/**
 * @file theme.h
 * @brief Shared menu/player palette (matches the libre menu layout, libre/app/menu_config.h).
 */
#ifndef HUD_UI_THEME_H
#define HUD_UI_THEME_H

#include "lvgl.h"

#define COLOR_BG             lv_color_hex(0x0A0E14)
#define COLOR_SIDEBAR        lv_color_hex(0x131A26)
#define COLOR_ACCENT         lv_color_hex(0x29ABE2)
#define COLOR_TEXT           lv_color_hex(0xE6EAF0)
#define COLOR_TEXT_DIM       lv_color_hex(0x7A8497)
#define COLOR_TEXT_ON_ACCENT lv_color_hex(0x05121C)
#define COLOR_OSD            lv_color_hex(0x0E141E)   /* transport-bar / OSD background */

#endif /* HUD_UI_THEME_H */
