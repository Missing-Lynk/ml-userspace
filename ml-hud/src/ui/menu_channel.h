/**
 * @file menu_channel.h
 * @brief The channel-grid screen: a tile per channel valid in the current band, coloured by measured
 *        SNR, with a green border on the tuned channel. One of the menu's sidebar sections.
 *
 * The grid borrows the menu's shared LVGL objects (content pane, keypad group, row styles) rather
 * than owning them; menu.c wires those in once via menu_channel_init and drives the screen through
 * the calls below. This mirrors how player.c borrows the menu (see player.h).
 */
#ifndef HUD_UI_MENU_CHANNEL_H
#define HUD_UI_MENU_CHANNEL_H

#include "lvgl.h"

#include <stdbool.h>

/** @brief The menu objects + callbacks the channel grid borrows. Accessors return the current value,
 *  so the grid always sees the live object (the menu rebuilds/destroys these as sections change). */
typedef struct {
    lv_obj_t   *(*content)(void);             /**< content pane: the tile parent, rebuilt per section */
    lv_group_t *(*group)(void);               /**< keypad group, for grid-navigation refocus */
    lv_style_t *(*style_item)(void);          /**< base row/tile style */
    lv_style_t *(*style_item_focused)(void);  /**< focused row/tile style */
    bool         (*in_sidebar)(void);          /**< focus is in the sidebar (a full rebuild is safe) */
    void        (*centered_hint)(const char *text);  /**< fill the content pane with a dim message */
    void        (*rebuild)(void);             /**< re-render the whole content pane (render_content) */
    void        (*persist_channel)(int idx);  /**< save the picked channel to settings */
} menu_channel_host_t;

/** @brief One-time wiring of the menu host. Call from menu_init. */
void menu_channel_init(const menu_channel_host_t *host);

/** @brief Drop the tile pointers and scan generation. Call when the menu opens or closes (the
 *  content pane, and the tiles with it, are gone). */
void menu_channel_reset(void);

/** @brief Build the grid into the (already-cleared) content pane from the last scan. Shows a hint
 *  and leaves focus in the sidebar until a scan with valid channels arrives. */
void menu_channel_render(void);

/** @brief Fire one scan sweep and flag "scanning" so the next render shows that hint. The sweep
 *  retunes the RX across every channel and interrupts video, so it is requested only on demand. */
void menu_channel_request_scan(void);

/** @brief Refresh from a newly-arrived scan: reconcile the active border, update signals in place,
 *  or rebuild the grid if the valid set changed. Call every loop tick while the grid is shown. */
void menu_channel_refresh(void);

/** @brief Move focus one cell across the grid (@p dcol / @p drow in -1..+1). @return 1 if the key
 *  belongs to the grid (consumed), 0 to fall through to the menu's linear navigation. */
int  menu_channel_nav(int dcol, int drow);

#endif /* HUD_UI_MENU_CHANNEL_H */
