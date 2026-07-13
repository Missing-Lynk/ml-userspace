/** @file menu.c @brief See menu.h. */
#include "menu.h"
#include "display.h"
#include "sysosd.h"
#include "tone.h"

#include "backlight.h"
#include "buzzer.h"
#include "i18n.h"
#include "linkstate.h"
#include "recordings.h"

#include "lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Palette + sizes, matching the libre menu layout (libre/app/menu_config.h). */
#define COLOR_BG             lv_color_hex(0x0A0E14)
#define COLOR_SIDEBAR        lv_color_hex(0x131A26)
#define COLOR_ACCENT         lv_color_hex(0x29ABE2)
#define COLOR_TEXT           lv_color_hex(0xE6EAF0)
#define COLOR_TEXT_DIM       lv_color_hex(0x7A8497)
#define COLOR_TEXT_ON_ACCENT lv_color_hex(0x05121C)
#define SIDEBAR_WIDTH_PCT    20
#define SIDEBAR_PAD          22
#define CONTENT_PAD          36
#define MENU_ROW_GAP         16
#define ITEM_RADIUS          12
#define ITEM_PAD_VER         22
#define ITEM_PAD_HOR         24
/* OSD_BAR_HEIGHT (the bottom strip reserved for the System OSD) comes from sysosd.h. */

#define GOG_SECTION          "goggle"   /* every Goggles-menu setting lives in this JSON section */
#define DVR_SECTION          "dvr"      /* every DVR-menu setting lives in this JSON section */

/* item model */
typedef enum {
    ITEM_STEPPER,   /* value cycled through a fixed option list, LEFT/RIGHT steps it */
    ITEM_TOGGLE,    /* on/off switch, CENTER flips it */
    ITEM_DROPDOWN,  /* CENTER opens a select-list overlay; applied on accept, not on back */
    ITEM_ACTION,    /* CENTER opens a confirm overlay; runs the action on confirm */
} item_type_t;

typedef struct {
    item_type_t        type;
    const char        *title_key;     /* i18n key */
    const char        *setting_key;   /* settings JSON key */
    const char *const *options;       /* stepper: NULL-terminated option list, else NULL */
    int                default_index; /* stepper: starting option index */
    int                default_on;    /* toggle: starting state */
    const char        *action;        /* apply hook */
} gog_item_t;

static const char *const brightness_options[] = { "10%", "20%", "30%", "40%", "50%",
                                                  "60%", "70%", "80%", "90%", "100%", NULL };
static const char *const buzzer_options[]    = { "Off", "1", "2", "3", "4", "5",
                                                 "6", "7", "8", "9", "10", NULL };
static const char *const cell_volt_options[] = {
    "2.5V", "2.6V", "2.7V", "2.8V", "2.9V", "3.0V", "3.1V", "3.2V", "3.3V",
    "3.4V", "3.5V", "3.6V", "3.7V", "3.8V", "3.9V", "4.0V", "4.1V", "4.2V", NULL };
static const char *const msp_osd_options[]   = { "None", "BTFL", NULL };
static const char *const language_options[]  = { "English", "中文", NULL };

static const gog_item_t g_items[] = {
    { ITEM_STEPPER, "goggles.brightness",        "brightness",        brightness_options, 5, 0, "brightness" },
    { ITEM_STEPPER, "goggles.msp_osd",           "msp_osd",           msp_osd_options,    1, 0, "" },
    { ITEM_TOGGLE,  "goggles.show_system_osd",   "show_system_osd",   NULL,               0, 1, "" },
    { ITEM_TOGGLE,  "goggles.show_temperature",  "show_temperature",  NULL,               0, 1, "" },
    { ITEM_STEPPER, "goggles.buzzer_volume",     "buzzer_volume",     buzzer_options,     5, 0, "buzzer" },
    { ITEM_TOGGLE,  "goggles.key_tones_off",     "key_tones_off",     NULL,               0, 0, "key_tones" },
    { ITEM_TOGGLE,  "goggles.low_voltage_alarm", "low_voltage_alarm", NULL,               0, 1, "alarm" },
    { ITEM_STEPPER,  "goggles.min_cell_voltage", "min_cell_voltage",  cell_volt_options,  9, 0, "alarm_voltage" },
    { ITEM_DROPDOWN, "goggles.language",         "language",          language_options,   0, 0, "language" },
    { ITEM_ACTION,   "goggles.slot_switch",      NULL,                NULL,               0, 0, "slot_switch" },
};
#define GOG_ITEM_COUNT ((int) (sizeof(g_items) / sizeof(g_items[0])))

static const char *const resolution_options[] = { "1080p 60fps", "1080p 30fps",
                                                  "720p 60fps", "720p 30fps", NULL };

