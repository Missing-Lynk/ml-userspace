/**
 * @file hud.c
 * @brief HUD entry point: the menu, the BTFL OSD, and the OSD channel, over the DRM overlay plane.
 *
 * Binds the OSD channel (osd_channel): the mlm seams fed by ml-linkd - osd.sock for the MSP
 * canvases; the 0x09/0x11 status frames arrive through linkstate's telemetry.sock and are routed
 * back into the same callbacks. On the bench, tools/osd-replay feeds the same seams (no ml-linkd
 * needed). While the menu is closed, each OSD canvas is decoded
 * and rendered (btfl_osd) into the shared overlay. CENTER opens the menu (ui/menu over LVGL); BACK
 * steps one layer back and closes at the top level; a long BACK closes it outright. The menu and the
 * BTFL OSD share the one overlay plane and never draw at the same time.
 */
#include "channel/osd_channel.h"
#include "fb/drmoverlay.h"
#include "fb/surface.h"
#include "hal/board.h"
#include "hal/buzzer.h"
#include "hal/input.h"
#include "hal/telemetry.h"
#include "hud_state.h"
#include "osd/btfl_burn.h"
#include "osd/btfl_osd.h"
#include "services/linkcmd.h"
#include "services/linkstate.h"
#include "services/pipecmd.h"
#include "settings/settings.h"
#include "ui/display.h"
#include "ui/menu.h"
#include "ui/sysosd.h"
#include "ui/tempwarn.h"
#include "ui/tone.h"

#include "lvgl.h"

#include "../../ml-shared/mlm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LONGPRESS_MS    600    /* hold BACK this long to close the menu outright */
#define ALARM_PERIOD_MS 2000   /* low-voltage alarm chirp interval while active */
#define SYSOSD_PERIOD_MS 1000  /* System OSD telemetry refresh interval */
#define SRT_PERIOD_MS   1000   /* DVR subtitle-sidecar line interval while recording */
#define BIND_CHIRP_MS   600    /* buzzer chirp interval through the bind pair window */

#define TEMPWARN_HYST_C    5    /* the banner latches off this many deg C below the threshold */
#define TEMPWARN_CHIRP_MS  2000 /* overheat chirp interval while the banner is latched */

static volatile sig_atomic_t g_stop;

static void on_sigint(int s)
{
    (void) s;
    g_stop = 1;
}

/* A crash or abort (e.g. LVGL's out-of-memory assert) must never leave the buzzer PWM latched on:
 * silence it with an async-signal-safe raw write, then re-raise to die with the default action.
 */
static void on_fatal(int s)
{
    buzzer_panic_off();
    signal(s, SIG_DFL);
    raise(s);
}

/* Milliseconds from the monotonic clock: the LVGL tick source and the long-press timer. */
static uint32_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t) (now.tv_sec * 1000 + now.tv_nsec / 1000000);
}

typedef struct {
    surface_t     *fb;
    settings_t    *settings;
    hud_state_t    state;
    drm_overlay_t *drm;              /* DRM overlay-plane backend, or NULL */
    int            menu_available;   /* menu/LVGL are up (requires the DRM overlay) */
    int            prev_menu_open;   /* last observed menu_is_open(), for edge sync */
    int            back_held;        /* BACK is currently down */
    int            back_fired;       /* the long-press close already fired for this hold */
    uint32_t       back_down_ms;     /* when BACK went down */
    uint32_t       alarm_next_ms;    /* next low-voltage alarm check */
    uint32_t       sysosd_next_ms;   /* next System OSD telemetry refresh */
    uint32_t       srt_next_ms;      /* next DVR subtitle line (while recording, save_srt on) */
    int            burn_active;      /* DVR OSD burn-in gate is open (recording + dvr.record_osd) */
    uint32_t       rec_seen_ms;      /* when RECORDING was first observed (FlightTime base); 0 = not recording */
    int            rec_autostop_sent;  /* latch: the "stream lost" record-stop toggle was already sent */
    int            rec_autostart_sent; /* latch: the "stream up" auto-record start toggle was already sent */
    int            rec_is_auto;        /* the current recording was started by auto-record (not the button) */
    int            nosignal_sent;      /* latch: the "stream lost" no-signal-splash command was already sent */
    int            standby_asserted;   /* latch: the air-unit standby state was pushed for this link-up */
    int            power_asserted;     /* latch: the air-unit TX power was pushed for this link-up */
    int            channel_asserted;   /* latch: the saved RF channel was pushed for this link-up */
    int            camera_asserted;    /* latch: the saved camera/scale settings were pushed for this link-up */
    long           osd_frames;
    long           rendered;
    uint16_t       last_voltage_mV;  /* air-unit pack mV, from the 0x09/0x11 status frames */
    int            have_voltage;
    int            have_version;     /* saw a version frame (fw string + link metrics) */
    int            air_temp_c;       /* air-unit temperature (0x09 frame @98), whole deg C */
    int            have_air_temp;
    uint32_t       air_ts_us;        /* last :10000 header timestamp (us since air boot) */
    int            have_air_ontime;
    int            tempwarn_latched;  /* overheat banner state, with TEMPWARN_HYST_C hysteresis */
    int            tempwarn_shown;    /* last state pushed to the banner, for edge sync */
    uint32_t       tempwarn_next_ms;  /* next overheat chirp while latched */
} hud_ctx_t;

