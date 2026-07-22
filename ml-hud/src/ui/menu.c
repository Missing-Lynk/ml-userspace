/** @file menu.c @brief See menu.h. */
#include "menu.h"
#include "menu_channel.h"
#include "display.h"
#include "player.h"
#include "sysosd.h"
#include "theme.h"
#include "tone.h"

#include "backlight.h"
#include "buzzer.h"
#include "i18n.h"
#include "linkcmd.h"
#include "pipecmd.h"
#include "recordings.h"

#include "../../../ml-shared/mlm.h"   /* MLM_CAM_* selectors for the camera overlay */

#include "lvgl.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The palette (COLOR_*) lives in theme.h, shared with the player. Menu layout sizes: */
#define SIDEBAR_WIDTH_PCT    20
#define SIDEBAR_PAD          22
#define CONTENT_PAD          36
#define MENU_ROW_GAP         16
#define ITEM_RADIUS          12
#define ITEM_PAD_VER         22
#define ITEM_PAD_HOR         24
/* OSD_BAR_HEIGHT (the bottom strip reserved for the System OSD) comes from sysosd.h. */

#define GOG_SECTION          "goggle"   /* backs both the Goggles and System menus (see section_key) */
#define DVR_SECTION          "dvr"      /* every DVR-menu setting lives in this JSON section */
#define AIRUNIT_SECTION      "air_unit" /* every Air-Unit-menu setting lives in this JSON section */

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
static const char *const band_options[]      = { "Race", "Normal", NULL };

/* The RF band marker ml-video reads at boot to pick the baseband config. Under /usrdata (the
 * usr_data UBI volume the ml-usrdata service mounts): it must outlive the reboot that applies it.
 */
#define BAND_MARKER_DIR  "/usrdata/missinglynk"
#define BAND_MARKER_PATH BAND_MARKER_DIR "/rf-band"

/* The Goggles section: display and on-screen-display settings only. */
static const gog_item_t g_goggles_items[] = {
    { ITEM_STEPPER, "goggles.brightness",       "brightness",       brightness_options, 5, 0, "brightness" },
    { ITEM_STEPPER, "goggles.msp_osd",          "msp_osd",          msp_osd_options,    1, 0, "" },
    { ITEM_TOGGLE,  "goggles.show_system_osd",      "show_system_osd",      NULL,           0, 1, "" },
    { ITEM_TOGGLE,  "goggles.show_temperature",     "show_temperature",     NULL,           0, 1, "" },
    { ITEM_TOGGLE,  "goggles.show_encoder_stats",   "show_encoder_stats",   NULL,           0, 0, "" },
    { ITEM_TOGGLE,  "goggles.show_link_throughput", "show_link_throughput", NULL,           0, 0, "" },
    { ITEM_TOGGLE,  "goggles.show_air_ontime",      "show_air_ontime",      NULL,           0, 0, "" },
};
#define GOGGLES_ITEM_COUNT ((int) (sizeof(g_goggles_items) / sizeof(g_goggles_items[0])))

/* The System section: everything that is not a display setting. Every value still persists in
 * GOG_SECTION (section_key defaults there for this section), so splitting the sidebar list re-keys
 * no saved setting and hud.c keeps reading band/language/etc. from the same JSON section.
 */
static const gog_item_t g_system_items[] = {
    { ITEM_STEPPER,  "system.buzzer_volume",     "buzzer_volume",     buzzer_options,    5, 0, "buzzer" },
    { ITEM_TOGGLE,   "system.key_tones_off",     "key_tones_off",     NULL,              0, 0, "key_tones" },
    { ITEM_TOGGLE,   "system.low_voltage_alarm", "low_voltage_alarm", NULL,              0, 1, "alarm" },
    { ITEM_STEPPER,  "system.min_cell_voltage",  "min_cell_voltage",  cell_volt_options, 9, 0, "alarm_voltage" },
    { ITEM_DROPDOWN, "system.language",          "language",          language_options,  0, 0, "language" },
    { ITEM_DROPDOWN, "system.band",              "band",              band_options,      0, 0, "band" },
    { ITEM_ACTION,   "system.slot_switch",       NULL,                NULL,              0, 0, "slot_switch" },
};
#define SYSTEM_ITEM_COUNT ((int) (sizeof(g_system_items) / sizeof(g_system_items[0])))

static const char *const resolution_options[] = { "1080p 60fps", "1080p 30fps",
                                                  "720p 60fps", "720p 30fps", NULL };

static const gog_item_t g_dvr_items[] = {
    { ITEM_TOGGLE,   "dvr.autostart",   "autostart",  NULL,               0, 0, "" },
    { ITEM_DROPDOWN, "dvr.resolution",  "resolution", resolution_options, 0, 0, "dvr_res" },
    { ITEM_TOGGLE,   "dvr.record_osd",  "record_osd", NULL,               0, 0, "" },
    { ITEM_TOGGLE,   "dvr.save_srt",    "save_srt",   NULL,               0, 0, "" },
    { ITEM_TOGGLE,   "dvr.rtsp_stream", "rtsp_stream", NULL,              0, 0, "rtsp" },
    { ITEM_ACTION,   "dvr.format",      NULL,         NULL,               0, 0, "format" },
};
#define DVR_ITEM_COUNT ((int) (sizeof(g_dvr_items) / sizeof(g_dvr_items[0])))

static const char *const power_options[]   = { "25 mW", "100 mW", "200 mW", NULL };

