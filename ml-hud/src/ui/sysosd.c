/** @file sysosd.c @brief See sysosd.h. */
#include "sysosd.h"

#include "channel_label.h"
#include "linkstate.h"

#include "../../../ml-shared/mlm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Palette + spacing, matching the menu/libre look (libre app/menu_config.h). */
#define COLOR_OSD       lv_color_hex(0x0E141E)
#define COLOR_GREEN     lv_color_hex(0x46D17B)
#define COLOR_TEXT      lv_color_hex(0xE6EAF0)
#define COLOR_TEXT_DIM  lv_color_hex(0x7A8497)   /* placeholder / no-link value */
#define COLOR_WARN      lv_color_hex(0xE0633A)
#define COLOR_REC       lv_color_hex(0xFF0000)   /* System OSD "REC" indicator (matches libre) */
#define COLOR_BIND      lv_color_hex(0xD99A2B)   /* orange: "BIND" indicator (shares the REC slot) */
#define COLOR_PARTIAL   lv_color_hex(0xD99A2B)   /* orange: standby cue */
#define COLOR_YELLOW    lv_color_hex(0xE8D53A)   /* downlink headroom gauge: link filling up */
#define OSD_PAD_HOR     44
#define OSD_FIELD_GAP   36
#define OSD_ICON_GAP    10   /* minimum icon-to-value gap inside an icon field (at the widest value) */

/* Values render in B612 (baked by lv_font_conv - see font_b612_28.c): proportional, but with tabular
 * digits, so a ticking digit never changes the string width while ':'/'.' and unit letters stay
 * tight. The LV_SYMBOL_* icons only exist in LVGL's Montserrat builds, so icon labels keep that
 * font. */
extern const lv_font_t font_b612_28;
#define OSD_FONT        (&font_b612_28)
#define ICON_FONT       (&lv_font_montserrat_28)

/* Every continuously-varying field gets a fixed width, measured from its widest possible string
 * (field_width), so a value changing width (more digits, "No Link", ...) never reflows the flex row
 * and shifts its neighbours. The channel is the exception: it only changes on a deliberate channel
 * switch, so it is content-sized (no idle slack) and the one-off reflow then is fine. Iconed fields
 * are a fixed-width box with the icon pinned to the left edge and the value's right edge pinned to
 * the box's right edge (icon_field_t), so neither moves as the value width changes. Plain right-group
 * text is right-aligned so each value's right edge stays pinned to the bar's right edge. */
#define OSD_MAX_BATTERY  "25.2V"       /* 6S full; the cell count is not displayed */
#define OSD_MAX_LINK     "88dB"
#define OSD_MAX_BITRATE  "88.8Mbps"
#define OSD_MAX_SDCARD   "256G"        /* largest card realistically used */
#define OSD_MAX_TEMP     "88°C"
#define OSD_MAX_AIRBAT   "25.20V"      /* 6S full, %d.%02dV format */
#define OSD_MAX_DISTANCE "8888m"
#define OSD_MAX_AIR_BR   "88.8"           /* air encoder bitrate (SEI), own fixed field */
#define OSD_MAX_AIR_QP   "88"             /* air encoder QP (SEI), own fixed field */
#define OSD_MAX_AIR_DL   "88.8"           /* air-unit RF-link throughput / capacity (Mbps) */
#define OSD_MAX_ONTIME   "88:88"          /* air-unit on-time MM:SS (u32 us counter maxes at 71:34) */

/* Low-battery blink: half-period of the battery-icon blink while the alarm is active. */
#define ALARM_TICK_MS   250

#define SIGNAL_MAX      4          /* SNR is mapped onto 0..SIGNAL_MAX bars purely for the field colour */

#define GOG_SECTION     "goggle"   /* the goggle settings section (mirrors menu.c) */

/* An iconed field: a fixed-width box holding the symbol (Montserrat) and the value (mono) as separate
 * labels, flexed apart so the icon hugs the left edge and the value's right edge hugs the right edge.
 * Colour is styled on the box (text colour inherits to both labels); the alarm blink toggles the box
 * opacity so icon and value blink together. */
typedef struct {
    lv_obj_t *box;
    lv_obj_t *icon;
    lv_obj_t *value;
} icon_field_t;