static void render_and_present(hud_ctx_t *h, const unsigned char *canvas, int len)
{
    /* btfl_osd_update returns the changed cells' rectangles (0 = nothing changed; -1 = whole surface
     * dirty). The backend rewrites only those pixels.
     */
    rect_t rects[128];
    int n = btfl_osd_update(h->fb, canvas, len, rects, (int) (sizeof(rects) / sizeof(rects[0])));
    if (n == 0) {
        return;
    }

    if (h->drm != NULL) {
        drm_overlay_present(h->drm, h->fb, rects, n);
        if (n < 0) {
            sysosd_invalidate();   /* a full BTFL present repacks the whole plane, clobbering the bar strip */
        }
    }
    h->rendered++;
}

static void on_osd(void *ctx, const unsigned char *canvas, int len)
{
    hud_ctx_t *h = ctx;
    h->osd_frames++;

    /* DVR OSD burn-in: forward changed cells to the pipeline while the gate is open. NOT behind
     * the visibility gates below - the recording keeps its OSD while the menu hides the on-screen
     * one, and independent of the msp_osd overlay setting (dvr.record_osd is its own intent).
     */
    if (h->burn_active) {
        btfl_burn_update(canvas, len);
    }

    if (!hud_btfl_visible(h->state)) {
        return;
    }

    /* The MSP OSD overlay setting can disable the BTFL OSD ("None"). On close the menu clears the
     * overlay, so nothing lingers when it is off.
     */
    if (strcmp(settings_get_string_in(h->settings, "goggle", "msp_osd", "BTFL"), "BTFL") != 0) {
        return;
    }

    render_and_present(h, canvas, len);
}

static void on_version(void *ctx, const osd_header_t *header, const osd_version_t *v)
{
    hud_ctx_t *h = ctx;
    h->air_ts_us = header->ts_us;
    h->have_air_ontime = 1;
    h->last_voltage_mV = v->voltage_mV;
    h->have_voltage = 1;
    h->have_version = 1;

    /* @98 is the air unit's own temperature in deg C. */
    h->air_temp_c = v->air_temp_c;
    h->have_air_temp = 1;
}

static void on_periodic(void *ctx, const osd_header_t *header, const osd_periodic_t *p)
{
    hud_ctx_t *h = ctx;
    h->air_ts_us = header->ts_us;
    h->have_air_ontime = 1;
    h->last_voltage_mV = p->voltage_mV;
    h->have_voltage = 1;
}

/* Push the dvr.resolution setting to the pipeline (MLM_CMD_DVR_RES latches it for the next
 * recording start). Sent just before every record-start toggle - the datagrams are ordered on
 * ctrl.sock, so even a freshly restarted pipeline has the format before the toggle arrives.
 */
static void send_dvr_format(hud_ctx_t *h)
{
    int height = 1080;
    int fps = 60;

    sscanf(settings_get_string_in(h->settings, "dvr", "resolution", "1080p 60fps"),
           "%dp %dfps", &height, &fps);
    pipecmd_set_dvr_res(height, fps);
}

/* Route a key edge to the menu. A key press also chirps the key tone unless it is disabled. When the
 * menu is closed only CENTER acts (it opens the menu); when open, keys navigate. BACK is timed: a
 * short press steps one layer back, a long press (handled in the loop) closes the menu.
 */
