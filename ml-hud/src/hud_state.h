/**
 * @file hud_state.h
 * @brief The HUD's two top-level states.
 *
 * The HUD has exactly two states:
 *   - HUD_MENU_CLOSED: the flying view. The System OSD and the BTFL (FC/MSP) OSD are drawn over the
 *     live video.
 *   - HUD_MENU_OPEN:   the settings menu is up. The BTFL OSD is NOT drawn (it hides behind the menu).
 */
#ifndef HUD_STATE_H
#define HUD_STATE_H

#include <stdbool.h>

typedef enum {
    HUD_MENU_CLOSED = 0,
    HUD_MENU_OPEN   = 1,
} hud_state_t;

/** @brief Should the BTFL OSD be drawn in this state? Only when the menu is closed. */
static inline bool hud_btfl_visible(hud_state_t st)
{
    return st == HUD_MENU_CLOSED;
}

#endif /* HUD_STATE_H */