/* Overheat-banner threshold against the air's transmitted temperature; the vendor threshold is
 * 105. "Off" disables the banner (hud.c parses the label with atoi). */
static const char *const temp_warn_options[] = { "Off", "90°C", "95°C", "100°C",
                                                 "105°C", "110°C", "115°C", NULL };

/* Air-unit settings; stored on the goggle and latched by the air unit at association (render_air_unit). */
static const gog_item_t g_airunit_items[] = {
    { ITEM_STEPPER, "air_unit.power",     "power",       power_options,     1, 0, "power" },
    { ITEM_TOGGLE,  "air_unit.standby",   "standby",     NULL,              0, 1, "standby" },
    { ITEM_STEPPER, "air_unit.temp_warn", "temp_warn_c", temp_warn_options, 4, 0, "" },
    { ITEM_ACTION,  "air_unit.camera",    NULL,          NULL,              0, 0, "camera" },
};
#define AIRUNIT_ITEM_COUNT ((int) (sizeof(g_airunit_items) / sizeof(g_airunit_items[0])))

/* Camera overlay: the live-image-first camera settings strip. Each item is applied to the air unit
 * the moment it changes (SetCameraInfo / SetScaleMode over ml-linkd), so the point is to judge the
 * change against the live feed: the overlay hides the menu chrome and shows only this compact strip.
 * Values are ints persisted in AIRUNIT_SECTION; hud.c re-asserts them on every link-up edge
 * (menu_camera_assert), because a re-association resets the air's ISP to its SetLdCfg defaults.
 * Only the HW-captured items are exposed (plans/rf-air-config.md section 1). */
typedef struct {
    const char        *title_key;    /* i18n key */
    const char        *setting_key;  /* int setting in AIRUNIT_SECTION */
    const char *const *labels;       /* list item: NULL-terminated labels (run through T()), else NULL */
    const int         *values;       /* list item: the value behind each label */
    int                min, max, step; /* numeric item (labels == NULL) */
    int                def;          /* default VALUE (matches the air's cold-boot ISP state) */
    int                cam_sel;      /* MLM_CAM_* selector; 0 = the zoom/aspect SetScaleMode pair */
} cam_item_t;

static const char *const cam_exposure_labels[] = { "camera.auto", "1/60", "1/120", "1/250",
                                                   "1/500", "1/1000", NULL };
static const int         cam_exposure_values[] = { 0, 16666, 8333, 4000, 2000, 1000 };
static const char *const cam_rotation_labels[] = { "camera.rot_normal", "camera.rot_flipped", NULL };
static const int         cam_rotation_values[] = { 0, 1 };
static const char *const cam_onoff_labels[]    = { "common.off", "common.on", NULL };
static const int         cam_onoff_values[]    = { 0, 1 };
static const char *const cam_zoom_labels[]     = { "1.0x", "0.7x", NULL };
static const int         cam_zoom_values[]     = { 100, 70 };
static const char *const cam_aspect_labels[]   = { "16:9", "4:3", NULL };
static const int         cam_aspect_values[]   = { 0, 1 };

/* The zoom/aspect settings keys, named because cam_push_scale reads them outside the table (both
 * fields ride the ONE SetScaleMode message, so either item pushes the pair). */
#define CAM_KEY_ZOOM   "camera_zoom_pct"
#define CAM_KEY_ASPECT "camera_aspect"

static const cam_item_t g_camera_items[] = {
    { "camera.exposure",   "camera_exposure_us", cam_exposure_labels, cam_exposure_values, 0, 0, 0,
      MLM_CAM_DEF_EXPOSURE, MLM_CAM_EXPOSURE },
    { "camera.saturation", "camera_saturation",  NULL,                NULL,                0, 100, 5,
      MLM_CAM_DEF_SATURATION, MLM_CAM_SATURATION },
    { "camera.sharpness",  "camera_sharpness",   NULL,                NULL,                0, 100, 5,
      MLM_CAM_DEF_SHARPNESS, MLM_CAM_SHARPNESS },
    { "camera.rotation",   "camera_rotation",    cam_rotation_labels, cam_rotation_values, 0, 0, 0,
      MLM_CAM_DEF_ROTATION, MLM_CAM_ROTATION },
    { "camera.nr3d",       "camera_nr3d",        cam_onoff_labels,    cam_onoff_values,    0, 0, 0,
      MLM_CAM_DEF_NR3D, MLM_CAM_NR3D },
    { "camera.zoom",       CAM_KEY_ZOOM,         cam_zoom_labels,     cam_zoom_values,     0, 0, 0,
      MLM_CAM_DEF_ZOOM_PCT, 0 },
    { "camera.aspect",     CAM_KEY_ASPECT,       cam_aspect_labels,   cam_aspect_values,   0, 0, 0,
      MLM_CAM_DEF_ASPECT, 0 },
};
#define CAMERA_ITEM_COUNT ((int) (sizeof(g_camera_items) / sizeof(g_camera_items[0])))

/* compact strip metrics (the menu rows are too fat for an overlay meant to stay out of the image);
 * the panel width is computed from the widest title/value in the set (cam_panel_width), floored
 * here so a short catalog does not produce a sliver */
