/** @file player.c @brief See player.h. */
#include "player.h"
#include "theme.h"
#include "sysosd.h"

#include "linkstate.h"
#include "pipecmd.h"
#include "recordings.h"

#include <stdio.h>

/* Playback transport bar (matches the libre menu's player). */
#define PLAYBACK_PAD         28
#define PLAYBACK_TIME_WIDTH  250   /* fixed width for "M:SS / M:SS" so the bar does not shift */
#define PLAYBACK_STATE_WIDTH 200   /* fixed width: fits the play/pause icon and "<< 8x" scrub label */
#define PLAYBACK_BAR_THICK   16
#define PLAYBACK_FONT        (&lv_font_montserrat_32)   /* title + time */
#define PLAYBACK_ICON_FONT   (&lv_font_montserrat_48)   /* the play/pause indicator */

/* Fast-forward / rewind ladder (matches the libre player): LEFT/RIGHT step the index, CENTER drops
 * back to 1x. The pipeline realises each speed as a single rate seek (pipecmd_playback_speed). */
static const int PLAYER_SPEEDS[] = { -8, -4, -2, 1, 2, 4, 8 };
#define PLAYER_SPEED_COUNT  ((int) (sizeof PLAYER_SPEEDS / sizeof PLAYER_SPEEDS[0]))
#define PLAYER_SPEED_NORMAL 3   /* index whose value is 1x (normal play) */

#define PLAYER_LOAD_TIMEOUT_MS 8000

static const player_host_t *g_host;

/* Playback player overlay: a selected recording plays on the pipeline while this bar shows a live
 * progress bar + controls. Keys route straight to playback commands while it is open. */
static lv_obj_t *g_player_box;
static lv_obj_t *g_player_bar;
static lv_obj_t *g_player_time;
static lv_obj_t *g_player_state;           /* play/pause indicator */
static int       g_player_open;
static int       g_player_seen_playing;    /* saw MLM_STATE_PLAYBACK once (guards the auto-close) */
static int       g_return_focus = -1;      /* recordings row to re-focus after playback (-1 = none) */
static int       g_player_speed_index = PLAYER_SPEED_NORMAL;
static char      g_player_path[512];       /* the playing clip, for replay after end-of-clip */
static unsigned  g_player_dur_ms;          /* clip length from the MP4 header; the stable scrubber
                                            * total (the pipeline's live duration grows for
                                            * fragmented clips, so it is only a fallback) */
static char      g_player_name[64];        /* selected clip's display name (kept for the reveal) */

/* Loading phase: after a clip is picked the menu stays up with a spinner over the selected row, and
 * navigation is frozen, until the pipeline reports the first frame is on screen (then the menu is
 * hidden and the transport bar shown). This hides the ~1 s decoder warm-up behind the list. */
static int       g_player_loading;
static lv_obj_t *g_loading_spinner;
static uint32_t  g_loading_deadline_ms;    /* lv_tick deadline to stop waiting for the first frame */

void player_init(const player_host_t *host)
{
    g_host = host;
}

