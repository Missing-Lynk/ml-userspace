/** @file menu_channel.c @brief See menu_channel.h. */
#include "menu_channel.h"
#include "channel_label.h"
#include "theme.h"

#include "i18n.h"
#include "linkcmd.h"
#include "linkstate.h"

#include "lvgl.h"

#include "../../../ml-shared/mlm.h"

#include <stdint.h>

/* channel grid: 4 columns, up to ceil(19/4) rows; the descriptors must outlive the object. */
#define CHAN_COLS          4
#define CHAN_MAX_ROWS      5
#define CHAN_REF_ROWS      4   /* the Race band's 16 channels: the tile size every band matches */
#define CHAN_TILE_GAP      16  /* column gap between tiles (matches the menu's MENU_ROW_GAP) */
#define COLOR_CHAN_ACTIVE  lv_color_hex(0x46D17B)   /* active-channel border (System OSD green) */

static const menu_channel_host_t *g_host;

/* the grid needs one FR row per tile row plus a content-sized row for the Refresh button */
static int32_t   g_chan_cols[CHAN_COLS + 1];
static int32_t   g_chan_rows[CHAN_MAX_ROWS + 2];
static lv_obj_t *g_chan_tiles[MLM_SCAN_MAX_CH];    /* one focusable tile per shown (valid) channel */
static uint8_t   g_chan_tile_idx[MLM_SCAN_MAX_CH]; /* the table index each tile stands for */
static int       g_chan_tile_count;                /* tiles currently built (0 = a hint is shown) */
static unsigned  g_chan_scan_gen;                  /* scan generation the tiles were built from */
static int       g_chan_scanning;                  /* a sweep was requested and no result has landed yet */
static int       g_chan_active_shown = -1;         /* channel index the active border is currently on */
static lv_obj_t *g_chan_refresh;                   /* the Refresh button below the grid, NULL while a hint shows */
static lv_obj_t *g_chan_spinner;                   /* scanning indicator inside the button, hidden when idle */

static void chan_set_scanning_indicator(int on);

void menu_channel_init(const menu_channel_host_t *host)
{
    g_host = host;
}

void menu_channel_reset(void)
{
    g_chan_tile_count = 0;   /* the content pane is gone; drop the dangling tile pointers */
    g_chan_scan_gen = 0;
    g_chan_active_shown = -1;
    g_chan_refresh = NULL;
    g_chan_spinner = NULL;
}

void menu_channel_request_scan(void)
{
    linkcmd_request_scan();
    g_chan_scanning = 1;
    chan_set_scanning_indicator(1);   /* show the spinner if the button is on screen */
}

/* Bucket a measured per-channel SNR exactly as the vendor's scan handler does
 * (AR_MID_RX_WIRELESS_GET_SCAN_RESULT_IMPL, ar_lowdelay-full.txt:58402-58411): an unsigned compare
 * chain on the RAW linear Get1V1Info value, with no scaling and no dB conversion. The dB form
 * (10*log10(raw/36)) is the vendor's OSD readout only and never reaches these tiles - bucketing the
 * rounded dB instead would quantise the thresholds. Returns 1..4 (hue 30 = orange .. 120 = green).
 *
 * The vendor's strength-0 case (red) is deliberately not produced here: it is the absence of a
 * measurement, not a bad channel, so set_tile_signal greys it. Red would otherwise paint the whole
 * grid whenever the air unit is down, and would read the same as a weak-but-usable channel. */
static int tile_snr_bucket(int snr_raw)
{
    if (snr_raw >= 1100) {
        return 4;
    }

    if (snr_raw >= 600) {
        return 3;
    }

    if (snr_raw >= 160) {
        return 2;
    }

    return 1;
}

/* Paint a signal line with a dB reading: the WIFI glyph plus the value, hue = bucket * 30
 * (orange .. green). The two callers differ only in where the dB and its bucket come from. */