static const gog_item_t g_dvr_items[] = {
    { ITEM_TOGGLE,   "dvr.autostart",   "autostart",  NULL,               0, 0, "" },
    { ITEM_DROPDOWN, "dvr.resolution",  "resolution", resolution_options, 0, 0, "" },
    { ITEM_TOGGLE,   "dvr.record_osd",  "record_osd", NULL,               0, 0, "" },
    { ITEM_TOGGLE,   "dvr.save_srt",    "save_srt",   NULL,               0, 0, "" },
    { ITEM_ACTION,   "dvr.format",      NULL,         NULL,               0, 0, "format" },
};
#define DVR_ITEM_COUNT ((int) (sizeof(g_dvr_items) / sizeof(g_dvr_items[0])))

/* Catalog search order: an on-device override, the dev staging dir, the shipped rootfs path, then
 * the build tree.
 */
static const char *const LANG_DIRS[] = {
    "/usrdata/hud/lang",
    "/run/ml/hud/lang",
    "/usr/local/share/hud/lang",
    "lang",
};

/* sidebar sections */
typedef enum {
    SECTION_GOGGLES = 0,   /* the goggle settings list */
    SECTION_DVR,           /* the DVR settings list */
    SECTION_AIRUNIT,       /* air-unit settings; gated on a live RF link (dimmed when down) */
    SECTION_PLAYBACK,      /* the SD-card recordings list */
    NUM_SECTIONS,
} section_t;

#define MAX_RECORDINGS 64

static const char *const g_nav_icons[NUM_SECTIONS] = {
    LV_SYMBOL_SETTINGS, LV_SYMBOL_SD_CARD, LV_SYMBOL_WIFI, LV_SYMBOL_VIDEO };
static const char *const g_nav_keys[NUM_SECTIONS]  = {
    "nav.goggles", "nav.dvr", "nav.air_unit", "nav.playback" };

/* state */
static settings_t  *g_settings;
static lv_group_t  *g_group;
static lv_indev_t  *g_keypad;
static lv_obj_t    *g_menu;                       /* top area (sidebar + content), NULL when closed */
static lv_obj_t    *g_sidebar;
static lv_obj_t    *g_content;                     /* content pane, rebuilt per section */
static lv_obj_t    *g_sidebar_buttons[NUM_SECTIONS];
static lv_obj_t    *g_rows[GOG_ITEM_COUNT];        /* the Goggles rows, NULL while another section shows */
static int          g_section;                     /* SECTION_* */
static int          g_content_focusable;           /* 0 = hint screen: keep focus in the sidebar */
static int          g_zone;                        /* 0 = sidebar, 1 = content */
static int          g_is_open;

static lv_obj_t          *g_select_box;            /* the select-list modal backdrop, or NULL */
static const gog_item_t  *g_select_item;           /* the dropdown being edited, NULL for a confirm */
static lv_obj_t          *g_select_row;            /* the content row that opened the overlay */
static int                g_select_open;
static int                g_select_closing;        /* a deferred close is scheduled */
static void             (*g_confirm_fn)(void);     /* action to run when a confirm overlay is accepted */

static lv_font_t    g_menu_font;                   /* Montserrat with the CJK fallback */
static lv_style_t   g_style_item;
static lv_style_t   g_style_item_focused;
static lv_style_t   g_style_sidebar_active;        /* active section while focus is in the content */

extern const lv_font_t font_zh;                    /* baked CJK subset (services/font_zh.c) */

static void set_language(const char *option);
static void apply_item(const gog_item_t *item, const char *value);
static void open_select(const gog_item_t *item, lv_obj_t *row);
static void open_confirm(const char *prompt, const char *confirm_label, void (*fn)(void), lv_obj_t *row);
static void slot_switch_to_a(void);
static void format_sdcard(void);
static void close_select(void);
static void render_content(void);
static void render_centered_hint(const char *text);
static void enter_sidebar_zone(void);
static void enter_content_zone(void);
static int  menu_is_linked(void);
static void update_air_unit_dim(void);

/* keypad injection: the controller feeds discrete key edges into this queue; the LVGL keypad
 * indev drains one per read, so a queued press then release becomes a navigation/click/key event.
 */
typedef struct {
    uint32_t          key;
    lv_indev_state_t  state;
} key_evt_t;

#define KEYQ_CAP 32
static key_evt_t g_keyq[KEYQ_CAP];
static int       g_keyq_head, g_keyq_tail;

static void keyq_push(uint32_t key, lv_indev_state_t state)
{
    int next = (g_keyq_tail + 1) % KEYQ_CAP;
    if (next == g_keyq_head) {
        return;   /* full: drop */
    }

    g_keyq[g_keyq_tail].key = key;
    g_keyq[g_keyq_tail].state = state;
    g_keyq_tail = next;
}

static int keyq_pop(key_evt_t *out)
{
    if (g_keyq_head == g_keyq_tail) {
        return 0;
    }

    *out = g_keyq[g_keyq_head];
    g_keyq_head = (g_keyq_head + 1) % KEYQ_CAP;
    return 1;
}