static void on_button(void *ctx, hud_button_t button, hud_button_edge_t edge)
{
    hud_ctx_t *h = ctx;
    if (!h->menu_available) {
        return;
    }

    if (edge == HUD_EDGE_DOWN && !settings_get_bool_in(h->settings, "goggle", "key_tones_off", 0)) {
        tone_beep();
    }

    /* Record works whether or not the menu is open (you record while flying), so it is handled before
     * the menu-open gate below. The pipeline is the source of truth: this just sends a toggle, and the
     * REC indicator follows the pipeline's reported state (see linkstate/sysosd). Recording only makes
     * sense with a live feed, so the toggle is ignored with no stream (there is nothing to capture, and
     * a running recording is auto-stopped on stream loss by the main loop).
     */
    if (button == HUD_BTN_RECORD && edge == HUD_EDGE_DOWN) {
        if (linkstate_is_airunit_connected()) {
            send_dvr_format(h);   /* a starting toggle records in the configured format */
            pipecmd_record_toggle();
            h->rec_is_auto = 0;   /* manual control: this recording is not auto-record's to stop */
        }
        return;
    }

    /* Bind a new air unit. Only while no air unit is connected, so a press can never re-pair mid-
     * flight (ml-linkd enforces the same gate).
     * Works with the menu open or closed, like record. ml-linkd reports progress back over the
     * link seam, which drives the System OSD "BIND" indicator and the buzzer cues
     * (bind_ui_tick). */
    if (button == HUD_BTN_BIND && edge == HUD_EDGE_DOWN) {
        if (!linkstate_is_airunit_connected() && !linkstate_is_binding()) {
            linkcmd_bind();
        }

        return;
    }

    if (!menu_is_open()) {
        if (button == HUD_BTN_CENTER && edge == HUD_EDGE_DOWN) {
            menu_open();
            fprintf(stderr, "hud: menu open\n");
        }

        return;
    }

    if (button == HUD_BTN_BACK) {
        if (edge == HUD_EDGE_DOWN) {
            h->back_held = 1;
            h->back_fired = 0;
            h->back_down_ms = now_ms();
        } else if (edge == HUD_EDGE_UP) {
            if (h->back_held && !h->back_fired) {
                menu_back();
            }
            h->back_held = 0;
        }

        return;
    }

    if (edge != HUD_EDGE_DOWN) {
        return;
    }

    switch (button) {
        case HUD_BTN_UP: {
            menu_up();
        } break;

        case HUD_BTN_DOWN: {
            menu_down();
        } break;

        case HUD_BTN_LEFT: {
            menu_left();
        } break;

        case HUD_BTN_RIGHT: {
            menu_right();
        } break;

        case HUD_BTN_CENTER: {
            menu_center();
        } break;

        default: {
        }
    }
}

/* Low-voltage alarm: while enabled and the per-cell voltage is below the threshold, chirp on each
 * check (its own ALARM_PERIOD_MS cadence).
 */
static void alarm_tick(hud_ctx_t *h, uint32_t now)
{
    if (h->drm == NULL || (int32_t) (now - h->alarm_next_ms) < 0) {
        return;
    }

    h->alarm_next_ms = now + ALARM_PERIOD_MS;
    if (!settings_get_bool_in(h->settings, "goggle", "low_voltage_alarm", 1)) {
        return;
    }

    telemetry_t telemetry;
    telemetry_read(&telemetry);
    if (!telemetry.have_battery || telemetry.cell_count <= 0) {
        return;
    }

    float per_cell = telemetry.pack_volts / telemetry.cell_count;
    float threshold = (float) atof(settings_get_string_in(h->settings, "goggle", "min_cell_voltage", "3.4V"));
    if (per_cell < threshold) {
        tone_beep();
    }
}

/* Bind UI: mirror ml-linkd's bind state onto the System OSD "BIND" indicator and the buzzer. While
 * binding, chirp on a slow cadence (the vendor beeps through the pair window); on completion, play
 * the success melody (the power-on chime) or a distinct failure cue, exactly once per bind. The
 * in-progress chirp respects key_tones_off; the result melody always plays (buzzer-volume gated), as
 * it is the answer to a deliberate action, not a key tone. */
static void bind_ui_tick(hud_ctx_t *h, uint32_t now)
{
    static int inited;
    static unsigned last_gen;
    static uint32_t next_chirp_ms;
    int binding = linkstate_is_binding();
    int ok = 0;
    unsigned gen;

    if (!inited) {
        last_gen = linkstate_bind_result(NULL);   /* ignore any result that predates the HUD */
        inited = 1;
    }

    if (h->drm != NULL) {
        sysosd_set_binding(binding);
    }

    if (binding) {
        if ((int32_t) (now - next_chirp_ms) >= 0) {
            next_chirp_ms = now + BIND_CHIRP_MS;
            if (!settings_get_bool_in(h->settings, "goggle", "key_tones_off", 0)) {
                tone_beep();
            }
        }
    } else {
        next_chirp_ms = now;   /* re-arm so the next bind's first chirp is immediate */
    }

    gen = linkstate_bind_result(&ok);
    if (gen != last_gen) {
        last_gen = gen;
        if (ok) {
            tone_success();
        } else {
            tone_fail();
        }
    }
}

