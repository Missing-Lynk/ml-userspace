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
#include "hal/input.h"
#include "hal/telemetry.h"
#include "hud_state.h"
#include "osd/btfl_osd.h"
#include "services/linkstate.h"
#include "services/pipecmd.h"
#include "settings/settings.h"
#include "ui/display.h"
#include "ui/menu.h"
#include "ui/sysosd.h"
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

static volatile sig_atomic_t g_stop;

static void on_sigint(int s)
{
    (void) s;
    g_stop = 1;
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
    long           osd_frames;
    long           rendered;
    uint16_t       last_voltage_mV;
    int            have_voltage;
    int            have_version;     /* saw a version frame (fw string + link metrics) */
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
    (void) header;
    h->last_voltage_mV = v->voltage_mV;
    h->have_voltage = 1;
    h->have_version = 1;
}

static void on_periodic(void *ctx, const osd_header_t *header, const osd_periodic_t *p)
{
    hud_ctx_t *h = ctx;
    (void) header;
    h->last_voltage_mV = p->voltage_mV;
    h->have_voltage = 1;
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
        tone_beep(now_ms());
    }

    /* Record works whether or not the menu is open (you record while flying), so it is handled before
     * the menu-open gate below. The pipeline is the source of truth: this just sends a toggle, and the
     * REC indicator follows the pipeline's reported state (see linkstate/sysosd).
     */
    if (button == HUD_BTN_RECORD && edge == HUD_EDGE_DOWN) {
        pipecmd_record_toggle();
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
 * check.
 */
static void alarm_check(hud_ctx_t *h)
{
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
        tone_beep(now_ms());
    }
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
        if (have_drm) {
            sysosd_set_recording(linkstate_pipeline_state() == MLM_STATE_RECORDING);

            /* track the playback scrubber at loop rate, not the 1 s OSD cadence */
            menu_playback_tick();
        }

        /* Beeps: end any finished key tone; run the low-voltage alarm on its own cadence. */
        uint32_t now = now_ms();
        tone_tick(now);
        if (have_drm && (int32_t) (now - h.alarm_next_ms) >= 0) {
            h.alarm_next_ms = now + ALARM_PERIOD_MS;
            alarm_check(&h);
        }

        if (have_drm && (int32_t) (now - h.sysosd_next_ms) >= 0) {
            h.sysosd_next_ms = now + SYSOSD_PERIOD_MS;
            telemetry_t telemetry;
            telemetry_read(&telemetry);
            sysosd_update(&telemetry, h.settings);
            menu_refresh_link();   /* keep the Air Unit entry's dim in step with the live link */
        }

        /* Long BACK: fire once while held past the threshold. */
        if (have_drm && h.back_held && !h.back_fired && menu_is_open()
            && now_ms() - h.back_down_ms >= LONGPRESS_MS) {
            menu_close_all();
            h.back_fired = 1;
            fprintf(stderr, "hud: menu close (long back)\n");
        }

        /* Sync the BTFL gate to the menu on any open/close edge, however it was triggered. */
        if (have_drm) {
            int open_now = menu_is_open();
            if (open_now != h.prev_menu_open) {
                sysosd_set_menu_open(open_now);   /* solid bar background for context while the menu is up */
                if (open_now) {
                    h.state = HUD_MENU_OPEN;
                } else {
                    h.state = HUD_MENU_CLOSED;
                    btfl_osd_invalidate();   /* the menu overwrote the surface: full OSD redraw next */
                    sysosd_invalidate();     /* and repaint the bar over the coming full BTFL present */
                    fprintf(stderr, "hud: menu closed\n");
                }
                h.prev_menu_open = open_now;
            }
        }

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