static lv_obj_t *g_osd;              /* the bar; child of the screen */
static lv_obj_t *g_group_left;       /* goggle + connection group */
static lv_obj_t *g_group_right;      /* air-unit (quad) group */
static lv_obj_t *g_lbl_channel;      /* RF channel (services/linkstate) */
static icon_field_t g_fld_battery;   /* goggle pack */
static icon_field_t g_fld_link;      /* WIFI + SNR (services/linkstate) */
static icon_field_t g_fld_sdcard;
static icon_field_t g_fld_bitrate;   /* incoming-video downlink rate (Mbps); dim when the link is down */
static lv_obj_t *g_lbl_temp;         /* goggle SoC temperature; gated by the Show Temperature setting */
static lv_obj_t *g_lbl_rec;          /* red "REC"; shown only while ml-pipeline reports recording */
static lv_obj_t *g_lbl_standby;      /* power glyph at the left of the air group; shown only in standby */
static icon_field_t g_fld_quad_battery; /* air-unit pack (:10000 status frame) */
static icon_field_t g_fld_distance;  /* RF-ranging distance (services/linkstate) */
static lv_obj_t *g_lbl_quad_temp;    /* air-unit temperature (:10000 @98); gated by Show Temperature */
static icon_field_t g_fld_air_br;    /* air encoder bitrate (Mbps) from the SEI (MLM_T_FRAMESTATS) */
static icon_field_t g_fld_air_qp;    /* air encoder QP from the SEI; own field so it never shifts BR */
static icon_field_t g_fld_air_dl;    /* air-unit RF-link throughput/capacity (chip Get1V1Info) */
static lv_obj_t *g_lbl_ontime;       /* air-unit on-time MM:SS (:10000 header timestamp) */

static int g_menu_open;
static int g_force_hidden;            /* playback hides the whole bar, overriding the setting */
static int g_alarm_active;           /* set each update; read by the blink timer */
static int g_blink_on = 1;

/* Coarse charge level from the per-cell voltage (LiPo ~3.3 empty .. 4.2 full). */
static const char *battery_icon_for(float per_cell_volts)
{
    if (per_cell_volts >= 4.0f) {
        return LV_SYMBOL_BATTERY_FULL;
    }

    if (per_cell_volts >= 3.8f) {
        return LV_SYMBOL_BATTERY_3;
    }

    if (per_cell_volts >= 3.6f) {
        return LV_SYMBOL_BATTERY_2;
    }

    if (per_cell_volts >= 3.4f) {
        return LV_SYMBOL_BATTERY_1;
    }

    return LV_SYMBOL_BATTERY_EMPTY;
}

/* Worst=red .. best=green over a 0..max level (libre signal_color). */
static lv_color_t signal_color(int level, int max)
{
    if (level < 0 || max <= 0) {
        return COLOR_TEXT_DIM;
    }

    if (level > max) {
        level = max;
    }

    int hue = level * 120 / max;   /* 0deg = red, 60 = yellow, 120 = green */
    return lv_color_hsv_to_rgb((uint16_t) hue, 80, 95);
}

/* Pixel width of @p text rendered in @p font. Fields are fixed to the width of their widest possible
 * string so a changing value never reflows the bar. */