static void keypad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void) indev;
    static key_evt_t last = { 0, LV_INDEV_STATE_RELEASED };
    key_evt_t event;
    if (keyq_pop(&event)) {
        last = event;
    }

    data->key = last.key;
    data->state = last.state;
}

static void feed_key(uint32_t key)
{
    keyq_push(key, LV_INDEV_STATE_PRESSED);
    keyq_push(key, LV_INDEV_STATE_RELEASED);
}

/* The JSON section for the settings list currently shown. Item reads/writes go here; correct because
 * a row can only be touched while its own section is displayed.
 */
static const char *section_key(void)
{
    return g_section == SECTION_DVR ? DVR_SECTION : GOG_SECTION;
}

/* option helpers */
static int option_count(const char *const *options)
{
    int count = 0;
    if (options == NULL) {
        return 0;
    }

    while (options[count] != NULL) {
        count++;
    }

    return count;
}

static int option_index_of(const char *const *options, const char *value)
{
    if (options == NULL || value == NULL) {
        return -1;
    }

    for (int i = 0; options[i] != NULL; i++) {
        if (strcmp(options[i], value) == 0) {
            return i;
        }
    }

    return -1;
}

/* The stored option index for a stepper: the saved text, else the default. */
static int stepper_index(const gog_item_t *item)
{
    int count = option_count(item->options);
    int index = option_index_of(item->options, settings_get_string_in(g_settings, section_key(), item->setting_key, NULL));
    if (index < 0) {
        index = item->default_index;
    }

    if (index < 0 || index >= count) {
        index = 0;
    }

    return index;
}

static int toggle_on(const gog_item_t *item)
{
    return settings_get_bool_in(g_settings, section_key(), item->setting_key, item->default_on);
}

/* styles + fonts */
static void fonts_init(void)
{
    g_menu_font = lv_font_montserrat_48;
    g_menu_font.fallback = &font_zh;
}

static void styles_init(void)
{
    lv_style_init(&g_style_item);
    lv_style_set_bg_opa(&g_style_item, LV_OPA_TRANSP);
    lv_style_set_text_color(&g_style_item, COLOR_TEXT);
    lv_style_set_text_font(&g_style_item, &g_menu_font);
    lv_style_set_radius(&g_style_item, ITEM_RADIUS);
    lv_style_set_border_width(&g_style_item, 0);
    lv_style_set_pad_ver(&g_style_item, ITEM_PAD_VER);
    lv_style_set_pad_hor(&g_style_item, ITEM_PAD_HOR);

    lv_style_init(&g_style_item_focused);
    lv_style_set_bg_opa(&g_style_item_focused, LV_OPA_COVER);
    lv_style_set_bg_color(&g_style_item_focused, COLOR_ACCENT);
    lv_style_set_text_color(&g_style_item_focused, COLOR_TEXT_ON_ACCENT);

    lv_style_init(&g_style_sidebar_active);   /* muted fill: the section shown while focus is in content */
    lv_style_set_bg_opa(&g_style_sidebar_active, LV_OPA_40);
    lv_style_set_bg_color(&g_style_sidebar_active, COLOR_ACCENT);
}