/* DVR subtitle sidecar: while recording and the dvr.save_srt setting is on, send ml-pipeline one
 * telemetry line per second (pipecmd_srt_text); the pipeline stamps it against the recording's
 * video timeline and writes the .srt next to the .mp4. The line mirrors the vendor's subtitle
 * fields (Signal/CH/FlightTime/SBat/GBat/Bitrate/Distance, plus temperatures when Show
 * Temperature is on), sourced from the same caches the System OSD reads: linkstate for the RF
 * metrics, the :10000 status frames for the air-unit pack, hal/telemetry for the goggle-local
 * values. Unknown values render as 0, matching the vendor's zero-filled fields.
 */
static void srt_send_line(hud_ctx_t *h, int connected, uint32_t now)
{
    telemetry_t telemetry;
    telemetry_read(&telemetry);

    /* SNR -> 0..4 signal bars, the sysosd mapping: ~0 dB = 0, >= 20 dB = 4 */
    int snr = linkstate_snr_db();
    unsigned signal = 0;
    if (connected && snr != MLM_LINKINFO_NONE && snr > 0) {
        signal = snr >= 20 ? 4 : (unsigned) (snr / 5);
    }

    int channel = linkstate_channel();
    int distance = linkstate_distance_m();
    float sbat = (connected && h->have_voltage) ? (float) h->last_voltage_mV / 1000.0f : 0.0f;
    float gbat = telemetry.have_battery ? telemetry.pack_volts : 0.0f;
    unsigned bitrate = telemetry.have_bitrate ? (unsigned) (telemetry.bitrate_mbps + 0.5f) : 0;
    unsigned flight_s = h->rec_seen_ms ? (now - h->rec_seen_ms) / 1000 : 0;

    /* STYMode is the vendor's fast-mode flag; our equivalent is the band setting (Race = 1). */
    unsigned stymode =
        strcmp(settings_get_string_in(h->settings, "goggle", "band", "Normal"), "Race") == 0 ? 1 : 0;

    char line[192];
    int n = snprintf(line, sizeof line,
                     "Signal:%1u CH:%2u FlightTime:%4u SBat:%5.2f GBat:%5.2f Bitrate:%2uMbps Distance:%6um STYMode:%1u",
                     signal,
                     channel != MLM_LINKINFO_NONE ? (unsigned) channel : 0,
                     flight_s, sbat, gbat, bitrate,
                     distance != MLM_LINKINFO_NONE ? (unsigned) distance : 0, stymode);

    if (settings_get_bool_in(h->settings, "goggle", "show_temperature", 1) && n > 0
        && (size_t) n < sizeof line) {
        snprintf(line + n, sizeof line - (size_t) n, " AirTemp:%3u GndTemp:%3u",
                 (connected && h->have_air_temp) ? (unsigned) h->air_temp_c : 0,
                 telemetry.have_temp ? (unsigned) telemetry.temp_c : 0);
    }

    pipecmd_srt_text(line);
}

/* DVR subtitle cadence: latch the FlightTime base on the first loop that observes RECORDING and
 * clear it when the recording ends, so the count also survives a HUD restart mid-recording
 * (restarting from the reattach point). Not gated on the overlay: telemetry and linkstate work
 * headless, and the recording may have been auto-started.
 */
static void srt_tick(hud_ctx_t *h, int recording, int connected, uint32_t now)
{
    if (recording && h->rec_seen_ms == 0) {
        h->rec_seen_ms = now ? now : 1;
        h->srt_next_ms = now;   /* first line right away, then every SRT_PERIOD_MS */
    } else if (!recording) {
        h->rec_seen_ms = 0;
    }

    if (recording && (int32_t) (now - h->srt_next_ms) >= 0) {
        h->srt_next_ms = now + SRT_PERIOD_MS;
        if (settings_get_bool_in(h->settings, "dvr", "save_srt", 0)) {
            srt_send_line(h, connected, now);
        }
    }
}

/* DVR OSD burn-in gate: open while recording with dvr.record_osd on. On the rising edge the
 * pipeline's cell cache is cleared and the burn grid invalidated, so the next canvas re-sends
 * every occupied cell and the two sides restart in sync (also covers a setting toggled on
 * mid-recording). On the falling edge (setting toggled off mid-recording) the cache is cleared
 * so the still-running recording stops burning; a recording that simply ended needs nothing
 * (the pipeline clears its cache at record stop).
 */
