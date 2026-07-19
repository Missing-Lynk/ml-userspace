/** @file sysosd.c @brief See sysosd.h. */
#include "sysosd.h"

#include "channel_label.h"
#include "linkstate.h"

#include "../../../ml-shared/mlm.h"

#include <stdlib.h>
#include <string.h>

/* Palette + spacing, matching the menu/libre look (libre app/menu_config.h). */
#define COLOR_OSD       lv_color_hex(0x0E141E)
#define COLOR_GREEN     lv_color_hex(0x46D17B)
#define COLOR_TEXT      lv_color_hex(0xE6EAF0)
#define COLOR_TEXT_DIM  lv_color_hex(0x7A8497)   /* placeholder / no-link value */
#define COLOR_WARN      lv_color_hex(0xE0633A)
#define COLOR_REC       lv_color_hex(0xFF0000)   /* System OSD "REC" indicator (matches libre) */
#define COLOR_PARTIAL   lv_color_hex(0xD99A2B)   /* orange: standby cue */
#define OSD_PAD_HOR     44
#define OSD_FIELD_GAP   36
#define OSD_FONT        (&lv_font_montserrat_28)

/* Every continuously-varying field gets a fixed width, measured from its widest possible string
 * (field_width), so a value changing width (more digits, "No Link", ...) never reflows the flex row
 * and shifts its neighbours. The channel is the exception: it only changes on a deliberate channel
 * switch, so it is content-sized (no idle slack) and the one-off reflow then is fine. Left-group text
 * is left-aligned inside its box (grows rightward into the box); right-group text is right-aligned so
 * each value's right edge stays pinned to the bar's right edge. */
#define OSD_MAX_BATTERY  LV_SYMBOL_BATTERY_FULL " 25.2V (6S)"
#define OSD_MAX_LINK     LV_SYMBOL_WIFI " 88 dB"
#define OSD_MAX_BITRATE  LV_SYMBOL_DOWNLOAD " 88.8 Mbps"
#define OSD_MAX_SDCARD   LV_SYMBOL_SD_CARD " 256G"   /* largest card realistically used */
#define OSD_MAX_TEMP     "88°C"
#define OSD_MAX_AIRBAT   LV_SYMBOL_BATTERY_2 " 25.20V"   /* 6S full, %d.%02dV format */
#define OSD_MAX_DISTANCE LV_SYMBOL_GPS " 8888 m"

/* Low-battery blink: half-period of the battery-icon blink while the alarm is active. */
#define ALARM_TICK_MS   250

#define SIGNAL_MAX      4          /* SNR is mapped onto 0..SIGNAL_MAX bars purely for the field colour */

#define GOG_SECTION     "goggle"   /* the goggle settings section (mirrors menu.c) */

static lv_obj_t *g_osd;              /* the bar; child of the screen */
static lv_obj_t *g_group_left;       /* goggle + connection group */
static lv_obj_t *g_group_right;      /* air-unit (quad) group */
static lv_obj_t *g_lbl_channel;      /* RF channel (services/linkstate) */
static lv_obj_t *g_lbl_battery;      /* goggle pack */
static lv_obj_t *g_lbl_link;         /* WIFI + SNR (services/linkstate) */
static lv_obj_t *g_lbl_sdcard;
static lv_obj_t *g_lbl_bitrate;      /* incoming-video downlink rate (Mbps); dim when the link is down */
static lv_obj_t *g_lbl_temp;         /* goggle SoC temperature; gated by the Show Temperature setting */
static lv_obj_t *g_lbl_rec;          /* red "REC"; shown only while ml-pipeline reports recording */
static lv_obj_t *g_lbl_standby;      /* power glyph at the left of the air group; shown only in standby */
static lv_obj_t *g_lbl_quad_battery; /* air-unit pack (:10000 status frame) */
static lv_obj_t *g_lbl_distance;     /* RF-ranging distance (services/linkstate) */
static lv_obj_t *g_lbl_quad_temp;    /* air-unit temperature (:10000 @98); gated by Show Temperature */

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

/* Pixel width of @p text rendered in the OSD font. Fields are fixed to the width of their widest
 * possible string so a changing value never reflows the bar. */