#define CAM_PANEL_MIN_WIDTH 360
#define CAM_PANEL_MARGIN    24
#define CAM_ROW_PAD_VER     10
#define CAM_VALUE_PAD_HOR   20

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
    SECTION_CHANNEL = 0,   /* the RF channel grid (scan overview) */
    SECTION_GOGGLES,       /* display + OSD settings */
    SECTION_SYSTEM,        /* everything else: audio, alarm, language, RF band, slot */
    SECTION_AIRUNIT,       /* air-unit settings; always editable, pushed to the air at association */
    SECTION_DVR,           /* the DVR settings list */
    SECTION_PLAYBACK,      /* the SD-card recordings list */
    NUM_SECTIONS,
} section_t;

#define MAX_RECORDINGS 64

static const char *const g_nav_icons[NUM_SECTIONS] = {
    LV_SYMBOL_LIST, LV_SYMBOL_IMAGE, LV_SYMBOL_SETTINGS, LV_SYMBOL_WIFI, LV_SYMBOL_SD_CARD,
    LV_SYMBOL_VIDEO };
static const char *const g_nav_keys[NUM_SECTIONS]  = {
    "nav.channel", "nav.goggles", "nav.system", "nav.air_unit", "nav.dvr", "nav.playback" };

/* state */
static settings_t  *g_settings;
static lv_group_t  *g_group;
static lv_indev_t  *g_keypad;
static lv_obj_t    *g_menu;                       /* top area (sidebar + content), NULL when closed */
static lv_obj_t    *g_sidebar;
static lv_obj_t    *g_content;                     /* content pane, rebuilt per section */
static lv_obj_t    *g_sidebar_buttons[NUM_SECTIONS];
static lv_obj_t    *g_rows[SYSTEM_ITEM_COUNT];     /* the System rows (the list with the language item), NULL while another section shows */
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
static const char        *g_notice_pending;        /* notice to raise once the current overlay is torn down */
static const char        *g_band_pending;          /* band option label awaiting its reboot confirm */
static int                g_band_confirm_due;      /* raise that confirm once the select list is gone */

static lv_obj_t          *g_cam_panel;             /* the camera overlay strip, NULL when closed */
static lv_obj_t          *g_cam_row;               /* the Air Unit row that opened it (return focus) */

static lv_font_t    g_menu_font;                   /* Montserrat with the CJK fallback */
static lv_style_t   g_style_item;
static lv_style_t   g_style_item_focused;
static lv_style_t   g_style_sidebar_active;        /* active section while focus is in the content */

extern const lv_font_t font_zh;                    /* baked CJK subset (services/font_zh.c) */

static void set_language(const char *option);
static void apply_item(const gog_item_t *item, const char *value);
static void open_select(const gog_item_t *item, lv_obj_t *row);
static void open_confirm(const char *prompt, const char *confirm_label, void (*fn)(void), lv_obj_t *row);
static void open_notice(const char *prompt);
static void band_confirm_apply(void);
static void camera_open(lv_obj_t *row);
static void camera_close(void);
static lv_obj_t *make_modal_button(lv_obj_t *box, const char *text);
static void slot_switch_to_a(void);
static void format_sdcard(void);
static void close_select(void);
static void render_content(void);
static void render_centered_hint(const char *text);
static void enter_sidebar_zone(void);
static void enter_content_zone(void);

/* The recordings player (transport bar + playback state) lives in player.c; menu.c drives it through
 * player.h and hands it the shared menu objects via g_player_host below. */

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
 * a row can only be touched while its own section is displayed. Goggles and System both fall through
 * to GOG_SECTION: the split is presentational, so a setting keeps persisting where it always did.
 */