static void burn_tick(hud_ctx_t *h, int recording)
{
    int active = recording && settings_get_bool_in(h->settings, "dvr", "record_osd", 0);

    if (active != h->burn_active) {
        pipecmd_osd_clear();
        if (active) {
            btfl_burn_invalidate();
        }
        h->burn_active = active;
    }
}

/* Auto record and auto stop, all keyed off ml-linkd's connection state - the single source of truth
 * for whether a stream is present:
 *   - stop a running recording the moment the stream drops (the pipeline would otherwise keep
 *     muxing a dead feed);
 *   - start one when the stream comes up if the DVR auto-record setting is on (and at once if it is
 *     switched on while a stream is already live);
 *   - turning auto-record off stops the recording it started (never a manual one).
 * Each command is a toggle, so all are latched + state-guarded to fire once. The auto-start latch
 * clears on disconnect, so a manual stop while still connected is respected - not overridden until
 * the link drops and comes back.
 */
static void record_policy_tick(hud_ctx_t *h, int connected, int recording, int state)
{
    int autorecord = settings_get_bool_in(h->settings, "dvr", "autostart", 0);

    if (recording && !connected && !h->rec_autostop_sent) {
        pipecmd_record_toggle();
        h->rec_autostop_sent = 1;
        h->rec_is_auto = 0;
    } else if (!recording) {
        h->rec_autostop_sent = 0;
    }

    if (autorecord && connected && linkstate_has_pipeline_state() && state == MLM_STATE_IDLE
        && !h->rec_autostart_sent) {
        send_dvr_format(h);        /* the auto-started recording uses the configured format */
        pipecmd_record_toggle();   /* idle-guarded: a toggle from idle starts recording */
        h->rec_autostart_sent = 1;
        h->rec_is_auto = 1;        /* auto-record owns this recording */
    }

    if (!connected) {
        h->rec_autostart_sent = 0;
    }

    /* The auto-off stop is level-checked and clears rec_is_auto after the toggle, so it fires once -
     * and also catches a recording that was still starting when the toggle flipped (it stops once
     * the pipeline reports RECORDING).
     */
    if (!autorecord && recording && h->rec_is_auto) {
        pipecmd_record_toggle();
        h->rec_is_auto = 0;
    }
}

/* Push the air-unit settings once per link-up; every latch clears on disconnect. The menu defaults
 * matter here: a user who never touches a row still needs standby armed and power pushed.
 * ml-linkd re-applies standby and power on its own session restarts, so re-asserting on every
 * link-up edge covers both. The channel, which
 * ml-linkd does not persist (without this every boot lands on its bring-up default), defaults to 0
 * - the Normal-band bring-up channel - until the user picks one; ml-linkd ignores a channel outside
 * the current band's valid set, leaving its own first-valid choice in place.
 */
static void assert_air_settings(hud_ctx_t *h, int connected)
{
    if (!connected) {
        h->standby_asserted = 0;
        h->power_asserted = 0;
        h->channel_asserted = 0;
        h->camera_asserted = 0;
        return;
    }

    if (!h->standby_asserted) {
        linkcmd_set_standby(settings_get_bool_in(h->settings, "air_unit", "standby", 1));
        h->standby_asserted = 1;
    }

    if (!h->power_asserted) {
        /* the stored value is the level label ("100 mW"); linkcmd maps it to mW */
        linkcmd_set_power(settings_get_string_in(h->settings, "air_unit", "power", "100 mW"));
        h->power_asserted = 1;
    }

    if (!h->channel_asserted) {
        int channel = settings_get_int_in(h->settings, "goggle", "channel", 0);
        if (channel >= 0) {
            linkcmd_select_channel((unsigned) channel);
        }

        h->channel_asserted = 1;
    }

    if (!h->camera_asserted) {
        /* a re-association resets the air's ISP to its association defaults; push the saved values */
        menu_camera_assert();
        h->camera_asserted = 1;
    }
}

/* No-signal splash: when the link drops during live view, ask the pipeline to park on the default
 * no-signal frame instead of holding the last decoded frame. Latched to fire once per drop; the
 * latch clears on reconnect and the pipeline resumes video on its own when frames return. Only in
 * live (IDLE) mode - playback owns the display itself.
 */