/* row renderers */
/* A full-width focusable row: optional icon + title on the left, callers add value cells on the right. */
static lv_obj_t *make_row(const char *icon, const char *title)
{
    lv_obj_t *button = lv_button_create(g_content);
    lv_obj_remove_style_all(button);                 /* must precede set_width: it wipes local styles */
    lv_obj_set_width(button, lv_pct(100));
    lv_obj_add_style(button, &g_style_item, 0);
    lv_obj_add_style(button, &g_style_item_focused, LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title_label = lv_label_create(button);
    if (icon != NULL) {
        lv_label_set_text_fmt(title_label, "%s  %s", icon, title);
    } else {
        lv_label_set_text(title_label, title);
    }

    lv_obj_set_flex_grow(title_label, 1);   /* take the row width left of the value */
    return button;
}

/* A caret is bright when that direction is available, dimmed at the end of the range. */
static void set_caret(lv_obj_t *caret, int active)
{
    lv_obj_set_style_text_color(caret, active ? COLOR_TEXT : COLOR_TEXT_DIM, 0);
}

static void refresh_stepper_row(lv_obj_t *row, const gog_item_t *item)
{
    int count = option_count(item->options);
    int index = stepper_index(item);
    lv_label_set_text(lv_obj_get_child(row, 2), count > 0 ? item->options[index] : "");
    set_caret(lv_obj_get_child(row, 1), index > 0);
    set_caret(lv_obj_get_child(row, 3), index < count - 1);
}

/* A stepper row's right side: "left-caret  value  right-caret". */
static void add_stepper_value(lv_obj_t *row, const gog_item_t *item)
{
    lv_obj_t *left = lv_label_create(row);
    lv_label_set_text(left, LV_SYMBOL_LEFT);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_pad_hor(value, 14, 0);

    lv_obj_t *right = lv_label_create(row);
    lv_label_set_text(right, LV_SYMBOL_RIGHT);

    refresh_stepper_row(row, item);
}

/* A dropdown row's right side: "value  down-caret". The caret signals CENTER opens a select list. */
static void refresh_dropdown_row(lv_obj_t *row, const gog_item_t *item)
{
    int count = option_count(item->options);
    int index = stepper_index(item);
    lv_label_set_text(lv_obj_get_child(row, 1), count > 0 ? item->options[index] : "");
}

static void add_dropdown_value(lv_obj_t *row, const gog_item_t *item)
{
    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_pad_hor(value, 14, 0);

    lv_obj_t *caret = lv_label_create(row);
    lv_label_set_text(caret, LV_SYMBOL_DOWN);

    refresh_dropdown_row(row, item);
}

/* An action row's right side: a caret signalling CENTER opens a confirm overlay. */
static void add_action_value(lv_obj_t *row)
{
    lv_obj_t *caret = lv_label_create(row);
    lv_label_set_text(caret, LV_SYMBOL_RIGHT);
}

/* A toggle row's right side: an on/off switch (CENTER/LEFT/RIGHT drive it, not a tap). */
static void add_toggle_switch(lv_obj_t *row, int on)
{
    lv_obj_t *toggle = lv_switch_create(row);
    lv_obj_remove_flag(toggle, LV_OBJ_FLAG_CLICKABLE);
    if (on) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
}

/* value changes */
static void set_stepper_index(lv_obj_t *row, const gog_item_t *item, int index)
{
    settings_set_string_in(g_settings, section_key(), item->setting_key, item->options[index]);
    apply_item(item, item->options[index]);
    refresh_stepper_row(row, item);
}

static void set_toggle(lv_obj_t *row, const gog_item_t *item, int on)
{
    settings_set_bool_in(g_settings, section_key(), item->setting_key, on);
    apply_item(item, on ? "on" : "off");
    if (on) {
        lv_obj_add_state(lv_obj_get_child(row, 1), LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(lv_obj_get_child(row, 1), LV_STATE_CHECKED);
    }
}

/* Push one setting's value to the hardware. Alarm/key-tone settings only persist; the loop reads
 * them live.
 */
static void apply_item(const gog_item_t *item, const char *value)
{
    if (strcmp(item->action, "brightness") == 0) {
        backlight_set_percent(atoi(value));   /* atoi("60%") -> 60 */
    } else if (strcmp(item->action, "buzzer") == 0) {
        buzzer_set_volume(atoi(value));   /* "Off" -> 0, "1".."10" */
        tone_beep(lv_tick_get());         /* confirm the new volume with a beep */
    } else if (strcmp(item->action, "language") == 0) {
        set_language(value);
    }
}

/* language */
static void load_catalog(const char *code)
{
    for (unsigned i = 0; i < sizeof(LANG_DIRS) / sizeof(LANG_DIRS[0]); i++) {
        if (i18n_load_language(LANG_DIRS[i], code) == 0) {
            return;
        }
    }

    if (strcmp(code, "en") != 0) {
        load_catalog("en");   /* fall back to English if the requested catalog is missing */
    }
}

/* Re-apply the labels from the current catalog in place (no rebuild, so focus is kept). The Goggles
 * rows exist only while that section is shown (language can only change from there).
 */
static void relabel(void)
{
    for (int i = 0; i < NUM_SECTIONS; i++) {
        if (g_sidebar_buttons[i] != NULL) {
            lv_label_set_text_fmt(lv_obj_get_child(g_sidebar_buttons[i], 0), "%s   %s",
                                  g_nav_icons[i], T(g_nav_keys[i]));
        }
    }

    for (int i = 0; i < GOG_ITEM_COUNT; i++) {
        if (g_rows[i] != NULL) {
            lv_label_set_text(lv_obj_get_child(g_rows[i], 0), T(g_items[i].title_key));
        }
    }
}

static void set_language(const char *option)
{
    const char *code = (option != NULL && strcmp(option, "中文") == 0) ? "zh" : "en";
    load_catalog(code);
    relabel();
}

/* events (fired by LVGL as it drains injected keys) */
static void item_clicked_cb(lv_event_t *event)
{
    const gog_item_t *item = (const gog_item_t *) lv_event_get_user_data(event);
    lv_obj_t *row = (lv_obj_t *) lv_event_get_target(event);

    if (item->type == ITEM_TOGGLE) {
        set_toggle(row, item, !toggle_on(item));
    } else if (item->type == ITEM_DROPDOWN) {
        open_select(item, row);   /* CENTER opens the select-list overlay */
    } else if (item->type == ITEM_ACTION && strcmp(item->action, "slot_switch") == 0) {
        open_confirm(T("slot_switch.prompt"), T("goggles.slot_switch"), slot_switch_to_a, row);
    } else if (item->type == ITEM_ACTION && strcmp(item->action, "format") == 0) {
        open_confirm(T("dvr.format_prompt"), T("dvr.format"), format_sdcard, row);
    }
    /* Steppers ignore CENTER: they change only via LEFT/RIGHT (item_key_cb). */
}

static void item_key_cb(lv_event_t *event)
{
    uint32_t key = lv_event_get_key(event);
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
        return;
    }

    const gog_item_t *item = (const gog_item_t *) lv_event_get_user_data(event);
    lv_obj_t *row = (lv_obj_t *) lv_event_get_target(event);

    if (item->type == ITEM_TOGGLE) {
        set_toggle(row, item, key == LV_KEY_RIGHT);   /* LEFT = off, RIGHT = on */
    } else if (item->type == ITEM_STEPPER) {          /* dropdowns ignore LEFT/RIGHT: CENTER opens the list */
        int count = option_count(item->options);
        int index = stepper_index(item);
        if (key == LV_KEY_RIGHT && index < count - 1) {
            set_stepper_index(row, item, index + 1);
        } else if (key == LV_KEY_LEFT && index > 0) {
            set_stepper_index(row, item, index - 1);
        }
    }
}

/* build */
static void build_chrome(void)
{
    g_menu = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_menu);
    lv_obj_set_size(g_menu, lv_pct(100), ui_display_height() - OSD_BAR_HEIGHT);
    lv_obj_align(g_menu, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(g_menu, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_menu, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(g_menu, LV_FLEX_FLOW_ROW);

    g_sidebar = lv_obj_create(g_menu);
    lv_obj_remove_style_all(g_sidebar);
    lv_obj_set_size(g_sidebar, lv_pct(SIDEBAR_WIDTH_PCT), lv_pct(100));
    lv_obj_set_style_bg_color(g_sidebar, COLOR_SIDEBAR, 0);
    lv_obj_set_style_bg_opa(g_sidebar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(g_sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_sidebar, SIDEBAR_PAD, 0);
    lv_obj_set_style_pad_row(g_sidebar, MENU_ROW_GAP, 0);

    g_content = lv_obj_create(g_menu);
    lv_obj_remove_style_all(g_content);
    lv_obj_set_flex_grow(g_content, 1);
    lv_obj_set_height(g_content, lv_pct(100));
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(g_content, CONTENT_PAD, 0);
    lv_obj_set_style_pad_row(g_content, MENU_ROW_GAP, 0);
}

/* Moving the sidebar selection previews that section in the content pane. */
static void sidebar_focused_cb(lv_event_t *event)
{
    g_section = (int) (intptr_t) lv_event_get_user_data(event);
    render_content();
}

static void build_sidebar(void)
{
    for (int i = 0; i < NUM_SECTIONS; i++) {
        lv_obj_t *button = lv_button_create(g_sidebar);
        lv_obj_remove_style_all(button);
        lv_obj_set_width(button, lv_pct(100));
        lv_obj_add_style(button, &g_style_item, 0);
        lv_obj_add_style(button, &g_style_item_focused, LV_STATE_FOCUSED);
        lv_obj_add_style(button, &g_style_sidebar_active, LV_STATE_CHECKED);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text_fmt(label, "%s   %s", g_nav_icons[i], T(g_nav_keys[i]));

        lv_obj_add_event_cb(button, sidebar_focused_cb, LV_EVENT_FOCUSED, (void *) (intptr_t) i);
        g_sidebar_buttons[i] = button;
    }

    update_air_unit_dim();   /* start the link-gated entry dimmed unless the air unit is already up */
}

/* content renderers (each fills the cleared g_content with rows) */
/* A settings list: one widget row per item (stepper/toggle/dropdown/action). rows, if non-NULL,
 * receives the row objects for in-place relabel (used only by Goggles, the only list with a language
 * item). Reads/writes route through section_key(), so the same code serves any section's list.
 */
static void render_settings_list(const gog_item_t *items, int count, lv_obj_t **rows)
{
    for (int i = 0; i < count; i++) {
        const gog_item_t *item = &items[i];
        lv_obj_t *row = make_row(NULL, T(item->title_key));
        if (item->type == ITEM_STEPPER) {
            add_stepper_value(row, item);
        } else if (item->type == ITEM_DROPDOWN) {
            add_dropdown_value(row, item);
        } else if (item->type == ITEM_ACTION) {
            add_action_value(row);
        } else {
            add_toggle_switch(row, toggle_on(item));
        }

        lv_obj_add_event_cb(row, item_clicked_cb, LV_EVENT_CLICKED, (void *) item);
        lv_obj_add_event_cb(row, item_key_cb, LV_EVENT_KEY, (void *) item);
        if (rows != NULL) {
            rows[i] = row;
        }
    }
}

static void render_goggles(void)
{
    render_settings_list(g_items, GOG_ITEM_COUNT, g_rows);
}

static void render_dvr(void)
{
    render_settings_list(g_dvr_items, DVR_ITEM_COUNT, NULL);
}

/* The SD-card recordings list: one focusable row per clip (icon + name, size on the right). The rows
 * are navigable but CENTER does nothing. An empty list shows a hint and keeps focus in the sidebar.
 */
static void render_playback(void)
{
    static recording_t recordings[MAX_RECORDINGS];
    int count = recordings_list(recordings, MAX_RECORDINGS);

    if (count == 0) {
        render_centered_hint(T("playback.empty"));   /* nothing to focus: keep focus in the sidebar */
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = make_row(LV_SYMBOL_VIDEO, recordings[i].name);
        char size[24];
        long gb_tenths = (recordings[i].size_mb * 10 + 512) / 1024;   /* MB -> GB, one decimal */
        snprintf(size, sizeof(size), "%ld.%ld GB", gb_tenths / 10, gb_tenths % 10);
        lv_obj_t *size_label = lv_label_create(row);
        lv_label_set_text(size_label, size);
    }
}

/* A big centered dim message filling the content area (the no-link / placeholder states). Keeps focus
 * in the sidebar: a hint has nothing to enter.
 */
static void render_centered_hint(const char *text)
{
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *hint = lv_label_create(g_content);
    lv_label_set_text(hint, text);
    lv_obj_set_style_text_color(hint, COLOR_TEXT_DIM, 0);
    g_content_focusable = 0;
}

/* Whether the air unit is currently reachable (ml-linkd telemetry seam, non-blocking cached read). */
static int menu_is_linked(void)
{
    return linkstate_airunit_connected();
}

/* The air-unit section: gated on a live link. With no link it is a dim "no air unit" hint (and the
 * sidebar entry is dimmed too, update_air_unit_dim); linked, it shows a placeholder.
 */
static void render_air_unit(void)
{
    if (!menu_is_linked()) {
        render_centered_hint(T("air_unit.no_link"));
        return;
    }

    render_centered_hint(T("air_unit.connected"));
}

/* Dim the Air Unit sidebar entry when the link is down, so it reads as inactive until the air unit
 * connects. Cheap restyle only (no rebuild), safe to call on a timer while the menu is open.
 */
static void update_air_unit_dim(void)
{
    lv_opa_t opa = menu_is_linked() ? LV_OPA_COVER : LV_OPA_50;
    lv_obj_set_style_opa(g_sidebar_buttons[SECTION_AIRUNIT], opa, 0);
}

/* Clear the content pane and render the current section into it. Called with focus in the sidebar
 * (or before the first zone is entered), so the group never holds a row this deletes.
 */
static void render_content(void)
{
    lv_obj_clean(g_content);
    for (int i = 0; i < GOG_ITEM_COUNT; i++) {
        g_rows[i] = NULL;
    }

    g_content_focusable = 1;
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (g_section == SECTION_GOGGLES) {
        render_goggles();
    } else if (g_section == SECTION_DVR) {
        render_dvr();
    } else if (g_section == SECTION_AIRUNIT) {
        render_air_unit();
    } else {
        render_playback();
    }
}

/* zones */
static void enter_sidebar_zone(void)
{
    int section = g_section;   /* re-adding the buttons re-fires FOCUSED, which would overwrite it */
    g_zone = 0;
    lv_group_remove_all_objs(g_group);
    for (int i = 0; i < NUM_SECTIONS; i++) {
        lv_group_add_obj(g_group, g_sidebar_buttons[i]);
        lv_obj_remove_state(g_sidebar_buttons[i], LV_STATE_CHECKED);   /* focus shows it while here */
    }

    g_section = section;
    lv_group_focus_obj(g_sidebar_buttons[section]);
    update_air_unit_dim();   /* the link may have changed while focus was in the content */
}

static void enter_content_zone(void)
{
    uint32_t count = lv_obj_get_child_count(g_content);
    if (count == 0) {
        return;
    }

    g_zone = 1;
    lv_group_remove_all_objs(g_group);
    for (uint32_t i = 0; i < count; i++) {
        lv_group_add_obj(g_group, lv_obj_get_child(g_content, i));
    }

    lv_group_focus_obj(lv_obj_get_child(g_content, 0));

    /* Keep the active section marked in the sidebar while focus is in the content. */
    for (int i = 0; i < NUM_SECTIONS; i++) {
        if (i == g_section) {
            lv_obj_add_state(g_sidebar_buttons[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(g_sidebar_buttons[i], LV_STATE_CHECKED);
        }
    }
}

/* modal overlay: a full-screen dim with a centered box, used by the dropdown select-list
 * (ITEM_DROPDOWN) and the confirm prompt (ITEM_ACTION). The option/button rows become the keypad
 * group while open, so UP/DOWN navigate them and CENTER activates the focused one; BACK cancels. The
 * teardown is deferred (lv_async) so it is safe to trigger from a row's own click event.
 */
static void select_close_async(void *unused)
{
    (void) unused;
    if (g_select_box != NULL) {
        lv_obj_delete(g_select_box);
        g_select_box = NULL;
    }

    g_select_open = 0;
    g_select_closing = 0;
    if (g_is_open) {
        enter_content_zone();
        if (g_select_row != NULL) {
            lv_group_focus_obj(g_select_row);   /* land back on the row that opened the list */
        }
    }
}

static void close_select(void)
{
    if (g_select_open && !g_select_closing) {
        g_select_closing = 1;
        lv_async_call(select_close_async, NULL);
    }
}

/* Apply option `index`: persist it, push it live, then close. Saving happens only here (on accept). */
static void select_apply(int index)
{
    const gog_item_t *item = g_select_item;
    settings_set_string_in(g_settings, section_key(), item->setting_key, item->options[index]);
    apply_item(item, item->options[index]);
    if (g_select_row != NULL) {
        refresh_dropdown_row(g_select_row, item);
    }

    close_select();
}

static void select_option_clicked_cb(lv_event_t *event)
{
    select_apply((int) (intptr_t) lv_event_get_user_data(event));
}

/* The dim backdrop + centered box with an accent heading, shared by the select list and the confirm
 * prompt. Stores the backdrop in g_select_box; returns the inner box for callers to fill with rows.
 */
static lv_obj_t *make_modal(const char *heading_text)
{
    g_select_box = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_select_box);
    lv_obj_set_size(g_select_box, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(g_select_box, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_select_box, LV_OPA_70, 0);   /* dim the menu behind */

    lv_obj_t *box = lv_obj_create(g_select_box);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(60));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, COLOR_SIDEBAR, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, ITEM_RADIUS, 0);
    lv_obj_set_style_pad_all(box, CONTENT_PAD, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, MENU_ROW_GAP, 0);

    lv_obj_t *heading = lv_label_create(box);
    lv_label_set_text(heading, heading_text);
    lv_obj_set_style_text_font(heading, &g_menu_font, 0);
    lv_obj_set_style_text_color(heading, COLOR_ACCENT, 0);
    return box;
}

/* A full-width focusable button row inside a modal box. Added to the keypad group by the caller. */
static lv_obj_t *make_modal_button(lv_obj_t *box, const char *text)
{
    lv_obj_t *opt = lv_button_create(box);
    lv_obj_remove_style_all(opt);
    lv_obj_set_width(opt, lv_pct(100));
    lv_obj_add_style(opt, &g_style_item, 0);
    lv_obj_add_style(opt, &g_style_item_focused, LV_STATE_FOCUSED);
    lv_obj_set_flex_flow(opt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(opt, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(opt);
    lv_label_set_text(label, text);
    return opt;
}

static void open_select(const gog_item_t *item, lv_obj_t *row)
{
    g_select_item = item;
    g_select_row = row;

    lv_obj_t *box = make_modal(T(item->title_key));

    int count = option_count(item->options);
    int current = stepper_index(item);
    lv_group_remove_all_objs(g_group);   /* the option rows become the keypad sink */

    lv_obj_t *focus_target = NULL;
    for (int i = 0; i < count; i++) {
        lv_obj_t *opt = make_modal_button(box, item->options[i]);
        lv_obj_add_event_cb(opt, select_option_clicked_cb, LV_EVENT_CLICKED, (void *) (intptr_t) i);
        lv_group_add_obj(g_group, opt);
        if (i == current) {
            focus_target = opt;
        }
    }

    if (focus_target != NULL) {
        lv_group_focus_obj(focus_target);
    }

    g_select_open = 1;
}

/* Confirm-overlay accept: user_data 1 = confirm (run the action), 0 = cancel. The action reboots on
 * success and so does not return; close the overlay if it does (the action failed).
 */
static void confirm_button_cb(lv_event_t *event)
{
    if ((intptr_t) lv_event_get_user_data(event) == 1 && g_confirm_fn != NULL) {
        g_confirm_fn();
    }

    close_select();
}

/* A two-button confirm modal: Cancel (focused by default) and confirm_label. Reuses the select
 * overlay's state + deferred teardown; g_select_item is NULL so the teardown treats it as non-list.
 */
static void open_confirm(const char *prompt, const char *confirm_label, void (*fn)(void), lv_obj_t *row)
{
    g_select_item = NULL;
    g_select_row = row;
    g_confirm_fn = fn;

    lv_obj_t *box = make_modal(prompt);
    lv_group_remove_all_objs(g_group);

    lv_obj_t *cancel = make_modal_button(box, T("common.cancel"));
    lv_obj_add_event_cb(cancel, confirm_button_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    lv_group_add_obj(g_group, cancel);

    lv_obj_t *ok = make_modal_button(box, confirm_label);
    lv_obj_add_event_cb(ok, confirm_button_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 1);
    lv_group_add_obj(g_group, ok);

    lv_group_focus_obj(cancel);   /* default to Cancel */
    g_select_open = 1;
}

/* Flip the GPT active boot slot to A and reset via the watchdog (plain reboot is a no-op on this
 * SoC). gpt0's mtd index varies, so it is found by name. Needs mtdtool + wdt-reset in /usr/local/bin.
 * Slot A is stock and untouched, so this is the always-safe direction.
 */
static void slot_switch_to_a(void)
{
    sync();
    system("g=$(grep '\"gpt0\"' /proc/mtd | cut -d: -f1); "
           "[ -n \"$g\" ] && /usr/local/bin/mtdtool setslot \"/dev/$g\" a && sync && "
           "/usr/local/bin/wdt-reset");
}

/* Reformat the SD card (whole-device exFAT, FAT32 fallback) and remount it, via ml-sdformat. Blocks
 * for the few seconds the format takes.
 */
static void format_sdcard(void)
{
    system("/usr/local/bin/ml-sdformat >/dev/null 2>&1");
}

/* lifecycle */
void menu_init(settings_t *settings)
{
    g_settings = settings;
    fonts_init();
    styles_init();

    g_group = lv_group_create();
    g_keypad = lv_indev_create();
    lv_indev_set_type(g_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(g_keypad, keypad_read_cb);
    lv_indev_set_group(g_keypad, g_group);
}

void menu_apply_persisted(void)
{
    set_language(settings_get_string_in(g_settings, GOG_SECTION, "language", "English"));

    const char *brightness = settings_get_string_in(g_settings, GOG_SECTION, "brightness", NULL);
    if (brightness != NULL) {
        backlight_set_percent(atoi(brightness));
    }

    /* Always set a buzzer volume (saved or the default) so key tones and the alarm can sound. */
    buzzer_set_volume(atoi(settings_get_string_in(g_settings, GOG_SECTION, "buzzer_volume", "5")));
}

void menu_open(void)
{
    if (g_is_open) {
        return;
    }

    build_chrome();
    build_sidebar();
    g_section = SECTION_GOGGLES;   /* a fresh open always starts at the top section */
    render_content();
    enter_sidebar_zone();
    g_is_open = 1;
}

void menu_close(void)
{
    if (g_select_box != NULL) {
        lv_obj_delete(g_select_box);
        g_select_box = NULL;
    }
    g_select_open = 0;
    g_select_closing = 0;

    if (g_menu != NULL) {
        lv_group_remove_all_objs(g_group);
        lv_obj_delete(g_menu);
        g_menu = NULL;
        g_sidebar = NULL;
        g_content = NULL;

        for (int i = 0; i < NUM_SECTIONS; i++) {
            g_sidebar_buttons[i] = NULL;
        }

        for (int i = 0; i < GOG_ITEM_COUNT; i++) {
            g_rows[i] = NULL;
        }
    }

    g_zone = 0;
    g_section = SECTION_GOGGLES;
    g_is_open = 0;
}

void menu_close_all(void)
{
    menu_close();
}

int menu_is_open(void)
{
    return g_is_open;
}

/* Refresh the Air Unit sidebar dim from the live link. Cheap + safe to call on a timer while the menu
 * is open: it only restyles the sidebar entry, never rebuilds focused content.
 */
void menu_refresh_link(void)
{
    if (g_is_open && g_sidebar != NULL) {
        update_air_unit_dim();
    }
}

int menu_depth(void)
{
    return g_zone;
}

void menu_back(void)
{
    if (!g_is_open) {
        return;
    }

    if (g_select_open) {
        close_select();   /* cancel the select list, no change saved */
        return;
    }

    if (g_zone == 1) {
        enter_sidebar_zone();
        return;
    }

    menu_close();
}

void menu_up(void)
{
    if (g_is_open) {
        feed_key(LV_KEY_PREV);
    }
}

void menu_down(void)
{
    if (g_is_open) {
        feed_key(LV_KEY_NEXT);
    }
}

void menu_left(void)
{
    if (g_is_open) {
        feed_key(LV_KEY_LEFT);
    }
}

void menu_right(void)
{
    if (g_is_open) {
        feed_key(LV_KEY_RIGHT);
    }
}

void menu_center(void)
{
    if (!g_is_open) {
        return;
    }

    if (g_zone == 0) {
        if (g_content_focusable) {   /* a hint screen (e.g. empty Playback) has nothing to enter */
            enter_content_zone();
        }
        return;
    }

    feed_key(LV_KEY_ENTER);
}