static void paint_signal_db(lv_obj_t *label, int db, int bucket)
{
    lv_label_set_text_fmt(label, "%s %ddB", LV_SYMBOL_WIFI, db);
    lv_obj_set_style_text_color(label,
                               lv_color_hsv_to_rgb((uint16_t)(bucket * 30), 80, 95), 0);
}

/* The tile's signal line: the WIFI glyph plus the channel's SNR in dB, coloured by the vendor's
 * bucket (hue = bucket * 30, orange .. green) so a channel reads at a glance. This is LINK quality,
 * not ambient noise: the sweep retunes to each channel and the air unit follows, so the value is the
 * SNR actually achievable there, and a low one means that channel is congested.
 *
 * A channel with no reading - unmeasured (no reply) or the chip reporting no lock - shows a grey
 * placeholder. Grey means "no link quality known here", which is not the same claim as a measured
 * bad channel; with the air unit down every channel reads that way.
 */
static void set_tile_signal(lv_obj_t *label, const struct mlm_scan_chan *ch)
{
    if (ch->snr_raw <= 0) {   /* MLM_SCAN_RAW_NOLOCK / _RAW_NONE: no reading to colour */
        lv_label_set_text_fmt(label, "%s --", LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(label, COLOR_TEXT_DIM, 0);
        return;
    }

    paint_signal_db(label, ch->snr_db, tile_snr_bucket(ch->snr_raw));
}

/* The live-SNR bucket, in the raw bucket's colour scale but keyed on dB: MLM_T_LINKINFO carries only
 * dB (no raw), so the active channel's pre-sweep reading is bucketed against the dB equivalents of the
 * raw thresholds (raw 1100/600/160 = ~15/12/6 dB). Returns 1..4 (hue 30 orange .. 120 green). */
static int snr_db_bucket(int db)
{
    if (db >= 15) {
        return 4;
    }

    if (db >= 12) {
        return 3;
    }

    if (db >= 6) {
        return 2;
    }

    return 1;
}

/* Set a tile's signal line, preferring the live link SNR for the currently tuned channel. The sweep
 * has not measured any channel yet when the grid first shows, but the active channel's SNR is already
 * known from MLM_T_LINKINFO (no retune needed), so show it there instead of the "--" placeholder. Any
 * channel the sweep has measured (snr_raw > 0) uses that reading. */
static void tile_apply_signal(lv_obj_t *label, const struct mlm_scan_chan *ch)
{
    int db = linkstate_snr_db();

    if (ch->snr_raw <= 0 && ch->index == linkstate_channel() && db != MLM_LINKINFO_NONE) {
        paint_signal_db(label, db, snr_db_bucket(db));
        return;
    }

    set_tile_signal(label, ch);
}

/* A green border marks the channel the RX is currently tuned to. */
static void set_tile_active(lv_obj_t *tile, int active)
{
    lv_obj_set_style_border_width(tile, active ? 3 : 0, 0);
    lv_obj_set_style_border_color(tile, COLOR_CHAN_ACTIVE, 0);
}

/* @brief Paint the active border on @p idx and nothing else. */
static void chan_show_active(int idx)
{
    g_chan_active_shown = idx;
    for (int s = 0; s < g_chan_tile_count; s++) {
        set_tile_active(g_chan_tiles[s], g_chan_tile_idx[s] == idx);
    }
}

/* CENTER on a tile: tune the local RX to it. The air unit follows over its own management link, so
 * nothing is sent to it and the session is not re-established. The retune is async and can fail (an
 * out-of-band index is refused by the chip), so the border is moved optimistically for feedback and
 * menu_channel_refresh reconciles it against ml-linkd's live channel ~1 s later - a select that did
 * not take reverts itself rather than leaving the UI claiming a channel the RX is not on.
 *
 * Persist the pick so it survives a reboot (hud.c re-asserts it on the next link-up), the same
 * contract as the air-unit power/bitrate/standby rows. ml-linkd holds the band's valid set and
 * rejects a channel outside it, so a value saved under one band cannot retune the chip off a
 * different one - it just falls back to the band's first channel.
 */
static void chan_tile_clicked(lv_event_t *event)
{
    int idx = (int) (intptr_t) lv_event_get_user_data(event);

    linkcmd_select_channel((unsigned) idx);
    g_host->persist_channel(idx);
    chan_show_active(idx);
}

/* One channel tile: the channel label (big: "CH<idx>" + raceband), the frequency, and the signal
 * line. Child order is fixed (0 label, 1 freq, 2 signal) so the refresh tick can update the signal
 * in place.
 */
static lv_obj_t *make_channel_tile(const struct mlm_scan_chan *ch)
{
    char label[24];

    lv_obj_t *tile = lv_button_create(g_host->content());
    lv_obj_add_event_cb(tile, chan_tile_clicked, LV_EVENT_CLICKED, (void *) (intptr_t) ch->index);
    lv_obj_remove_style_all(tile);
    lv_obj_add_style(tile, g_host->style_item(), 0);
    lv_obj_add_style(tile, g_host->style_item_focused(), LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    channel_label(label, sizeof label, ch->index);
    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_28, 0);
    lv_label_set_text(name, label);

    lv_obj_t *freq = lv_label_create(tile);
    lv_obj_set_style_text_font(freq, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(freq, COLOR_TEXT_DIM, 0);
    lv_label_set_text_fmt(freq, "%u MHz", ch->freq_mhz);

    lv_obj_t *sig = lv_label_create(tile);
    lv_obj_set_style_text_font(sig, &lv_font_montserrat_24, 0);
    tile_apply_signal(sig, ch);

    return tile;
}

/* @brief Show or hide the Refresh button's spinner. No-op unless the button is on screen. */
static void chan_set_scanning_indicator(int on)
{
    if (g_chan_spinner == NULL) {
        return;
    }

    if (on) {
        lv_obj_remove_flag(g_chan_spinner, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_chan_spinner, LV_OBJ_FLAG_HIDDEN);
    }
}

/* CENTER on the Refresh button: run one manual sweep. A press while a sweep is already in flight is
 * ignored - the spinner is already up and a second request would just re-interrupt video. */
static void chan_refresh_clicked(lv_event_t *event)
{
    (void) event;

    if (g_chan_scanning) {
        return;
    }

    menu_channel_request_scan();
}

/* The Refresh button below the grid: a manual re-scan trigger with a spinner that runs while a sweep
 * is in flight. Spanned full width in its own grid row so it reads as the grid's footer, and made a
 * direct child of the content pane so the menu adds it to the focus group with the tiles. The spinner
 * starts visible iff a sweep is already pending (this render can happen mid-scan).
 */
static void make_refresh_button(int row)
{
    lv_obj_t *content = g_host->content();

    lv_obj_t *button = lv_button_create(content);
    lv_obj_add_event_cb(button, chan_refresh_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, g_host->style_item(), 0);
    lv_obj_add_style(button, g_host->style_item_focused(), LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(button, 16, 0);
    lv_obj_set_grid_cell(button, LV_GRID_ALIGN_STRETCH, 0, CHAN_COLS, LV_GRID_ALIGN_CENTER, row, 1);

    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_label_set_text_fmt(label, "%s  %s", LV_SYMBOL_REFRESH, T("channel.refresh"));

    lv_obj_t *spinner = lv_spinner_create(button);
    lv_spinner_set_anim_params(spinner, 1000, 60);
    lv_obj_set_size(spinner, 28, 28);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
    if (!g_chan_scanning) {
        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    }

    g_chan_refresh = button;
    g_chan_spinner = spinner;
}

/* The channel grid: a tile per channel valid in the current mode (3 in Normal, 16 in Race), laid out
 * in a CHAN_COLS-wide LVGL grid. Renders whatever the last scan produced; it never requests one
 * itself, because this also runs for the sidebar preview and the sweep interrupts video (menu_center
 * fires it on entering the section instead). Until a scan arrives, or if none are valid, a centered
 * hint is shown and focus stays in the sidebar. The full channel table comes back regardless of
 * mode, but only the current mode's channels are usable, so out-of-mode entries are not shown.
 */
void menu_channel_render(void)
{
    lv_obj_t *content = g_host->content();
    struct mlm_scan scan;
    unsigned gen = linkstate_scan(&scan);

    g_chan_tile_count = 0;
    g_chan_scan_gen = gen;
    g_chan_refresh = NULL;   /* content is being rebuilt; the old button (if any) is gone */
    g_chan_spinner = NULL;

    if (gen == 0) {
        g_host->centered_hint(g_chan_scanning ? T("channel.scanning") : T("channel.scan_hint"));
        return;
    }

    int nvalid = 0;
    for (int i = 0; i < scan.count; i++) {
        if (scan.chan[i].valid) {
            nvalid++;
        }
    }

    if (nvalid == 0) {
        g_host->centered_hint(T("channel.no_link"));
        return;
    }

    /* Rows are equal fractions of the content height, so templating only the rows a band fills
     * would size the tiles by channel count: Normal's 3 channels would be one row stretched over
     * the whole screen. Template at least the Race band's rows so a tile is the same size in every
     * band; the surplus rows just stay empty.
     */
    int nrows = (nvalid + CHAN_COLS - 1) / CHAN_COLS;
    if (nrows < CHAN_REF_ROWS) {
        nrows = CHAN_REF_ROWS;
    }

    if (nrows > CHAN_MAX_ROWS) {
        nrows = CHAN_MAX_ROWS;
    }

    for (int c = 0; c < CHAN_COLS; c++) {
        g_chan_cols[c] = LV_GRID_FR(1);
    }

    g_chan_cols[CHAN_COLS] = LV_GRID_TEMPLATE_LAST;
    for (int r = 0; r < nrows; r++) {
        g_chan_rows[r] = LV_GRID_FR(1);
    }

    /* A content-sized footer row carries the Refresh button; the FR tile rows split what is left. */
    g_chan_rows[nrows] = LV_GRID_CONTENT;
    g_chan_rows[nrows + 1] = LV_GRID_TEMPLATE_LAST;

    lv_obj_set_style_pad_column(content, CHAN_TILE_GAP, 0);
    lv_obj_set_grid_dsc_array(content, g_chan_cols, g_chan_rows);
    lv_obj_set_layout(content, LV_LAYOUT_GRID);

    int slot = 0;
    for (int i = 0; i < scan.count && slot < MLM_SCAN_MAX_CH; i++) {
        if (!scan.chan[i].valid) {
            continue;
        }

        lv_obj_t *tile = make_channel_tile(&scan.chan[i]);
        lv_obj_set_grid_cell(tile, LV_GRID_ALIGN_STRETCH, slot % CHAN_COLS, 1,
                             LV_GRID_ALIGN_STRETCH, slot / CHAN_COLS, 1);
        set_tile_active(tile, scan.chan[i].index == scan.active_idx);
        g_chan_tiles[slot] = tile;
        g_chan_tile_idx[slot] = scan.chan[i].index;
        slot++;
    }

    g_chan_tile_count = slot;
    g_chan_active_shown = scan.active_idx;   /* menu_channel_refresh reconciles against the live channel */

    make_refresh_button(nrows);   /* the grid footer: manual re-scan + scanning spinner */
}

/* Refresh the channel grid from a newly-arrived scan. A changed tile set (first data, or a mode
 * change) needs a full rebuild, which is only safe with focus in the sidebar; while focus is in the
 * grid the signal + active highlight are updated in place so focus is kept. Called every loop tick.
 */
void menu_channel_refresh(void)
{
    struct mlm_scan scan;
    unsigned gen = linkstate_scan(&scan);

    /* Reconcile the active border against ml-linkd's live channel (MLM_T_LINKINFO, ~1 Hz), which is
     * published whether or not the air unit is up. It cannot key off the scan: a select retunes
     * without producing a new one, so the border would sit on the old channel until the next sweep.
     * Only repainted on change - this runs every loop tick. */
    if (g_chan_tile_count > 0) {
        int active = linkstate_channel();

        if (active == MLM_LINKINFO_NONE && gen != 0) {
            active = scan.active_idx;
        }

        if (active != g_chan_active_shown) {
            chan_show_active(active);
        }
    }

    if (gen == 0 || gen == g_chan_scan_gen) {
        return;
    }

    /* Only a completed sweep (measured) ends the spinner. The table is also republished unmeasured on
     * a cadence and at link-up so the grid stays populated; those must not clear a scan in flight. */
    if (scan.measured) {
        g_chan_scanning = 0;
        chan_set_scanning_indicator(0);
    }

    int nvalid = 0;
    for (int i = 0; i < scan.count; i++) {
        if (scan.chan[i].valid) {
            nvalid++;
        }
    }

    if (nvalid != g_chan_tile_count) {
        if (g_host->in_sidebar()) {
            g_host->rebuild();   /* rebuild is safe only while focus is in the sidebar */
        }

        return;
    }

    g_chan_scan_gen = gen;
    for (int s = 0; s < g_chan_tile_count; s++) {
        for (int i = 0; i < scan.count; i++) {
            if (scan.chan[i].index == g_chan_tile_idx[s]) {
                tile_apply_signal(lv_obj_get_child(g_chan_tiles[s], 2), &scan.chan[i]);
                break;
            }
        }
    }
}

/* @brief The focused tile's slot in g_chan_tiles, or -1 when focus is not on a tile. */
static int chan_focused_slot(void)
{
    lv_obj_t *focused = lv_group_get_focused(g_host->group());

    for (int i = 0; i < g_chan_tile_count; i++) {
        if (g_chan_tiles[i] == focused) {
            return i;
        }
    }

    return -1;
}

/**
 * @brief Move focus one cell across the channel grid.
 *
 * The grid is laid out row-major (menu_channel_render places slot at column slot % CHAN_COLS, row
 * slot / CHAN_COLS), but the menu's default navigation is linear PREV/NEXT over the focus group,
 * which walks tiles in child order: DOWN would step one tile to the RIGHT, and LEFT/RIGHT would do
 * nothing at all because a tile ignores those keys. Map the keys to the real geometry instead.
 * Called only while the grid holds focus (menu.c gates the section/zone check).
 *
 * @param dcol,drow  -1, 0 or +1.
 * @return 1 if the key belongs to the grid (consumed, whether or not focus moved), 0 to fall through
 *  to the default linear navigation.
 */
int menu_channel_nav(int dcol, int drow)
{
    int slot, col, target;

    /* The Refresh footer is not a grid tile: UP returns to the last tile row, and the other keys keep
     * focus put. It owns all four keys so the menu's linear fallback cannot carry focus off the grid. */
    if (g_chan_refresh != NULL && lv_group_get_focused(g_host->group()) == g_chan_refresh) {
        if (drow < 0 && g_chan_tile_count > 0) {
            lv_group_focus_obj(g_chan_tiles[g_chan_tile_count - 1]);
        }

        return 1;
    }

    slot = chan_focused_slot();
    if (slot < 0) {
        return 0;
    }

    /* horizontal moves stay in their row: no wrap onto the next one */
    col = slot % CHAN_COLS;
    if (dcol != 0 && (col + dcol < 0 || col + dcol >= CHAN_COLS)) {
        return 1;
    }

    /* grid edges, and the ragged last row (Normal is a single row of 3, so DOWN has nowhere to go).
     * DOWN off the bottom tile row drops onto the Refresh footer instead of stopping. */
    target = slot + dcol + drow * CHAN_COLS;
    if (target < 0 || target >= g_chan_tile_count) {
        if (drow > 0 && dcol == 0 && g_chan_refresh != NULL) {
            lv_group_focus_obj(g_chan_refresh);
        }

        return 1;
    }

    lv_group_focus_obj(g_chan_tiles[target]);
    return 1;
}