static void nosignal_tick(hud_ctx_t *h, int connected, int state)
{
    if (connected) {
        h->nosignal_sent = 0;
        return;
    }

    if (state != MLM_STATE_IDLE || !linkstate_has_pipeline_state() || h->nosignal_sent) {
        return;
    }

    pipecmd_show_nosignal();
    h->nosignal_sent = 1;

    /* Clear the last MSP OSD frame off the overlay once, so stale FC glyphs do not linger over the
     * no-signal splash. Only in the flying view (menu closed) - with the menu up the OSD is hidden
     * and the surface is the menu's to draw. btfl_osd_clear leaves the System OSD bar intact.
     */
    if (h->drm != NULL && hud_btfl_visible(h->state)) {
        rect_t rects[128];
        int n = btfl_osd_clear(h->fb, rects, (int) (sizeof(rects) / sizeof(rects[0])));
        if (n != 0) {
            drm_overlay_present(h->drm, h->fb, rects, n);
            if (n < 0) {
                sysosd_invalidate();   /* a full present repacks the whole plane, clobbering the bar strip */
            }
        }
    }
}

/* Air-unit overheat warning: compare the transmitted air temperature (0x09 frame @98) against the
 * air_unit.temp_warn_c threshold ("Off" parses to 0 = disabled) and drive the top-center banner.
 * The latch has TEMPWARN_HYST_C of hysteresis and clears on link-down, so a reconnect re-evaluates
 * fresh. This mirrors the vendor: the air sends only the raw temperature, the goggle applies the
 * threshold (stock: >105). While latched, a chirp sounds every TEMPWARN_CHIRP_MS.
 */
static void tempwarn_tick(hud_ctx_t *h, int connected, uint32_t now)
{
    if (h->drm == NULL) {
        return;
    }

    /* the stored value is the option label ("105°C"); atoi stops at the unit */
    int threshold = atoi(settings_get_string_in(h->settings, "air_unit", "temp_warn_c", "105°C"));
    if (threshold <= 0 || !connected || !h->have_air_temp) {
        h->tempwarn_latched = 0;
    } else if (h->air_temp_c >= threshold) {
        h->tempwarn_latched = 1;
    } else if (h->air_temp_c <= threshold - TEMPWARN_HYST_C) {
        h->tempwarn_latched = 0;
    }

    if (h->tempwarn_latched != h->tempwarn_shown) {
        tempwarn_set_active(h->tempwarn_latched);
        if (!h->tempwarn_latched) {
            btfl_osd_invalidate();   /* the banner erased the glyphs beneath it: full OSD redraw */
        }

        h->tempwarn_shown = h->tempwarn_latched;
    }

    if (h->tempwarn_latched && (int32_t) (now - h->tempwarn_next_ms) >= 0) {
        h->tempwarn_next_ms = now + TEMPWARN_CHIRP_MS;
        tone_beep();
    }
}

/* System OSD refresh, on its own SYSOSD_PERIOD_MS cadence. */
static void sysosd_tick(hud_ctx_t *h, uint32_t now)
{
    if (h->drm == NULL || (int32_t) (now - h->sysosd_next_ms) < 0) {
        return;
    }

    h->sysosd_next_ms = now + SYSOSD_PERIOD_MS;
    telemetry_t telemetry;
    telemetry_read(&telemetry);

    /* Air-unit values decoded from the :10000 status frames; blanked when the link is down so the
     * bar shows placeholders rather than stale readings. */
    int air_up = linkstate_is_airunit_connected();
    air_telem_t air = {
        .have_voltage = air_up && h->have_voltage,
        .voltage_mV = h->last_voltage_mV,
        .have_temp = air_up && h->have_air_temp,
        .temp_c = h->air_temp_c,
        .have_ontime = air_up && h->have_air_ontime,
        .ontime_s = h->air_ts_us / 1000000u,
    };

    sysosd_update(&telemetry, &air, h->settings);
}

/* Long BACK: fire the outright menu close once while the hold passes the threshold. */
static void back_longpress_tick(hud_ctx_t *h)
{
    if (h->menu_available && h->back_held && !h->back_fired && menu_is_open()
        && now_ms() - h->back_down_ms >= LONGPRESS_MS) {
        menu_close_all();
        h->back_fired = 1;
        fprintf(stderr, "hud: menu close (long back)\n");
    }
}