static int32_t field_width(const char *text)
{
    lv_point_t size;

    lv_text_get_size(&size, text, OSD_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

static lv_obj_t *add_field(lv_obj_t *group, const char *icon, const char *value, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(group);
    if (icon != NULL) {
        lv_label_set_text_fmt(label, "%s %s", icon, value);
    } else {
        lv_label_set_text(label, value);
    }

    lv_obj_set_style_text_font(label, OSD_FONT, 0);
    lv_obj_set_style_text_color(label, color, 0);

    return label;
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
        lv_obj_set_style_opa(g_lbl_battery, LV_OPA_COVER, 0);
        g_blink_on = 1;
        return;
    }

    g_blink_on = !g_blink_on;
    lv_obj_set_style_opa(g_lbl_battery, g_blink_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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
    g_lbl_channel = add_field(g_group_left, NULL, "CH --", COLOR_TEXT_DIM);
    g_lbl_link = add_field(g_group_left, LV_SYMBOL_WIFI, "-- dB", COLOR_WARN);
    lv_obj_set_width(g_lbl_link, field_width(OSD_MAX_LINK));
    g_lbl_battery = add_field(g_group_left, LV_SYMBOL_BATTERY_FULL, "--.-V", COLOR_TEXT);
    lv_obj_set_width(g_lbl_battery, field_width(OSD_MAX_BATTERY));

    /* Incoming-video bitrate. Only meaningful while the air-unit downlink is up, so it reads a dim
     * placeholder otherwise (like the RF fields). */
    g_lbl_bitrate = add_field(g_group_left, LV_SYMBOL_DOWNLOAD, "--.- Mbps", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_bitrate, field_width(OSD_MAX_BITRATE));
    g_lbl_sdcard = add_field(g_group_left, LV_SYMBOL_SD_CARD, "--", COLOR_TEXT);
    lv_obj_set_width(g_lbl_sdcard, field_width(OSD_MAX_SDCARD));

    /* REC sits mid-group, so it keeps its slot when idle (opacity toggled, not hidden): hiding it
     * would shift the temperature field each time recording starts/stops. */
    g_lbl_rec = add_field(g_group_left, NULL, "REC", COLOR_REC);
    lv_obj_set_width(g_lbl_rec, field_width("REC"));
    lv_obj_set_style_opa(g_lbl_rec, LV_OPA_TRANSP, 0);   /* visible only while recording */
    g_lbl_temp = add_field(g_group_left, NULL, "--°C", COLOR_TEXT);
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
    g_lbl_standby = add_field(g_group_right, LV_SYMBOL_POWER, "", COLOR_PARTIAL);
    lv_obj_add_flag(g_lbl_standby, LV_OBJ_FLAG_HIDDEN);
    g_lbl_quad_temp = add_field(g_group_right, NULL, "--°C", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_quad_temp, field_width(OSD_MAX_TEMP));
    lv_obj_set_style_text_align(g_lbl_quad_temp, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_add_flag(g_lbl_quad_temp, LV_OBJ_FLAG_HIDDEN);
    g_lbl_distance = add_field(g_group_right, LV_SYMBOL_GPS, "-- m", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_distance, field_width(OSD_MAX_DISTANCE));
    lv_obj_set_style_text_align(g_lbl_distance, LV_TEXT_ALIGN_RIGHT, 0);
    g_lbl_quad_battery = add_field(g_group_right, LV_SYMBOL_BATTERY_2, "--.-V", COLOR_TEXT_DIM);
    lv_obj_set_width(g_lbl_quad_battery, field_width(OSD_MAX_AIRBAT));
    lv_obj_set_style_text_align(g_lbl_quad_battery, LV_TEXT_ALIGN_RIGHT, 0);

    lv_timer_create(alarm_tick_cb, ALARM_TICK_MS, NULL);
}

void sysosd_set_menu_open(int open)
{
    g_menu_open = open;
    if (g_osd != NULL) {
        lv_obj_set_style_bg_opa(g_osd, open ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

void sysosd_set_recording(int recording)
{
    if (g_lbl_rec == NULL) {
        return;
    }

    /* Opacity, not hide: the field keeps its slot so the fields after it never shift. */
    lv_obj_set_style_opa(g_lbl_rec, recording ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
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
        g_alarm_active = 0;
        lv_label_set_text(g_lbl_battery, LV_SYMBOL_BATTERY_FULL " --.-V");
        return;
    }

    float per_cell = (telemetry->cell_count > 0)
                     ? telemetry->pack_volts / telemetry->cell_count
                     : telemetry->pack_volts;
    int volts_tenths = (int) (telemetry->pack_volts * 10.0f + 0.5f);

    int alarm_on = settings_get_bool_in(settings, GOG_SECTION, "low_voltage_alarm", 1);
    float threshold = (float) atof(settings_get_string_in(settings, GOG_SECTION, "min_cell_voltage", "3.4V"));
    g_alarm_active = (alarm_on && per_cell < threshold);
    lv_obj_set_style_text_color(g_lbl_battery, g_alarm_active ? COLOR_WARN : COLOR_TEXT, 0);

    if (telemetry->cell_count > 0) {
        lv_label_set_text_fmt(g_lbl_battery, "%s %d.%dV (%dS)", battery_icon_for(per_cell),
                              volts_tenths / 10, volts_tenths % 10, telemetry->cell_count);
    } else {
        lv_label_set_text_fmt(g_lbl_battery, "%s %d.%dV", battery_icon_for(per_cell),
                              volts_tenths / 10, volts_tenths % 10);
    }
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
        lv_label_set_text_fmt(g_lbl_link, "%s %d dB", LV_SYMBOL_WIFI, snr);
        /* Map SNR onto 0..SIGNAL_MAX bars for the colour only: ~0 dB red .. >=20 dB green. */
        int level = snr <= 0 ? 0 : snr >= 20 ? SIGNAL_MAX : snr / 5;
        lv_obj_set_style_text_color(g_lbl_link, signal_color(level, SIGNAL_MAX), 0);
    } else {
        /* Down/no-reading: dim dashes like the other fields; the warn colour alone flags the state. */
        lv_label_set_text_fmt(g_lbl_link, "%s -- dB", LV_SYMBOL_WIFI);
        lv_obj_set_style_text_color(g_lbl_link, COLOR_WARN, 0);
    }

    int distance = linkstate_distance_m();
    if (connected && distance != MLM_LINKINFO_NONE) {
        lv_label_set_text_fmt(g_lbl_distance, "%s %d m", LV_SYMBOL_GPS, distance);
        lv_obj_set_style_text_color(g_lbl_distance, COLOR_TEXT, 0);
    } else {
        lv_label_set_text_fmt(g_lbl_distance, "%s -- m", LV_SYMBOL_GPS);
        lv_obj_set_style_text_color(g_lbl_distance, COLOR_TEXT_DIM, 0);
    }

    /* Standby cue: the power glyph, shown only when the air reports standby (quad disarmed +
     * standby armed). Hidden otherwise, so the bar is unchanged when the air is active. */
    if (connected && linkstate_standby()) {
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
        lv_label_set_text_fmt(g_lbl_quad_battery, "%s %d.%02dV", LV_SYMBOL_BATTERY_2, cv / 100, cv % 100);
        lv_obj_set_style_text_color(g_lbl_quad_battery, COLOR_TEXT, 0);
    } else {
        lv_label_set_text_fmt(g_lbl_quad_battery, "%s --.-V", LV_SYMBOL_BATTERY_2);
        lv_obj_set_style_text_color(g_lbl_quad_battery, COLOR_TEXT_DIM, 0);
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

    int connected = linkstate_airunit_connected();
    int show_temp = settings_get_bool_in(settings, GOG_SECTION, "show_temperature", 1);

    update_goggle_battery(telemetry, settings);
    update_link_fields(connected, settings);
    update_air_fields(air, show_temp);

    if (telemetry->have_sdcard) {
        lv_label_set_text_fmt(g_lbl_sdcard, "%s %dG", LV_SYMBOL_SD_CARD, (int) (telemetry->sd_free_gb + 0.5f));
    } else {
        lv_label_set_text(g_lbl_sdcard, LV_SYMBOL_SD_CARD " --");
    }

    /* Incoming-video bitrate: driven by the actual sdio0 byte flow (telemetry.c smooths it and holds
     * the last value across brief gaps), NOT the link-liveness flag - the air throttles its status
     * cadence in standby, which would otherwise blank a steadily-arriving video rate. */
    if (telemetry->have_bitrate) {
        int tenths = (int) (telemetry->bitrate_mbps * 10.0f + 0.5f);
        lv_label_set_text_fmt(g_lbl_bitrate, "%s %d.%d Mbps", LV_SYMBOL_DOWNLOAD, tenths / 10, tenths % 10);
        lv_obj_set_style_text_color(g_lbl_bitrate, COLOR_TEXT, 0);
    } else {
        lv_label_set_text_fmt(g_lbl_bitrate, "%s --.- Mbps", LV_SYMBOL_DOWNLOAD);
        lv_obj_set_style_text_color(g_lbl_bitrate, COLOR_TEXT_DIM, 0);
    }

    /* Goggle SoC temperature, shown only when the Show Temperature setting is on and a reading exists. */
    if (show_temp && telemetry->have_temp) {
        lv_label_set_text_fmt(g_lbl_temp, "%d°C", telemetry->temp_c);
        lv_obj_remove_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    }
}