static const char *section_key(void)
{
    if (g_section == SECTION_DVR) {
        return DVR_SECTION;
    }

    if (g_section == SECTION_AIRUNIT) {
        return AIRUNIT_SECTION;
    }

    return GOG_SECTION;
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

static bool toggle_is_on(const gog_item_t *item)
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

/* The "left-caret  value  right-caret" triplet, appended to @p parent as three labels. Used by the
 * settings-list stepper rows and the camera strip's value lines. @p value_width >= 0 fixes the
 * value label to that text width (centered), so the carets never move as the value steps; < 0
 * leaves it content-sized. */
static void add_caret_value_labels(lv_obj_t *parent, int32_t pad_hor, int32_t value_width)
{
    lv_obj_t *left = lv_label_create(parent);
    lv_label_set_text(left, LV_SYMBOL_LEFT);

    lv_obj_t *value = lv_label_create(parent);
    lv_obj_set_style_pad_hor(value, pad_hor, 0);
    if (value_width >= 0) {
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(value, value_width + 2 * pad_hor);
    }

    lv_obj_t *right = lv_label_create(parent);
    lv_label_set_text(right, LV_SYMBOL_RIGHT);
}

/* A stepper row's right side: "left-caret  value  right-caret". */
static void add_stepper_value(lv_obj_t *row, const gog_item_t *item)
{
    add_caret_value_labels(row, 14, -1);

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
        /* "Off" -> 0, "1".."10" */
        buzzer_set_volume(atoi(value));

        /* confirm the new volume with a beep */
        tone_beep();
    } else if (strcmp(item->action, "language") == 0) {
        set_language(value);
    } else if (strcmp(item->action, "standby") == 0) {
        /* arm/disarm the air unit's standby */
        linkcmd_set_standby(strcmp(value, "on") == 0);
    } else if (strcmp(item->action, "power") == 0) {
        /* the level label ("100 mW"); linkcmd maps it to mW */
        linkcmd_set_power(value);
    } else if (strcmp(item->action, "rtsp") == 0) {
        /* the pipeline brings the restream (and, if needed, a file-less encoder) up or down */
        pipecmd_set_rtsp(strcmp(value, "on") == 0);
    } else if (strcmp(item->action, "dvr_res") == 0) {
        /* the option label ("720p 30fps"); the pipeline latches it for the next recording */
        int height = 1080;
        int fps = 60;
        sscanf(value, "%dp %dfps", &height, &fps);
        pipecmd_set_dvr_res(height, fps);
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

/* Re-apply the labels from the current catalog in place (no rebuild, so focus is kept). The System
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

    for (int i = 0; i < SYSTEM_ITEM_COUNT; i++) {
        if (g_rows[i] != NULL) {
            lv_label_set_text(lv_obj_get_child(g_rows[i], 0), T(g_system_items[i].title_key));
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
        set_toggle(row, item, !toggle_is_on(item));
    } else if (item->type == ITEM_DROPDOWN) {
        open_select(item, row);   /* CENTER opens the select-list overlay */
    } else if (item->type == ITEM_ACTION && strcmp(item->action, "slot_switch") == 0) {
        open_confirm(T("slot_switch.prompt"), T("system.slot_switch"), slot_switch_to_a, row);
    } else if (item->type == ITEM_ACTION && strcmp(item->action, "format") == 0) {
        open_confirm(T("dvr.format_prompt"), T("dvr.format"), format_sdcard, row);
    } else if (item->type == ITEM_ACTION && strcmp(item->action, "camera") == 0) {
        camera_open(row);
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
}

/* content renderers (each fills the cleared g_content with rows) */
/* A settings list: one widget row per item (stepper/toggle/dropdown/action). rows, if non-NULL,
 * receives the row objects for in-place relabel (used only by System, the only list with a language
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
            add_toggle_switch(row, toggle_is_on(item));
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
    render_settings_list(g_goggles_items, GOGGLES_ITEM_COUNT, NULL);
}

static void render_system(void)
{
    render_settings_list(g_system_items, SYSTEM_ITEM_COUNT, g_rows);
}

static void render_dvr(void)
{
    render_settings_list(g_dvr_items, DVR_ITEM_COUNT, NULL);
}

/* CENTER on a recording row: start playing that clip (the player remembers it for the return focus). */
static void playback_row_clicked(lv_event_t *event)
{
    static recording_t recs[MAX_RECORDINGS];
    int i = (int) (intptr_t) lv_event_get_user_data(event);
    int n = recordings_list(recs, MAX_RECORDINGS);
    if (i >= 0 && i < n) {
        player_start(i, recs[i].name);
    }
}

/* The SD-card recordings list: one focusable row per clip (icon + name, length/size on the right).
 * CENTER plays the clip (player_start). The list is bounded by MAX_RECORDINGS, so it fits LVGL's pool.
 * An absent card or an empty list shows a hint and keeps focus in the sidebar.
 */
static void render_playback(void)
{
    if (!recordings_sd_available()) {
        render_centered_hint(T("playback.no_sd"));   /* no card / not mounted: keep focus in the sidebar */
        return;
    }

    static recording_t recordings[MAX_RECORDINGS];
    int count = recordings_list(recordings, MAX_RECORDINGS);

    if (count == 0) {
        render_centered_hint(T("playback.empty"));   /* nothing to focus: keep focus in the sidebar */
        return;
    }

    for (int i = 0; i < count; i++) {
        lv_obj_t *row = make_row(LV_SYMBOL_VIDEO, recordings[i].name);
        lv_obj_add_event_cb(row, playback_row_clicked, LV_EVENT_CLICKED, (void *) (intptr_t) i);

        /* Right-hand value: clip length (from the MP4 header) and file size. The length is dropped
         * only if the header will not parse. */
        char value[40];
        char path[512];
        recordings_path(recordings[i].name, path, sizeof path);
        long gb_tenths = (recordings[i].size_mb * 10 + 512) / 1024;   /* MB -> GB, one decimal */

        unsigned ms = recordings_duration_ms(path);
        if (ms > 0) {
            unsigned s = ms / 1000;
            snprintf(value, sizeof(value), "%u:%02u   %ld.%ldGB", s / 60, s % 60,
                     gb_tenths / 10, gb_tenths % 10);
        } else {
            snprintf(value, sizeof(value), "%ld.%ldGB", gb_tenths / 10, gb_tenths % 10);
        }
        lv_obj_t *value_label = lv_label_create(row);
        lv_obj_set_style_text_color(value_label, COLOR_TEXT_DIM, 0);   /* de-emphasise vs the name */
        lv_label_set_text(value_label, value);
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

/* The air-unit section: always editable. The settings live on the goggle and are pushed to the air
 * unit at association (the air latches them during the handshake), so they are prepared here whether
 * or not the link is currently up.
 */
static void render_air_unit(void)
{
    render_settings_list(g_airunit_items, AIRUNIT_ITEM_COUNT, NULL);
}

/* Clear the content pane and render the current section into it. Called with focus in the sidebar
 * (or before the first zone is entered), so the group never holds a row this deletes.
 */
static void render_content(void)
{
    lv_obj_clean(g_content);
    for (int i = 0; i < SYSTEM_ITEM_COUNT; i++) {
        g_rows[i] = NULL;
    }

    g_content_focusable = 1;
    lv_obj_set_flex_flow(g_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (g_section == SECTION_CHANNEL) {
        menu_channel_render();
    } else if (g_section == SECTION_GOGGLES) {
        render_goggles();
    } else if (g_section == SECTION_SYSTEM) {
        render_system();
    } else if (g_section == SECTION_DVR) {
        render_dvr();
    } else if (g_section == SECTION_AIRUNIT) {
        render_air_unit();
    } else {
        render_playback();
    }
}

/* Loop-tick refresh of the channel grid (menu_channel.c does the work). Guarded here because the
 * "grid is the shown section" test needs the menu's own open/section state.
 */
void menu_channel_tick(void)
{
    if (!g_is_open || g_section != SECTION_CHANNEL) {
        return;
    }

    menu_channel_refresh();
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

/* camera overlay */
static int cam_value(const cam_item_t *item)
{
    return settings_get_int_in(g_settings, AIRUNIT_SECTION, item->setting_key, item->def);
}

/* list items: the label index of the current value (falls back to the default's index, then 0) */
static int cam_label_index(const cam_item_t *item)
{
    int value = cam_value(item);
    int def_index = 0;
    for (int i = 0; item->labels[i] != NULL; i++) {
        if (item->values[i] == value) {
            return i;
        }

        if (item->values[i] == item->def) {
            def_index = i;
        }
    }

    return def_index;
}

/* Push the persisted zoom/aspect pair to the air unit: both ride the ONE SetScaleMode message. */
static void cam_push_scale(void)
{
    linkcmd_set_scale(settings_get_int_in(g_settings, AIRUNIT_SECTION, CAM_KEY_ASPECT,
                                          MLM_CAM_DEF_ASPECT),
                      (unsigned) settings_get_int_in(g_settings, AIRUNIT_SECTION, CAM_KEY_ZOOM,
                                                     MLM_CAM_DEF_ZOOM_PCT));
}

/* Push one item's persisted value to the air unit via ml-linkd. */
static void cam_push(const cam_item_t *item)
{
    if (item->cam_sel != 0) {
        linkcmd_set_camera((unsigned) item->cam_sel, (unsigned) cam_value(item));
    } else {
        cam_push_scale();
    }
}

void menu_camera_assert(void)
{
    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        if (g_camera_items[i].cam_sel != 0) {
            cam_push(&g_camera_items[i]);
        }
    }

    cam_push_scale();   /* the zoom/aspect pair rides once, not once per item */
}

/* A section is a column: the setting name on its own line, the "< value >" line below it (children
 * of the value row: left caret, value, right caret). */
static void refresh_cam_row(lv_obj_t *section, const cam_item_t *item)
{
    lv_obj_t *line = lv_obj_get_child(section, 1);
    lv_obj_t *value = lv_obj_get_child(line, 1);
    if (item->labels != NULL) {
        int index = cam_label_index(item);
        lv_label_set_text(value, T(item->labels[index]));
        set_caret(lv_obj_get_child(line, 0), index > 0);
        set_caret(lv_obj_get_child(line, 2), item->labels[index + 1] != NULL);
    } else {
        int v = cam_value(item);
        lv_label_set_text_fmt(value, "%d", v);
        set_caret(lv_obj_get_child(line, 0), v > item->min);
        set_caret(lv_obj_get_child(line, 2), v < item->max);
    }
}

/* The fixed width of a section's value label: the widest option in its set, so the carets around it
 * never move as the value steps. */
static int32_t cam_value_width(const cam_item_t *item)
{
    lv_point_t size;
    int32_t max_w = 0;

    if (item->labels != NULL) {
        for (int i = 0; item->labels[i] != NULL; i++) {
            lv_text_get_size(&size, T(item->labels[i]), &g_menu_font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            if (size.x > max_w) {
                max_w = size.x;
            }
        }
    } else {
        char buf[16];
        const int bounds[2] = { item->min, item->max };
        for (unsigned i = 0; i < 2; i++) {
            snprintf(buf, sizeof buf, "%d", bounds[i]);
            lv_text_get_size(&size, buf, &g_menu_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
            if (size.x > max_w) {
                max_w = size.x;
            }
        }
    }

    return max_w;
}

/* The panel width that hugs the content: the widest of every section's title line and its
 * caret-value-caret line, plus the panel and section padding. Computed per open (from the value
 * widths camera_open already measured), so it adapts to the loaded language catalog. */
static int32_t cam_panel_width(const int32_t *value_widths)
{
    lv_point_t size;
    int32_t caret_w;
    int32_t max_w = 0;

    lv_text_get_size(&size, LV_SYMBOL_LEFT, &g_menu_font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    caret_w = size.x;

    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        int32_t line_w = 2 * caret_w + value_widths[i] + 2 * CAM_VALUE_PAD_HOR;

        lv_text_get_size(&size, T(g_camera_items[i].title_key), &g_menu_font, 0, 0, LV_COORD_MAX,
                         LV_TEXT_FLAG_NONE);
        if (size.x > max_w) {
            max_w = size.x;
        }

        if (line_w > max_w) {
            max_w = line_w;
        }
    }

    lv_text_get_size(&size, T("camera.reset"), &g_menu_font, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    if (size.x > max_w) {
        max_w = size.x;
    }

    max_w += 2 * (SIDEBAR_PAD + ITEM_PAD_HOR);
    return max_w > CAM_PANEL_MIN_WIDTH ? max_w : CAM_PANEL_MIN_WIDTH;
}

/* The reset row: persist every camera default, push the lot to the air (menu_camera_assert reads
 * the freshly-persisted settings, so zoom/aspect ride one coherent scale message), and refresh all
 * the visible sections. */
static void cam_reset_clicked_cb(lv_event_t *event)
{
    (void) event;
    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        /* memory-only per key: one settings_save below instead of a flash rewrite per key */
        settings_set_int_in_nosave(g_settings, AIRUNIT_SECTION, g_camera_items[i].setting_key,
                                   g_camera_items[i].def);
    }

    settings_save(g_settings);
    menu_camera_assert();
    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        refresh_cam_row(lv_obj_get_child(g_cam_panel, i), &g_camera_items[i]);
    }
}

/* LEFT/RIGHT on a camera row: step the value, persist it, push it live, refresh the row. */
static void cam_key_cb(lv_event_t *event)
{
    uint32_t key = lv_event_get_key(event);
    if (key != LV_KEY_LEFT && key != LV_KEY_RIGHT) {
        return;
    }

    const cam_item_t *item = (const cam_item_t *) lv_event_get_user_data(event);
    lv_obj_t *row = (lv_obj_t *) lv_event_get_target(event);
    int value = cam_value(item);

    if (item->labels != NULL) {
        int index = cam_label_index(item);
        if (key == LV_KEY_RIGHT && item->labels[index + 1] != NULL) {
            value = item->values[index + 1];
        } else if (key == LV_KEY_LEFT && index > 0) {
            value = item->values[index - 1];
        } else {
            return;
        }
    } else {
        int next = value + (key == LV_KEY_RIGHT ? item->step : -item->step);
        if (next < item->min || next > item->max) {
            return;
        }

        value = next;
    }

    /* Memory-only: a held key sweeps saturation/sharpness in ~20 steps, and a full settings-file
     * flash rewrite per step is pure wear - the live cam_push is the feedback that matters.
     * camera_close persists the batch once. */
    settings_set_int_in_nosave(g_settings, AIRUNIT_SECTION, item->setting_key, value);
    cam_push(item);
    refresh_cam_row(row, item);
}

/* Hide the menu (and the System OSD bar) and show the compact strip over the live feed: the point
 * of every camera item is to judge its effect against the live image, so all chrome gets out of the
 * way. Each setting is a section: the name on its own line, the "< value >" line below it, and a
 * thin divider between sections. BACK restores everything (camera_close), landing back on the
 * Camera row. */
static void camera_open(lv_obj_t *row)
{
    /* one text-measure pass: both the panel width and each value label's fixed width use these */
    int32_t widths[CAMERA_ITEM_COUNT];
    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        widths[i] = cam_value_width(&g_camera_items[i]);
    }

    g_cam_row = row;
    lv_obj_add_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
    sysosd_set_visible(0);   /* every strip of live image counts here */

    g_cam_panel = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(g_cam_panel);
    lv_obj_set_size(g_cam_panel, cam_panel_width(widths), LV_SIZE_CONTENT);
    lv_obj_align(g_cam_panel, LV_ALIGN_LEFT_MID, CAM_PANEL_MARGIN, 0);
    lv_obj_set_style_bg_color(g_cam_panel, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(g_cam_panel, LV_OPA_50, 0);   /* the live feed stays visible behind */
    lv_obj_set_style_radius(g_cam_panel, ITEM_RADIUS, 0);
    lv_obj_set_style_pad_all(g_cam_panel, SIDEBAR_PAD, 0);
    lv_obj_set_flex_flow(g_cam_panel, LV_FLEX_FLOW_COLUMN);

    lv_group_remove_all_objs(g_group);   /* the strip sections become the keypad sink */
    for (int i = 0; i < CAMERA_ITEM_COUNT; i++) {
        const cam_item_t *item = &g_camera_items[i];

        lv_obj_t *section = make_modal_button(g_cam_panel, T(item->title_key));
        lv_obj_set_style_pad_ver(section, CAM_ROW_PAD_VER, 0);
        lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        /* divider between sections (the reset row below is always last) */
        lv_obj_set_style_border_side(section, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(section, 2, 0);
        lv_obj_set_style_border_color(section, COLOR_TEXT_DIM, 0);
        lv_obj_set_style_border_opa(section, LV_OPA_40, 0);

        lv_obj_t *line = lv_obj_create(section);
        lv_obj_remove_style_all(line);
        lv_obj_set_width(line, lv_pct(100));
        lv_obj_set_height(line, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        add_caret_value_labels(line, CAM_VALUE_PAD_HOR, widths[i]);

        refresh_cam_row(section, item);
        lv_obj_add_event_cb(section, cam_key_cb, LV_EVENT_KEY, (void *) item);
        lv_group_add_obj(g_group, section);
    }

    /* the last entry: reset every camera setting to its default (CENTER activates it) */
    lv_obj_t *reset = make_modal_button(g_cam_panel, T("camera.reset"));
    lv_obj_set_style_pad_ver(reset, CAM_ROW_PAD_VER, 0);
    lv_obj_add_event_cb(reset, cam_reset_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(g_group, reset);

    lv_group_focus_obj(lv_obj_get_child(g_cam_panel, 0));
}

static void camera_close(void)
{
    if (g_cam_panel == NULL) {
        return;
    }

    lv_obj_delete(g_cam_panel);
    g_cam_panel = NULL;
    settings_save(g_settings);   /* persist the batch the memory-only steppers accumulated */
    sysosd_set_visible(1);       /* the update tick re-applies the show_system_osd setting */

    if (g_menu != NULL) {
        lv_obj_remove_flag(g_menu, LV_OBJ_FLAG_HIDDEN);
        enter_content_zone();
        if (g_cam_row != NULL) {
            lv_group_focus_obj(g_cam_row);   /* land back on the Camera row */
        }
    }

    g_cam_row = NULL;
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

        /* A band choice was made in the list that just closed: confirm it, since applying it
         * reboots. Checked before the notice - the confirm is what raises the failure notice.
         */
        if (g_band_confirm_due) {
            g_band_confirm_due = 0;   /* before open_confirm: its own close runs this path again */
            open_confirm(T("band.reboot_prompt"), T("common.reboot"), band_confirm_apply,
                         g_select_row);
            return;
        }

        /* an overlay that asked for a notice (e.g. a failed band write) raises it now */
        if (g_notice_pending != NULL) {
            const char *prompt = g_notice_pending;
            g_notice_pending = NULL;
            open_notice(prompt);
        }
    } else {
        /* the menu closed under the overlay: drop anything it was going to raise */
        g_notice_pending = NULL;
        g_band_pending = NULL;
        g_band_confirm_due = 0;
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

    /* The band only takes effect across a reboot, so it is confirmed before it is committed:
     * stash the choice and let the close raise the confirm. Nothing is persisted until then, so
     * Cancel needs no revert - the row still shows the old band because it never changed.
     */
    if (strcmp(item->action, "band") == 0) {
        g_band_pending = item->options[index];
        g_band_confirm_due = 1;
        close_select();
        return;
    }

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

/* Select the RF band by writing the marker ml-video reads at boot: "normal" = the 3-channel band,
 * anything else = race (16 channels). The band is the baseband config's chan_valid_bmp, which only
 * reaches the AR8030 at firmware upload, and artosyn_sdio must never be warm-reloaded, so this
 * cannot take effect before the next boot: raise the notice saying so once the select list is gone.
 * Deliberately does not reboot - on a RAM-booted slot B a watchdog reset lands in stock slot A, not
 * back here.
 */
/* Confirm accepted: persist the band, drop the marker ml-video reads at boot, and reset. The band
 * is the baseband config's chan_valid_bmp and only reaches the AR8030 at firmware upload, and
 * artosyn_sdio must never be warm-reloaded, so a reboot is the only way to apply it.
 *
 * The reset boots whatever slot the GPT marks active, which is the point: it is stock slot A while
 * slot B is only RAM-booted, so this returns to B only once B is the flashed active slot.
 */
static void band_confirm_apply(void)
{
    const char *band = (g_band_pending != NULL && strcmp(g_band_pending, "Normal") == 0)
                       ? "normal" : "race";

    mkdir(BAND_MARKER_DIR, 0755);

    FILE *f = fopen(BAND_MARKER_PATH, "w");
    if (f == NULL) {
        /* Without the marker the band cannot survive the reboot that applies it, so do NOT reboot:
         * say it failed instead. /usrdata is a mount (ml-usrdata) and may be absent.
         */
        fprintf(stderr, "menu: cannot write %s: %s\n", BAND_MARKER_PATH, strerror(errno));
        g_notice_pending = T("band.save_failed");
        g_band_pending = NULL;
        return;
    }

    fputs(band, f);
    fclose(f);

    settings_set_string_in(g_settings, GOG_SECTION, "band", g_band_pending);
    g_band_pending = NULL;
    sync();

    system("/usr/local/bin/wdt-reset");
}

/* A one-button acknowledgement modal. Same overlay state as the select list and the confirm prompt,
 * so it can only be raised once theirs is torn down (g_notice_pending, drained in the close).
 */
static void open_notice(const char *prompt)
{
    g_select_item = NULL;
    g_select_row = NULL;
    g_confirm_fn = NULL;

    lv_obj_t *box = make_modal(prompt);
    lv_group_remove_all_objs(g_group);

    lv_obj_t *ok = make_modal_button(box, T("common.ok"));
    lv_obj_add_event_cb(ok, confirm_button_cb, LV_EVENT_CLICKED, (void *) (intptr_t) 0);
    lv_group_add_obj(g_group, ok);
    lv_group_focus_obj(ok);

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

/* player host: the shared menu objects + focus callback the recordings player borrows (player.h). */
static lv_group_t *host_group(void)     { return g_group; }
static lv_obj_t   *host_menu_root(void) { return g_menu; }
static lv_obj_t   *host_content(void)   { return g_content; }
static bool         host_menu_is_open(void) { return g_is_open; }

/* Re-enter the recordings list after playback and land on the clip that was played (not the top). */
static void restore_recordings_list(int row_index)
{
    enter_content_zone();
    if (row_index >= 0 && (uint32_t) row_index < lv_obj_get_child_count(g_content)) {
        lv_group_focus_obj(lv_obj_get_child(g_content, row_index));
    }
}

static const player_host_t g_player_host = {
    .group        = host_group,
    .menu_root    = host_menu_root,
    .content      = host_content,
    .menu_open    = host_menu_is_open,
    .restore_list = restore_recordings_list,
};

/* channel host: the shared menu objects + callbacks the channel grid borrows (menu_channel.h). */
static lv_style_t *host_style_item(void)         { return &g_style_item; }
static lv_style_t *host_style_item_focused(void) { return &g_style_item_focused; }
static bool         host_is_in_sidebar(void)         { return g_zone == 0; }
static void        host_persist_channel(int idx) { settings_set_int_in(g_settings, GOG_SECTION, "channel", idx); }

static const menu_channel_host_t g_channel_host = {
    .content            = host_content,
    .group              = host_group,
    .style_item         = host_style_item,
    .style_item_focused = host_style_item_focused,
    .in_sidebar         = host_is_in_sidebar,
    .centered_hint      = render_centered_hint,
    .rebuild            = render_content,
    .persist_channel    = host_persist_channel,
};

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

    player_init(&g_player_host);
    menu_channel_init(&g_channel_host);
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
    g_section = SECTION_CHANNEL;   /* a fresh open always starts at the top section */
    menu_channel_reset();

    /* No scan is fired on open: the sweep retunes the RX across every channel and interrupts video,
     * which can knock a flying pilot's link, so opening the menu must stay passive. A fresh open
     * lands focus in the sidebar; the sweep runs only when the channel section is actively entered
     * (menu_center) or the grid's Refresh button is pressed. The grid renders the last scan, or a
     * "press CENTER to scan" hint when none has run yet. */
    render_content();
    enter_sidebar_zone();
    g_is_open = 1;
}

void menu_close(void)
{
    player_on_menu_closing();   /* stop any playback before the content pane (spinner parent) goes */

    if (g_select_box != NULL) {
        lv_obj_delete(g_select_box);
        g_select_box = NULL;
    }

    g_select_open = 0;
    g_select_closing = 0;

    /* one teardown path: the transient menu-restore inside is harmless, g_menu is deleted below */
    camera_close();

    if (g_menu != NULL) {
        lv_group_remove_all_objs(g_group);
        lv_obj_delete(g_menu);
        g_menu = NULL;
        g_sidebar = NULL;
        g_content = NULL;

        for (int i = 0; i < NUM_SECTIONS; i++) {
            g_sidebar_buttons[i] = NULL;
        }

        for (int i = 0; i < SYSTEM_ITEM_COUNT; i++) {
            g_rows[i] = NULL;
        }
    }

    menu_channel_reset();   /* g_content is gone; drop the dangling tile pointers */
    g_zone = 0;
    g_section = SECTION_CHANNEL;
    g_is_open = 0;
}

void menu_close_all(void)
{
    menu_close();
}

bool menu_is_open(void)
{
    return g_is_open;
}

void menu_playback_tick(void)
{
    player_tick();   /* drive the loading phase / transport bar (no-op unless a clip is playing) */
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

    if (player_is_loading()) {
        player_cancel_loading();   /* abandon the clip before it opened */
        return;
    }

    if (player_is_open()) {
        player_close(1);   /* stop playback, return to the recordings list */
        return;
    }

    if (g_cam_panel != NULL) {
        camera_close();   /* restore the menu, focus back on the Camera row */
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

/* Grid navigation is only consulted while the channel grid holds focus; menu_channel.c maps the key
 * to the tile geometry. Every other section is a vertical list where linear navigation is correct.
 */
static int chan_nav(int dcol, int drow)
{
    if (g_section != SECTION_CHANNEL || g_zone != 1) {
        return 0;
    }

    return menu_channel_nav(dcol, drow);
}

void menu_up(void)
{
    if (g_is_open && !player_is_open() && !player_is_loading()) {
        if (chan_nav(0, -1)) {
            return;
        }

        feed_key(LV_KEY_PREV);
    }
}

void menu_down(void)
{
    if (g_is_open && !player_is_open() && !player_is_loading()) {
        if (chan_nav(0, 1)) {
            return;
        }

        feed_key(LV_KEY_NEXT);
    }
}

void menu_left(void)
{
    if (!g_is_open) {
        return;
    }

    /* navigation frozen while a clip loads */
    if (player_is_loading()) {
        return;
    }

    if (player_is_open()) {
        player_key_left();
        return;
    }

    if (chan_nav(-1, 0)) {
        return;
    }

    feed_key(LV_KEY_LEFT);
}

void menu_right(void)
{
    if (!g_is_open) {
        return;
    }

    /* navigation frozen while a clip loads */
    if (player_is_loading()) {
        return;
    }

    if (player_is_open()) {
        player_key_right();
        return;
    }

    if (chan_nav(1, 0)) {
        return;
    }

    feed_key(LV_KEY_RIGHT);
}

void menu_center(void)
{
    if (!g_is_open) {
        return;
    }

    if (player_is_loading()) {
        return;   /* ignore input while a clip loads */
    }

    if (player_is_open()) {
        player_key_center();
        return;
    }

    if (g_zone == 0) {
        /* Entering the channel section runs one scan sweep; re-entering it is the manual refresh.
         * The sweep retunes the RX across every valid channel and interrupts video for its duration,
         * so it is deliberately not fired from sidebar_focused_cb: that previews a section on every
         * scrub past it and would sweep repeatedly. Requested before the focusable check so a first
         * scan can be started while the grid is still an unfocusable hint. */
        if (g_section == SECTION_CHANNEL) {
            menu_channel_request_scan();
            render_content();   /* swap the idle hint for "scanning" */
        }

        if (g_content_focusable) {   /* a hint screen (e.g. empty Playback) has nothing to enter */
            enter_content_zone();
        }

        return;
    }

    feed_key(LV_KEY_ENTER);
}