/* Sync the BTFL gate to the menu on any open/close edge, however it was triggered. */
static void menu_state_sync(hud_ctx_t *h)
{
    if (!h->menu_available) {
        return;
    }

    int open_now = menu_is_open();
    if (open_now == h->prev_menu_open) {
        return;
    }

    sysosd_set_menu_open(open_now);   /* solid bar background for context while the menu is up */
    if (open_now) {
        h->state = HUD_MENU_OPEN;
    } else {
        h->state = HUD_MENU_CLOSED;
        btfl_osd_invalidate();   /* the menu overwrote the surface: full OSD redraw next */
        sysosd_invalidate();     /* and repaint the bar over the coming full BTFL present */
        fprintf(stderr, "hud: menu closed\n");
    }

    h->prev_menu_open = open_now;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "  --btfl-font PATH  BTFL OSD glyph font PNG (default: HUD_MSP_FONT env, then search list)\n"
        "  --settings PATH   settings JSON file (default /usrdata/hud/settings.json)\n"
        "  --drm [PLANE]     present on the DRM overlay plane via ml-drmfd (default plane 38)\n"
        "  --menu-open       start with the menu OPEN\n"
        "  --frames N        exit after N OSD frames (default: run until idle/signal)\n"
        "  --idle-ms N       exit after N ms with no datagram, menu closed (default 3000; 0 = never)\n",
        p);
}

