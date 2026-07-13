/** @file sysosd.c @brief See sysosd.h. */
#include "sysosd.h"

#include <stdlib.h>
#include <string.h>

/* Palette + spacing, matching the menu/libre look. */
#define COLOR_OSD       lv_color_hex(0x0E141E)
#define COLOR_TEXT      lv_color_hex(0xE6EAF0)
#define COLOR_WARN      lv_color_hex(0xE0633A)
#define COLOR_REC       lv_color_hex(0xFF0000)   /* System OSD "REC" indicator (matches libre) */
#define OSD_PAD_HOR     44
#define OSD_FIELD_GAP   36
#define OSD_BATTERY_WIDTH 170   /* fixed (fits "25.2V (6S)"), so the value changing does not reflow */
#define OSD_FONT        (&lv_font_montserrat_28)

/* Low-battery blink: half-period of the battery-icon blink while the alarm is active. */
#define ALARM_TICK_MS   250

#define GOG_SECTION     "goggle"   /* the goggle settings section (mirrors menu.c) */

static lv_obj_t *g_osd;              /* the bar; child of the screen */
static lv_obj_t *g_group_left;       /* goggle telemetry group */
static lv_obj_t *g_lbl_battery;
static lv_obj_t *g_lbl_sdcard;
static lv_obj_t *g_lbl_temp;         /* SoC temperature; gated by the Show Temperature setting */
static lv_obj_t *g_lbl_rec;          /* red "REC"; shown only while ml-pipeline reports recording */

static int g_menu_open;
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

static lv_obj_t *add_field(lv_obj_t *group, const char *text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(group);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, OSD_FONT, 0);
    lv_obj_set_style_text_color(label, color, 0);

    return label;
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
    lv_obj_set_flex_align(g_osd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(g_osd, OSD_PAD_HOR, 0);
    lv_obj_set_style_bg_color(g_osd, COLOR_OSD, 0);
    lv_obj_set_style_bg_opa(g_osd, LV_OPA_TRANSP, 0);   /* transparent until the menu opens */

    g_group_left = lv_obj_create(g_osd);
    lv_obj_remove_style_all(g_group_left);
    lv_obj_remove_flag(g_group_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(g_group_left, LV_SIZE_CONTENT, lv_pct(100));
    lv_obj_set_flex_flow(g_group_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_group_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g_group_left, OSD_FIELD_GAP, 0);

    g_lbl_battery = add_field(g_group_left, LV_SYMBOL_BATTERY_FULL " --.-V", COLOR_TEXT);
    lv_obj_set_width(g_lbl_battery, OSD_BATTERY_WIDTH);   /* fixed width: no reflow on value change */
    g_lbl_sdcard = add_field(g_group_left, LV_SYMBOL_SD_CARD " --", COLOR_TEXT);
    g_lbl_temp = add_field(g_group_left, "--°C", COLOR_TEXT);
    lv_obj_add_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);   /* shown only when enabled and a reading exists */
    g_lbl_rec = add_field(g_group_left, "REC", COLOR_REC);
    lv_obj_add_flag(g_lbl_rec, LV_OBJ_FLAG_HIDDEN);    /* shown only while recording */

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

    if (recording) {
        lv_obj_remove_flag(g_lbl_rec, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_rec, LV_OBJ_FLAG_HIDDEN);
    }
}

void sysosd_invalidate(void)
{
    if (g_osd != NULL) {
        lv_obj_invalidate(g_osd);
    }
}

void sysosd_update(const telemetry_t *telemetry, settings_t *settings)
{
    if (g_osd == NULL) {
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

    /* LVGL's built-in printf has no %f, so format the floats with integer math. */
    if (telemetry->have_battery) {
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
    } else {
        g_alarm_active = 0;
        lv_label_set_text(g_lbl_battery, LV_SYMBOL_BATTERY_FULL " --.-V");
    }

    if (telemetry->have_sdcard) {
        lv_label_set_text_fmt(g_lbl_sdcard, "%s %dG", LV_SYMBOL_SD_CARD, (int) (telemetry->sd_free_gb + 0.5f));
    } else {
        lv_label_set_text(g_lbl_sdcard, LV_SYMBOL_SD_CARD " --");
    }

    /* Temperature: shown only when the Show Temperature setting is on and a reading exists. */
    int show_temp = settings_get_bool_in(settings, GOG_SECTION, "show_temperature", 1);
    if (show_temp && telemetry->have_temp) {
        lv_label_set_text_fmt(g_lbl_temp, "%d°C", telemetry->temp_c);
        lv_obj_remove_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(g_lbl_temp, LV_OBJ_FLAG_HIDDEN);
    }
}