static int32_t text_width(const char *text, const lv_font_t *font)
{
    lv_point_t size;

    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static int32_t field_width(const char *text)
{
    return text_width(text, OSD_FONT);
}

static lv_obj_t *add_field(lv_obj_t *group, const char *value, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(group);

    lv_label_set_text(label, value);
    lv_obj_set_style_text_font(label, OSD_FONT, 0);
    lv_obj_set_style_text_color(label, color, 0);

    return label;
}

/* A two-label field: box fixed to lead + gap + widest value (@p max_value), lead and value flexed to
 * opposite edges. The gap absorbs any value-width change, so the lead never moves and the value's
 * right edge stays put. The lead is an LV_SYMBOL_* glyph (icon font) or a text prefix like "BR"
 * (OSD font); see the two wrappers below. */
static icon_field_t make_pair_field(lv_obj_t *group, const char *lead, const lv_font_t *lead_font,
                                    const char *value, lv_color_t color, const char *max_value)
{
    icon_field_t field;

    field.box = lv_obj_create(group);
    lv_obj_remove_style_all(field.box);
    lv_obj_remove_flag(field.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(field.box, text_width(lead, lead_font) + OSD_ICON_GAP + field_width(max_value),
                    lv_pct(100));
    lv_obj_set_flex_flow(field.box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(field.box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_text_color(field.box, color, 0);

    field.icon = lv_label_create(field.box);
    lv_label_set_text(field.icon, lead);
    lv_obj_set_style_text_font(field.icon, lead_font, 0);

    field.value = lv_label_create(field.box);
    lv_label_set_text(field.value, value);
    lv_obj_set_style_text_font(field.value, OSD_FONT, 0);

    return field;
}

static icon_field_t add_icon_field(lv_obj_t *group, const char *icon, const char *value,
                                   lv_color_t color, const char *max_value)
{
    return make_pair_field(group, icon, ICON_FONT, value, color, max_value);
}

static icon_field_t add_prefix_field(lv_obj_t *group, const char *prefix, const char *value,
                                     lv_color_t color, const char *max_value)
{
    return make_pair_field(group, prefix, OSD_FONT, value, color, max_value);
}

/* Set a field's value text and colour in one call. Colour goes on the box - inherited by both
 * labels - never on the labels themselves, so the icon/value tint and the alarm blink (box opacity)
 * stay in sync. LVGL has no va_list set_text variant, so format locally.
 */
__attribute__((format(printf, 3, 4)))
static void field_set(icon_field_t *field, lv_color_t color, const char *fmt, ...)
{
    char text[32];
    va_list args;

    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);

    lv_label_set_text(field->value, text);
    lv_obj_set_style_text_color(field->box, color, 0);
}

static lv_obj_t *make_group(void)
{
    lv_obj_t *group = lv_obj_create(g_osd);
    lv_obj_remove_style_all(group);
    lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(group, LV_SIZE_CONTENT, lv_pct(100));   /* hug the fields, do not clip */
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(group, OSD_FIELD_GAP, 0);

    return group;
}

/* Blink the battery icon while the low-battery alarm is active. Opacity (not hide) is toggled so the
 * bar layout does not jump. The chirp is the HUD loop's job (alarm_check), not this timer.
 */
static void alarm_tick_cb(lv_timer_t *timer)
{
    (void) timer;
    if (!g_alarm_active) {
        lv_obj_set_style_opa(g_fld_battery.box, LV_OPA_COVER, 0);
        g_blink_on = 1;
        return;
    }

    g_blink_on = !g_blink_on;
    lv_obj_set_style_opa(g_fld_battery.box, g_blink_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void sysosd_create(lv_obj_t *parent)
{
    g_osd = lv_obj_create(parent);
    lv_obj_remove_style_all(g_osd);
    lv_obj_set_size(g_osd, lv_pct(100), OSD_BAR_HEIGHT);
    lv_obj_align(g_osd, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(g_osd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_osd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_osd, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(g_osd, OSD_PAD_HOR, 0);
    lv_obj_set_style_bg_color(g_osd, COLOR_OSD, 0);
    lv_obj_set_style_bg_opa(g_osd, LV_OPA_TRANSP, 0);   /* transparent until the menu opens */

    /* LEFT: goggle + connection, ordered channel - link - battery - bitrate - SD - REC - temp.
     * Channel and link come from ml-linkd (services/linkstate); battery, SD and temperature are
     * goggle-local (hal/telemetry). */
    g_group_left = make_group();
    g_lbl_channel = add_field(g_group_left, "CH --", COLOR_TEXT_DIM);
    g_fld_link = add_icon_field(g_group_left, LV_SYMBOL_WIFI, "--dB", COLOR_WARN, OSD_MAX_LINK);
    g_fld_battery = add_icon_field(g_group_left, LV_SYMBOL_BATTERY_FULL, "--.-V", COLOR_TEXT,
                                   OSD_MAX_BATTERY);

    /* Incoming-video bitrate. Only meaningful while the air-unit downlink is up, so it reads a dim
     * placeholder otherwise (like the RF fields). */
    g_fld_bitrate = add_icon_field(g_group_left, LV_SYMBOL_DOWNLOAD, "--.-Mbps", COLOR_TEXT_DIM,
                                   OSD_MAX_BITRATE);
    g_fld_sdcard = add_icon_field(g_group_left, LV_SYMBOL_SD_CARD, "--", COLOR_TEXT, OSD_MAX_SDCARD);

    /* REC sits mid-group, so it keeps its slot when idle (opacity toggled, not hidden): hiding it
     * would shift the temperature field each time recording starts/stops. */
    /* One slot shared by REC (recording) and BIND (binding a new air unit); they are mutually
     * exclusive in practice (binding runs only while disconnected, recording only while connected).
     * Sized to the wider "BIND" so neither clips and the fields after it never shift. */
    g_lbl_rec = add_field(g_group_left, "REC", COLOR_REC);
    lv_obj_set_width(g_lbl_rec, field_width("BIND"));
    lv_obj_set_style_opa(g_lbl_rec, LV_OPA_TRANSP, 0);   /* visible only while recording/binding */
    g_lbl_temp = add_field(g_group_left, "--°C", COLOR_TEXT);
    lv_obj_set_width(g_lbl_temp, field_width(OSD_MAX_TEMP));
    lv_obj_add_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);   /* shown only when enabled and a reading exists */

    /* RIGHT: air unit, ordered standby - temp - distance - battery. Battery + temperature ride the
     * :10000 status frames; distance is a local baseband reading via ml-linkd; standby is the air's
     * work-mode readback (SetStandyMode 0x12), shown only when the air reports standby.
     * The group is pinned to the bar's right edge, so hiding a left field leaves the fields to its
     * right in place instead of shifting them. The two toggling fields (standby with the air's arm
     * state, temperature with Show Temperature) are therefore the leftmost, keeping the always-on
     * distance + battery steady. */
    g_group_right = make_group();
    g_lbl_standby = add_field(g_group_right, LV_SYMBOL_POWER, COLOR_PARTIAL);
    lv_obj_set_style_text_font(g_lbl_standby, ICON_FONT, 0);   /* symbol glyph, not in the mono font */
    lv_obj_add_flag(g_lbl_standby, LV_OBJ_FLAG_HIDDEN);

    /* Air encoder self-report (SEI BR/QP) and the air link throughput/capacity (chip Get1V1Info).
     * Opt-in overlays, hidden until their toggle is on; leftmost of the right group so enabling one
     * grows the bar leftward without shifting the steady distance/battery fields. */
    g_fld_air_br = add_prefix_field(g_group_right, "BR", "--.-", COLOR_TEXT_DIM, OSD_MAX_AIR_BR);
    lv_obj_add_flag(g_fld_air_br.box, LV_OBJ_FLAG_HIDDEN);
    g_fld_air_qp = add_prefix_field(g_group_right, "QP", "--", COLOR_TEXT_DIM, OSD_MAX_AIR_QP);
    lv_obj_add_flag(g_fld_air_qp.box, LV_OBJ_FLAG_HIDDEN);
    g_fld_air_dl = add_prefix_field(g_group_right, "MAX", "--.-", COLOR_TEXT_DIM, OSD_MAX_AIR_DL);
    lv_obj_add_flag(g_fld_air_dl.box, LV_OBJ_FLAG_HIDDEN);
    g_lbl_ontime = add_field(g_group_right, "--:--", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_ontime, field_width(OSD_MAX_ONTIME));
    lv_obj_set_style_text_align(g_lbl_ontime, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(g_lbl_ontime, LV_OBJ_FLAG_HIDDEN);

    g_lbl_quad_temp = add_field(g_group_right, "--°C", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_quad_temp, field_width(OSD_MAX_TEMP));
    lv_obj_set_style_text_align(g_lbl_quad_temp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(g_lbl_quad_temp, LV_OBJ_FLAG_HIDDEN);
    g_fld_distance = add_icon_field(g_group_right, LV_SYMBOL_GPS, "--m", COLOR_TEXT_DIM,
                                    OSD_MAX_DISTANCE);
    g_fld_quad_battery = add_icon_field(g_group_right, LV_SYMBOL_BATTERY_2, "--.-V", COLOR_TEXT_DIM,
                                        OSD_MAX_AIRBAT);

    lv_timer_create(alarm_tick_cb, ALARM_TICK_MS, NULL);
}

void sysosd_set_menu_open(int open)
{
    g_menu_open = open;
    if (g_osd != NULL) {
        lv_obj_set_style_bg_opa(g_osd, open ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

/* REC and BIND share g_lbl_rec. Binding wins the slot when both are somehow set (a bind only runs
 * while disconnected, so recording cannot really be active then). Opacity, not hide: the field keeps
 * its slot so the fields after it never shift. */
static int g_recording;
static int g_binding;

static void rec_slot_refresh(void)
{
    if (g_lbl_rec == NULL) {
        return;
    }

    if (g_binding) {
        lv_label_set_text(g_lbl_rec, "BIND");
        lv_obj_set_style_text_color(g_lbl_rec, COLOR_BIND, 0);
        lv_obj_set_style_opa(g_lbl_rec, LV_OPA_COVER, 0);
    } else if (g_recording) {
        lv_label_set_text(g_lbl_rec, "REC");
        lv_obj_set_style_text_color(g_lbl_rec, COLOR_REC, 0);
        lv_obj_set_style_opa(g_lbl_rec, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_opa(g_lbl_rec, LV_OPA_TRANSP, 0);
    }
}

void sysosd_set_recording(int recording)
{
    g_recording = recording;
    rec_slot_refresh();
}

void sysosd_set_binding(int binding)
{
    g_binding = binding;
    rec_slot_refresh();
}

void sysosd_set_visible(int visible)
{
    if (g_osd == NULL) {
        return;
    }

    g_force_hidden = !visible;   /* latched: sysosd_update must not override it each tick */
    if (visible) {
        lv_obj_remove_flag(g_osd, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_osd, LV_OBJ_FLAG_HIDDEN);   /* playback shows only the transport bar */
    }
}

void sysosd_invalidate(void)
{
    if (g_osd != NULL) {
        lv_obj_invalidate(g_osd);
    }
}

/* Update the goggle battery field + the low-battery alarm flag. */
static void update_goggle_battery(const telemetry_t *telemetry, settings_t *settings)
{
    if (!telemetry->have_battery) {
        /* Dim the placeholder like every other no-reading field, so a latched alarm colour does not
         * outlive its reading. */
        g_alarm_active = 0;
        lv_label_set_text(g_fld_battery.icon, LV_SYMBOL_BATTERY_FULL);
        field_set(&g_fld_battery, COLOR_TEXT_DIM, "--.-V");
        return;
    }

    /* The cell count still scales the alarm threshold (per-cell volts) but is not displayed. */
    float per_cell = (telemetry->cell_count > 0)
                     ? telemetry->pack_volts / telemetry->cell_count
                     : telemetry->pack_volts;
    int volts_tenths = (int) (telemetry->pack_volts * 10.0f + 0.5f);

    int alarm_on = settings_get_bool_in(settings, GOG_SECTION, "low_voltage_alarm", 1);
    float threshold = (float) atof(settings_get_string_in(settings, GOG_SECTION, "min_cell_voltage", "3.4V"));
    g_alarm_active = (alarm_on && per_cell < threshold);
    lv_label_set_text(g_fld_battery.icon, battery_icon_for(per_cell));
    field_set(&g_fld_battery, g_alarm_active ? COLOR_WARN : COLOR_TEXT, "%d.%dV",
              volts_tenths / 10, volts_tenths % 10);
}

/* Update the RF fields from ml-linkd (channel, SNR, distance). Dim placeholders when the link is down
 * or a value has not arrived. The channel is the table index our RX is tuned to - a local fact that
 * holds whether or not the air is connected - so it is shown whenever known (green while linked, plain
 * otherwise) so a pilot can always read off their channel. */
static void update_link_fields(int connected, settings_t *settings)
{
    /* Before ml-linkd's first report, fall back to the saved channel (the value the HUD asserts on
     * every link-up; default 0, the Normal-band bring-up channel), so the field is right from the
     * first paint instead of flashing "CH --" and reflowing the bar. The content-sized label then
     * only changes on an actual channel switch. */
    int channel = linkstate_channel();
    if (channel == MLM_LINKINFO_NONE) {
        channel = settings_get_int_in(settings, GOG_SECTION, "channel", 0);
    }

    if (channel >= 0) {
        char label[24];

        channel_label(label, sizeof label, channel);
        lv_label_set_text(g_lbl_channel, label);
        lv_obj_set_style_text_color(g_lbl_channel, connected ? COLOR_GREEN : COLOR_TEXT, 0);
    } else {
        lv_label_set_text(g_lbl_channel, "CH --");
        lv_obj_set_style_text_color(g_lbl_channel, COLOR_TEXT_DIM, 0);
    }

    int snr = linkstate_snr_db();
    if (connected && snr != MLM_LINKINFO_NONE) {
        /* Map SNR onto 0..SIGNAL_MAX bars for the colour only: ~0 dB red .. >=20 dB green. */
        int level = snr <= 0 ? 0 : snr >= 20 ? SIGNAL_MAX : snr / 5;
        field_set(&g_fld_link, signal_color(level, SIGNAL_MAX), "%ddB", snr);
    } else {
        /* Down/no-reading: dim dashes like the other fields; the warn colour alone flags the state. */
        field_set(&g_fld_link, COLOR_WARN, "--dB");
    }

    int distance = linkstate_distance_m();
    if (connected && distance != MLM_LINKINFO_NONE) {
        field_set(&g_fld_distance, COLOR_TEXT, "%dm", distance);
    } else {
        field_set(&g_fld_distance, COLOR_TEXT_DIM, "--m");
    }

    /* Standby cue: the power glyph, shown only when the air reports standby (quad disarmed +
     * standby armed). Hidden otherwise, so the bar is unchanged when the air is active. */
    if (connected && linkstate_is_standby()) {
        lv_obj_remove_flag(g_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Update the air-unit battery + temperature from the :10000 status frames. @p show_temp gates the
 * temperature field (the Show Temperature setting), matching the goggle temp. */
static void update_air_fields(const air_telem_t *air, int show_temp)
{
    if (air->have_voltage) {
        int cv = air->voltage_mV / 10;   /* mV -> centivolts, e.g. 7420 -> 742 -> "7.42V" */
        field_set(&g_fld_quad_battery, COLOR_TEXT, "%d.%02dV", cv / 100, cv % 100);
    } else {
        field_set(&g_fld_quad_battery, COLOR_TEXT_DIM, "--.-V");
    }

    if (show_temp) {
        if (air->have_temp) {
            lv_label_set_text_fmt(g_lbl_quad_temp, "%d°C", air->temp_c);
            lv_obj_set_style_text_color(g_lbl_quad_temp, COLOR_TEXT, 0);
        } else {
            lv_label_set_text(g_lbl_quad_temp, "--°C");
            lv_obj_set_style_text_color(g_lbl_quad_temp, COLOR_TEXT_DIM, 0);
        }
        lv_obj_remove_flag(g_lbl_quad_temp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_quad_temp, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Air-unit on-time as MM:SS, off the :10000 header timestamp (us since air boot, relayed via
 * air_telem_t). Opt-in overlay: hidden unless its toggle is on; dim dashes when the link is down.
 */
static void update_air_ontime(const air_telem_t *air, int show_ontime)
{
    if (!show_ontime) {
        lv_obj_add_flag(g_lbl_ontime, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (air->have_ontime) {
        lv_label_set_text_fmt(g_lbl_ontime, "%02u:%02u", air->ontime_s / 60, air->ontime_s % 60);
        lv_obj_set_style_text_color(g_lbl_ontime, COLOR_TEXT, 0);
    } else {
        lv_label_set_text(g_lbl_ontime, "--:--");
        lv_obj_set_style_text_color(g_lbl_ontime, COLOR_TEXT_DIM, 0);
    }

    lv_obj_remove_flag(g_lbl_ontime, LV_OBJ_FLAG_HIDDEN);
}

/* Colour the downlink rate by how close it runs to the link capacity (MAX = PHY throughput, chip
 * Get1V1Info +0x0c). The air targets ~70% of goodput, so the calm band runs to ~78%; yellow/orange/
 * red flag the shrinking margin - a red flash means the link dipped before the encoder backed off,
 * i.e. breakup imminent. Neutral white when there is no capacity reading. */
static lv_color_t downlink_color(float dl_mbps, int max_kbps)
{
    if (max_kbps <= 0 || dl_mbps <= 0.0f) {
        return COLOR_TEXT;
    }

    int pct = (int) (dl_mbps * 1000.0f * 100.0f / (float) max_kbps + 0.5f);
    if (pct < 45) {
        return COLOR_TEXT;      /* low usage, ample headroom */
    }

    if (pct < 78) {
        return COLOR_GREEN;     /* healthy - the ~70% steady-state target lives here */
    }

    if (pct < 88) {
        return COLOR_YELLOW;
    }

    if (pct < 94) {
        return COLOR_PARTIAL;   /* orange */
    }

    return COLOR_REC;           /* red: at capacity, breakup risk */
}

/* Air encoder self-report (SEI BR/QP from ml-pipeline) and the air link throughput / capacity (chip
 * Get1V1Info via ml-linkd). Both are opt-in overlays: hidden unless their toggle is on. Values dim
 * to placeholders when the feed is stale or the link is down. Rates are kbps -> Mbps, one decimal. */
static void update_air_encoder_fields(int connected, int show_encoder, int show_throughput)
{
    if (show_encoder) {
        int br_kbps = 0, qp = 0;
        if (linkstate_sei_brqp(&br_kbps, &qp) && br_kbps > 0) {
            int tenths = (br_kbps + 50) / 100;
            field_set(&g_fld_air_br, COLOR_TEXT, "%d.%d", tenths / 10, tenths % 10);
            field_set(&g_fld_air_qp, COLOR_TEXT, "%d", qp);
        } else {
            field_set(&g_fld_air_br, COLOR_TEXT_DIM, "--.-");
            field_set(&g_fld_air_qp, COLOR_TEXT_DIM, "--");
        }

        lv_obj_remove_flag(g_fld_air_br.box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(g_fld_air_qp.box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_fld_air_br.box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_fld_air_qp.box, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_throughput) {
        int max_kbps = linkstate_throughput_kbps();
        if (connected && max_kbps > 0) {
            int tenths = (max_kbps + 50) / 100;
            field_set(&g_fld_air_dl, COLOR_TEXT, "%d.%d", tenths / 10, tenths % 10);
        } else {
            field_set(&g_fld_air_dl, COLOR_TEXT_DIM, "--.-");
        }

        lv_obj_remove_flag(g_fld_air_dl.box, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_fld_air_dl.box, LV_OBJ_FLAG_HIDDEN);
    }
}

void sysosd_update(const telemetry_t *telemetry, const air_telem_t *air, settings_t *settings)
{
    if (g_osd == NULL) {
        return;
    }

    /* Playback hides the bar outright, leaving only the transport overlay. */
    if (g_force_hidden) {
        lv_obj_add_flag(g_osd, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* The bar shows while the menu is open (for context) or the Show System OSD setting is on. */
    int show_bar = g_menu_open || settings_get_bool_in(settings, GOG_SECTION, "show_system_osd", 1);
    if (show_bar) {
        lv_obj_remove_flag(g_osd, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_osd, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    int connected = linkstate_is_airunit_connected();
    int show_temp = settings_get_bool_in(settings, GOG_SECTION, "show_temperature", 1);
    int show_encoder = settings_get_bool_in(settings, GOG_SECTION, "show_encoder_stats", 0);
    int show_throughput = settings_get_bool_in(settings, GOG_SECTION, "show_link_throughput", 0);
    int show_ontime = settings_get_bool_in(settings, GOG_SECTION, "show_air_ontime", 0);

    update_goggle_battery(telemetry, settings);
    update_link_fields(connected, settings);
    update_air_fields(air, show_temp);
    update_air_encoder_fields(connected, show_encoder, show_throughput);
    update_air_ontime(air, show_ontime);

    if (telemetry->have_sdcard) {
        lv_label_set_text_fmt(g_fld_sdcard.value, "%dG", (int) (telemetry->sd_free_gb + 0.5f));
    } else {
        lv_label_set_text(g_fld_sdcard.value, "--");
    }

    /* Incoming-video bitrate: driven by the actual sdio0 byte flow (telemetry.c smooths it and holds
     * the last value across brief gaps), NOT the link-liveness flag - the air throttles its status
     * cadence in standby, which would otherwise blank a steadily-arriving video rate. */
    if (telemetry->have_bitrate) {
        int tenths = (int) (telemetry->bitrate_mbps * 10.0f + 0.5f);
        /* colour by headroom against the link capacity (MAX): calm when there is margin, red near
         * saturation. Uses MAX even when its overlay is off, so the gauge always works. */
        field_set(&g_fld_bitrate, downlink_color(telemetry->bitrate_mbps, linkstate_throughput_kbps()),
                  "%d.%dMbps", tenths / 10, tenths % 10);
    } else {
        field_set(&g_fld_bitrate, COLOR_TEXT_DIM, "--.-Mbps");
    }

    /* Goggle SoC temperature, shown only when the Show Temperature setting is on and a reading exists. */
    if (show_temp && telemetry->have_temp) {
        lv_label_set_text_fmt(g_lbl_temp, "%d°C", telemetry->temp_c);
        lv_obj_remove_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    }
}