int main(int argc, char **argv)
{
    const char *btfl_font = NULL;
    const char *settings_path = "/usrdata/hud/settings.json";
    int use_drm = 0;
    uint32_t drm_plane = 38;
    int width = 1920, height = 1080;
    int start_open = 0;
    long max_frames = 0;
    int idle_ms = 3000;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--btfl-font") && i + 1 < argc) {
            btfl_font = argv[++i];
        } else if (!strcmp(argv[i], "--settings") && i + 1 < argc) {
            settings_path = argv[++i];
        } else if (!strcmp(argv[i], "--drm")) {
            use_drm = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                drm_plane = (uint32_t) atoi(argv[++i]);
            }
        } else if (!strcmp(argv[i], "--menu-open")) {
            start_open = 1;
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--idle-ms") && i + 1 < argc) {
            idle_ms = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    settings_t *settings = settings_open(settings_path);
    if (settings == NULL) {
        fprintf(stderr, "hud: settings alloc failed\n");
        return 1;
    }

    fprintf(stderr, "hud: settings %s\n", settings_path);

    if (btfl_osd_init(btfl_font) != 0) {
        fprintf(stderr, "hud: could not load BTFL OSD glyph font (--btfl-font or HUD_MSP_FONT)\n");
        settings_close(settings);
        return 1;
    }

    /* --drm sizes the surface from the plane; cells clear to transparent so lower layers show. The
     * menu (LVGL) needs the overlay too, so it is available only with --drm.
     */
    drm_overlay_t drm;
    int have_drm = 0;
    if (use_drm) {
        if (drm_overlay_open(&drm, drm_plane) != 0) {
            fprintf(stderr, "hud: cannot open DRM overlay\n");
            settings_close(settings);
            return 1;
        }

        have_drm = 1;
        width = drm.w;
        height = drm.h;
    }

    surface_t fb;
    if (surface_init(&fb, width, height) != 0) {
        fprintf(stderr, "hud: surface alloc failed\n");
        if (have_drm) {
            drm_overlay_close(&drm);
        }
        settings_close(settings);
        return 1;
    }

    /* With the overlay up, the bottom strip is the System OSD's; map the BTFL grid into the region
     * above it so the two never fight over the shared plane. Host/no-drm runs use the full height.
     */
    int btfl_h = use_drm ? height - OSD_BAR_HEIGHT : height;
    btfl_osd_configure(width, btfl_h, 0, 0, 0, 0);

    if (have_drm) {
        lv_init();
        lv_tick_set_cb(now_ms);
        if (ui_display_init(&drm) != 0) {
            fprintf(stderr, "hud: LVGL display init failed\n");
            lv_deinit();
            surface_free(&fb);
            drm_overlay_close(&drm);
            settings_close(settings);

            return 1;
        }

        menu_init(settings);
        menu_apply_persisted();   /* load the saved language + push brightness/buzzer to the hardware */
        sysosd_create(lv_screen_active());
        tempwarn_create(lv_screen_active());
    }

    hud_ctx_t h;
    memset(&h, 0, sizeof(h));
    h.fb = &fb;
    h.settings = settings;
    h.drm = have_drm ? &drm : NULL;
    h.menu_available = have_drm;
    h.state = HUD_MENU_CLOSED;

    if (have_drm && start_open) {
        menu_open();
        sysosd_set_menu_open(1);
        h.state = HUD_MENU_OPEN;
        h.prev_menu_open = 1;
    }

    int fd = osd_channel_open();
    if (fd < 0) {
        fprintf(stderr, "hud: bind %s failed\n", MLM_OSD_SOCK);
        if (have_drm) {
            menu_close();
            lv_deinit();
            ui_display_deinit();
        }

        surface_free(&fb);
        if (have_drm) {
            drm_overlay_close(&drm);
        }
        settings_close(settings);

        return 1;
    }

    fprintf(stderr, "hud: listening on %s, menu %s, surface %dx%d, target=%s\n",
            MLM_OSD_SOCK, h.state == HUD_MENU_OPEN ? "OPEN" : "CLOSED", width, height,
            have_drm ? "drm-overlay" : "(none)");

    int input_fd = input_open();
    if (input_fd < 0) {
        fprintf(stderr, "hud: no keypad (%s), buttons disabled\n", board_current()->input_device);
    }

    /* Air-unit link presence, from ml-linkd's telemetry seam, gates the menu's Air Unit section. */
    int link_fd = linkstate_open();
    if (link_fd < 0) {
        fprintf(stderr, "hud: no link seam (ml-linkd telemetry.sock); Air Unit stays dimmed\n");
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    /* Never leave the buzzer latched: on a graceful exit atexit silences it; on a crash/abort the
     * fatal-signal handler does (before re-raising). */
    atexit(buzzer_panic_off);
    signal(SIGABRT, on_fatal);
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGILL, on_fatal);
    signal(SIGFPE, on_fatal);

    tone_init();

    const osd_channel_cb_t cb = { on_osd, on_version, on_periodic };

    /* ml-linkd routes the 0x09/0x11 status frames to telemetry.sock (linkstate's socket), not
     * osd.sock; route them into the same callbacks so voltage/version keep flowing in mlm mode.
     */
    linkstate_set_osd_cb(&cb, &h);
    const int poll_ms = have_drm ? 30 : 200;   /* short when LVGL is up so the menu stays responsive */
    int idle_accum = 0;
    while (!g_stop) {
        if (have_drm) {
            lv_timer_handler();
        }

        int r = osd_channel_poll(fd, &cb, &h, poll_ms);
        if (r < 0) {
            break;
        }

        if (input_fd >= 0) {
            input_drain(input_fd, on_button, &h);
        }

        /* drain link/telemetry datagrams; updates air-unit + pipeline state */
        linkstate_poll(link_fd);

        if (h.drm != NULL) {
            drm_overlay_extern_refresh(h.drm, linkstate_is_video_live());
        }

        /* ml-linkd's connection state is the single source of truth for whether a stream is
         * present; every policy below keys off this one read. */
        int state = linkstate_pipeline_state();
        int recording = (state == MLM_STATE_RECORDING);
        int connected = linkstate_is_airunit_connected();

        record_policy_tick(&h, connected, recording, state);
        assert_air_settings(&h, connected);
        nosignal_tick(&h, connected, state);

        if (have_drm) {
            sysosd_set_recording(recording);

            /* track the playback scrubber at loop rate, not the 1 s OSD cadence */
            menu_playback_tick();
            menu_channel_tick();   /* pick up channel scans while the channel grid is shown */
        }

        uint32_t now = now_ms();
        bind_ui_tick(&h, now);   /* BIND indicator + bind buzzer cues off ml-linkd's state */
        alarm_tick(&h, now);
        srt_tick(&h, recording, connected, now);
        burn_tick(&h, recording);
        sysosd_tick(&h, now);
        tempwarn_tick(&h, connected, now);
        back_longpress_tick(&h);
        menu_state_sync(&h);

        if (r == 0) {
            idle_accum += poll_ms;
            if (idle_ms > 0 && !menu_is_open() && idle_accum >= idle_ms) {
                break;
            }

            continue;
        }

        idle_accum = 0;
        if (max_frames > 0 && h.osd_frames >= max_frames) {
            break;
        }
    }

    osd_channel_close(fd);
    linkstate_close(link_fd);
    input_close(input_fd);
    tone_shutdown();

    fprintf(stderr, "hud: %ld OSD frame(s) received, %ld rendered\n", h.osd_frames, h.rendered);
    if (h.have_voltage) {
        fprintf(stderr, "hud: last telemetry: %u mV%s\n", h.last_voltage_mV,
                h.have_version ? " (+link metrics)" : "");
    }

    if (have_drm) {
        menu_close();
        lv_deinit();
        ui_display_deinit();
    }

    surface_free(&fb);

    if (have_drm) {
        drm_overlay_close(&drm);
    }

    settings_close(settings);
    return h.rendered > 0 ? 0 : 3;
}
