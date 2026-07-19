/** @file tempwarn.c @brief See tempwarn.h. */
#include "tempwarn.h"

#include "i18n.h"

#define WARN_COLOR    lv_color_hex(0xFF0000)
#define WARN_TOP_PAD  24     /* gap between the screen's top edge and the banner */
#define BLINK_MS      400    /* half-period of the blink; also the clobber-repair cadence */

static lv_obj_t  *g_warn;
static lv_font_t  g_warn_font;   /* Montserrat with the CJK fallback, for the zh catalog */
static int        g_active;
static int        g_blink_on;

extern const lv_font_t font_zh;  /* baked CJK subset (services/font_zh.c) */

/* Opacity (not hide) is toggled so every half-period repaints the label's rectangle, which also
 * restores the banner after a full BTFL present overwrote it on the shared plane.
 */
static void blink_tick_cb(lv_timer_t *timer)
{
    (void) timer;
    if (!g_active) {
        return;
    }

    g_blink_on = !g_blink_on;
    lv_obj_set_style_opa(g_warn, g_blink_on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void tempwarn_create(lv_obj_t *parent)
{
    g_warn_font = lv_font_montserrat_48;
    g_warn_font.fallback = &font_zh;

    g_warn = lv_label_create(parent);
    lv_obj_set_style_text_font(g_warn, &g_warn_font, 0);
    lv_obj_set_style_text_color(g_warn, WARN_COLOR, 0);
    lv_obj_align(g_warn, LV_ALIGN_TOP_MID, 0, WARN_TOP_PAD);
    lv_obj_add_flag(g_warn, LV_OBJ_FLAG_HIDDEN);

    lv_timer_create(blink_tick_cb, BLINK_MS, NULL);
}

void tempwarn_set_active(int active)
{
    if (g_warn == NULL || active == g_active) {
        return;
    }

    g_active = active;
    if (active) {
        lv_label_set_text(g_warn, T("warn.overheating"));
        g_blink_on = 1;
        lv_obj_set_style_opa(g_warn, LV_OPA_COVER, 0);
        lv_obj_remove_flag(g_warn, LV_OBJ_FLAG_HIDDEN);

        return;
    }

    lv_obj_add_flag(g_warn, LV_OBJ_FLAG_HIDDEN);
}