/* mm:ss for the player's time readout. */
static void fmt_ms(uint32_t ms, char *out, size_t n)
{
    uint32_t s = ms / 1000;
    snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

/* Refresh the transport bar from the pipeline's playback telemetry (linkstate): progress bar, time
 * readout, play/pause icon. Auto-closes once playback ends on its own (EOS -> back to live). */
static void player_update(void)
{
    if (!g_player_open) {
        return;
    }

    int paused = 0;
    unsigned pos = 0, dur = 0;
    int playing = linkstate_playback(&paused, &pos, &dur);
    if (playing) {
        g_player_seen_playing = 1;
    } else if (g_player_seen_playing) {
        /* pipeline left playback without us asking (it died, or a clip failed to start): fall back to
         * the list. End-of-clip does NOT reach here - the pipeline holds MLM_STATE_PLAYBACK there. */
        player_close(0);
        return;
    }

    int ended = linkstate_playback_ended();

    /* Prefer the header-parsed length: the pipeline's live duration grows for fragmented clips. */
    if (g_player_dur_ms > 0) {
        dur = g_player_dur_ms;
    }

    /* clamp: the reported position can overshoot the header length by a frame */
    if (dur > 0 && pos > dur) {
        pos = dur;
    }

    char cur[16], tot[16], line[40];
    fmt_ms(pos, cur, sizeof cur);
    if (ended) {
        /* clip finished: bar completely full. Show "M:SS / M:SS" when the length is known. */
        if (dur > 0) {
            lv_bar_set_range(g_player_bar, 0, (int32_t) dur);
            lv_bar_set_value(g_player_bar, (int32_t) dur, LV_ANIM_OFF);
            fmt_ms(dur, tot, sizeof tot);
            snprintf(line, sizeof line, "%s / %s", cur, tot);
        } else {
            lv_bar_set_range(g_player_bar, 0, 1);
            lv_bar_set_value(g_player_bar, 1, LV_ANIM_OFF);
            snprintf(line, sizeof line, "%s", cur);
        }
    } else if (dur > pos) {
        /* known length: fill the bar and show "M:SS / M:SS" */
        lv_bar_set_range(g_player_bar, 0, (int32_t) dur);
        lv_bar_set_value(g_player_bar, (int32_t) pos, LV_ANIM_OFF);
        fmt_ms(dur, tot, sizeof tot);
        snprintf(line, sizeof line, "%s / %s", cur, tot);
    } else {
        /* unknown length: empty bar, count up in the label (matches libre) */
        lv_bar_set_range(g_player_bar, 0, 1);
        lv_bar_set_value(g_player_bar, 0, LV_ANIM_OFF);
        snprintf(line, sizeof line, "%s", cur);
    }
    lv_label_set_text(g_player_time, line);

    if (ended) {
        lv_label_set_text(g_player_state, LV_SYMBOL_STOP);   /* clip finished: CENTER replays */
    } else if (g_player_speed_index != PLAYER_SPEED_NORMAL) {
        /* scrubbing: show "<< Nx" / ">> Nx" in place of the play/pause icon */
        int speed = PLAYER_SPEEDS[g_player_speed_index];
        const char *arrow = (speed < 0) ? LV_SYMBOL_PREV : LV_SYMBOL_NEXT;
        lv_label_set_text_fmt(g_player_state, "%s %dx", arrow, (speed < 0) ? -speed : speed);
    } else {
        lv_label_set_text(g_player_state, paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
    }
}

/* Tear down the loading spinner (safe to call when none is showing). */
static void loading_spinner_clear(void)
{
    if (g_loading_spinner != NULL) {
        lv_obj_delete(g_loading_spinner);   /* also deletes its rotation animation */
        g_loading_spinner = NULL;
    }
}

/* Rotate the loading arc. Driven by a linear-path animation, so the spin is constant-speed. */
static void loading_spin_cb(void *arc, int32_t angle)
{
    lv_arc_set_rotation(arc, angle);
}

/* A smooth constant-speed spinner centred over @p row: a fixed-length arc rotated linearly. Built
 * from lv_arc rather than lv_spinner, whose start/end-angle easing pulses the arc length and reads
 * as a stutter. Stored in g_loading_spinner. */
static void loading_spinner_show(lv_obj_t *row)
{
    lv_obj_t *arc = lv_arc_create(g_host->content());
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);           /* indicator only, no drag knob */
    lv_obj_add_flag(arc, LV_OBJ_FLAG_IGNORE_LAYOUT);        /* overlaid, not in the list flow */
    lv_obj_set_size(arc, 48, 48);
    lv_arc_set_bg_angles(arc, 0, 360);                      /* faint full ring */
    lv_arc_set_angles(arc, 0, 70);                          /* bright rotating segment */
    lv_obj_set_style_arc_width(arc, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, COLOR_TEXT_DIM, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_align_to(arc, row, LV_ALIGN_CENTER, 0, 0);

    lv_anim_t a;
    lv_anim_init(&a);   /* default path is linear: constant-speed, seamless 360 loop */
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, loading_spin_cb);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    g_loading_spinner = arc;
}

void player_start(int row_index, const char *name)
{
    recordings_path(name, g_player_path, sizeof g_player_path);
    snprintf(g_player_name, sizeof g_player_name, "%s", name);
    pipecmd_playback_play(g_player_path);
    g_return_focus = row_index;
    g_player_seen_playing = 0;
    g_player_speed_index = PLAYER_SPEED_NORMAL;   /* every clip starts at 1x */
    g_player_dur_ms = recordings_duration_ms(g_player_path);   /* stable total for the scrubber */

    loading_spinner_clear();
    lv_obj_t *row = lv_obj_get_child(g_host->content(), row_index);
    if (row != NULL) {
        loading_spinner_show(row);
    }

    g_player_loading = 1;
    g_loading_deadline_ms = lv_tick_get() + PLAYER_LOAD_TIMEOUT_MS;
}

/* Reveal the transport bar once the clip is on screen: hide the menu + System OSD and build the
 * overlay. Keys are then routed to playback commands. Modelled on the libre transport bar. */
static void player_reveal(void)
{
    const char *name = g_player_name;

    loading_spinner_clear();
    g_player_loading = 0;

    lv_group_remove_all_objs(g_host->group());
    if (g_host->menu_root() != NULL) {
        lv_obj_add_flag(g_host->menu_root(), LV_OBJ_FLAG_HIDDEN);   /* reveal the video underneath */
    }
    sysosd_set_visible(0);                             /* playback shows only the transport bar */

    /* Bottom transport overlay: [ title + (time | progress) column ] [ big play/pause icon ]. */
    g_player_box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_player_box);
    lv_obj_remove_flag(g_player_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(g_player_box, lv_pct(100));
    lv_obj_set_height(g_player_box, LV_SIZE_CONTENT);
    lv_obj_align(g_player_box, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(g_player_box, COLOR_OSD, 0);
    lv_obj_set_style_bg_opa(g_player_box, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(g_player_box, PLAYBACK_PAD, 0);
    lv_obj_set_flex_flow(g_player_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_player_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_player_box, PLAYBACK_PAD, 0);

    lv_obj_t *column = lv_obj_create(g_player_box);
    lv_obj_remove_style_all(column);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(column, 1);   /* take the width left of the icon */
    lv_obj_set_height(column, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column, PLAYBACK_PAD / 2, 0);

    lv_obj_t *title = lv_label_create(column);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);   /* ellipsise long names */
    lv_obj_set_style_text_color(title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, PLAYBACK_FONT, 0);
    lv_label_set_text(title, name);

    lv_obj_t *row = lv_obj_create(column);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);

    g_player_time = lv_label_create(row);
    lv_obj_set_width(g_player_time, PLAYBACK_TIME_WIDTH);   /* fixed so the bar doesn't shift */
    lv_obj_set_style_text_align(g_player_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(g_player_time, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(g_player_time, PLAYBACK_FONT, 0);

    g_player_bar = lv_bar_create(row);
    lv_obj_set_flex_grow(g_player_bar, 1);
    lv_obj_set_height(g_player_bar, PLAYBACK_BAR_THICK);
    lv_obj_set_style_bg_color(g_player_bar, COLOR_TEXT_DIM, 0);            /* track */
    lv_obj_set_style_bg_color(g_player_bar, COLOR_ACCENT, LV_PART_INDICATOR);

    g_player_state = lv_label_create(g_player_box);
    lv_obj_set_width(g_player_state, PLAYBACK_STATE_WIDTH);
    lv_obj_set_style_text_align(g_player_state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(g_player_state, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(g_player_state, PLAYBACK_ICON_FONT, 0);

    g_player_open = 1;
    player_update();
}

/* Tear the transport bar down and null its objects. */
static void destroy_bar(void)
{
    if (g_player_box != NULL) {
        lv_obj_delete(g_player_box);
        g_player_box = NULL;
    }

    g_player_bar = NULL;
    g_player_time = NULL;
    g_player_state = NULL;
    g_player_open = 0;
}

void player_close(int return_live)
{
    if (!g_player_open) {
        return;
    }

    if (return_live) {
        pipecmd_playback_stop();   /* return the pipeline to the live stream */
    }

    destroy_bar();

    sysosd_set_visible(1);
    if (g_host->menu_root() != NULL) {
        lv_obj_remove_flag(g_host->menu_root(), LV_OBJ_FLAG_HIDDEN);   /* bring the menu back over the video */
    }

    if (g_host->menu_open()) {
        g_host->restore_list(g_return_focus);   /* re-focus the clip that was played */
    }
}

void player_cancel_loading(void)
{
    if (!g_player_loading) {
        return;
    }

    pipecmd_playback_stop();
    loading_spinner_clear();
    g_player_loading = 0;
}

void player_on_menu_closing(void)
{
    if (g_player_loading) {
        /* The spinner is a child of the content pane and dies with it; drop the reference here so we
         * do not double-free it. */
        pipecmd_playback_stop();
        g_loading_spinner = NULL;
        g_player_loading = 0;
    }

    if (g_player_open) {
        pipecmd_playback_stop();
        destroy_bar();   /* the bar lives on the screen, not the menu: delete it explicitly */
        sysosd_set_visible(1);
    }
}

/* Drive the loading phase: reveal the transport bar once the first frame is up, or give up if the
 * clip never renders (empty/aborted recording, or a decode that timed out). */
static void player_loading_tick(void)
{
    if (linkstate_playback_rendering()) {
        player_reveal();
        return;
    }

    /* No frame will come from an empty clip (it EOSes without rendering); do not spin forever. */
    if (linkstate_playback_ended() || (int32_t) (lv_tick_get() - g_loading_deadline_ms) >= 0) {
        player_cancel_loading();
    }
}

void player_tick(void)
{
    if (g_player_loading) {
        player_loading_tick();   /* waiting for the first frame: reveal the bar or give up */
        return;
    }

    player_update();   /* advance the playback bar + auto-close on end (no-op unless open) */
}

int player_is_open(void)
{
    return g_player_open;
}

int player_is_loading(void)
{
    return g_player_loading;
}

/* Select a speed-ladder index (clamped), push it to the pipeline, and refresh the label. */
static void player_set_speed(int index)
{
    if (index < 0) {
        index = 0;
    }

    if (index > PLAYER_SPEED_COUNT - 1) {
        index = PLAYER_SPEED_COUNT - 1;
    }

    g_player_speed_index = index;
    pipecmd_playback_speed(PLAYER_SPEEDS[index]);
    player_update();
}

void player_key_left(void)
{
    player_set_speed(g_player_speed_index - 1);   /* step toward rewind */
}

void player_key_right(void)
{
    player_set_speed(g_player_speed_index + 1);   /* step toward fast-forward */
}

void player_key_center(void)
{
    /* At end-of-clip, CENTER replays from the start. */
    if (linkstate_playback_ended()) {
        g_player_speed_index = PLAYER_SPEED_NORMAL;
        pipecmd_playback_play(g_player_path);
        return;
    }

    /* While scrubbing, CENTER drops back to 1x; otherwise it toggles pause/resume. */
    if (g_player_speed_index != PLAYER_SPEED_NORMAL) {
        player_set_speed(PLAYER_SPEED_NORMAL);
        return;
    }

    int paused = 0;
    linkstate_playback(&paused, NULL, NULL);
    if (paused) {
        pipecmd_playback_resume();
    } else {
        pipecmd_playback_pause();
    }
}
