/**
 * @file menu.h
 * @brief The menu: a left sidebar of sections and a content pane, shown above the video when open.
 *
 * The first section is "Goggles" (brightness, buzzer, key tones, low-voltage alarm, minimum cell
 * voltage, language). Two focus zones: the sidebar (pick a section, CENTER enters it) and the
 * content (UP/DOWN move rows, LEFT/RIGHT change values, CENTER toggles). BACK steps one zone back
 * and closes at the sidebar; a long BACK closes outright. Item changes are written to the settings
 * object and applied live.
 */
#ifndef HUD_UI_MENU_H
#define HUD_UI_MENU_H

#include "settings.h"

#include <stdbool.h>

/** @brief One-time setup: styles, fonts, the keypad group. Call after LVGL init and the display. */
void menu_init(settings_t *settings);

/** @brief Load the saved language catalog and push persisted brightness + buzzer volume to the
 *         hardware. Call once at startup.
 */
void menu_apply_persisted(void);

void menu_open(void);      /**< @brief Build and show the menu (sidebar zone). No-op if already open. */
void menu_close(void);     /**< @brief Destroy the menu. */
bool  menu_is_open(void);   /**< @brief Whether the menu is shown. */
int  menu_depth(void);     /**< @brief Nesting depth (0 = sidebar, 1 = inside a section). */

/* Navigation, driven by the button controller (called outside LVGL event context). */
void menu_up(void);        /**< @brief Move focus to the previous row/section. */
void menu_down(void);      /**< @brief Move focus to the next row/section. */
void menu_left(void);      /**< @brief Step the focused value down. */
void menu_right(void);     /**< @brief Step the focused value up. */
void menu_center(void);    /**< @brief Enter the section (sidebar) or toggle/advance the item (content). */
void menu_back(void);      /**< @brief Step one zone back; close at the sidebar. */
void menu_close_all(void); /**< @brief Close the menu entirely, whatever the depth (long BACK). */

/** @brief Advance the playback transport bar from the pipeline's telemetry. Call every loop tick so
 *  the scrubber tracks position promptly; a no-op unless a clip is playing. */
void menu_playback_tick(void);

/** @brief Refresh the channel grid from a newly-arrived scan (signal + active highlight). Call every
 *  loop tick; a no-op unless the channel section is shown. */
void menu_channel_tick(void);

/** @brief Push every persisted camera setting (and the zoom/aspect scale pair) to the air unit via
 *  ml-linkd. hud.c calls this on every link-up edge: a re-association resets the air's ISP to its
 *  association defaults, so the saved values need re-asserting like power and standby. */
void menu_camera_assert(void);

#endif /* HUD_UI_MENU_H */
