/**
 * @file player.h
 * @brief The recordings player: a bottom transport bar shown over the video while a captured clip
 *        plays on the pipeline (preempting the live stream).
 *
 * Flow: the menu picks a clip (player_start), which starts the pipeline and shows a loading spinner
 * over the list row while the decoder warms up; player_tick() reveals the transport bar once the
 * first frame lands, then keeps the scrubber tracking the pipeline's telemetry. LEFT/RIGHT drive a
 * -8..8x speed ladder, CENTER pauses/resumes (and replays at end-of-clip), BACK returns to the list.
 *
 * The player borrows the menu's shared LVGL objects rather than owning them; menu.c wires those in
 * once via player_init.
 */
#ifndef HUD_UI_PLAYER_H
#define HUD_UI_PLAYER_H

#include "lvgl.h"

/** @brief The menu objects + callback the player needs. All accessors return the current value, so
 *  the player always sees the live object (the menu rebuilds/destroys these as it opens and closes). */
typedef struct {
    lv_group_t *(*group)(void);      /**< keypad group (the player takes it over while revealed) */
    lv_obj_t   *(*menu_root)(void);  /**< the menu's top object, hidden behind the video during playback */
    lv_obj_t   *(*content)(void);    /**< content pane holding the recordings rows (the spinner's parent) */
    int         (*menu_open)(void);  /**< whether the menu is still open (guards the focus restore) */
    void        (*restore_list)(int row_index);  /**< re-enter the recordings list, focus @p row_index */
} player_host_t;

/** @brief One-time wiring of the menu host. Call from menu_init. */
void player_init(const player_host_t *host);

/** @brief Start playing the recording in list row @p row_index (display name @p name): starts the
 *  pipeline and shows a loading spinner over the row. player_tick() reveals the bar on the first
 *  frame. */
void player_start(int row_index, const char *name);

/** @brief Advance the player each loop tick: drive the loading phase, then refresh the transport
 *  bar. A no-op when nothing is playing. */
void player_tick(void);

int  player_is_open(void);     /**< @brief The transport bar is up (a frame is on screen). */
int  player_is_loading(void);  /**< @brief A clip was picked but no frame is up yet. */

/* Key routing while the player owns input (menu.c dispatches to these when open). */
void player_key_left(void);    /**< @brief Step toward rewind. */
void player_key_right(void);   /**< @brief Step toward fast-forward. */
void player_key_center(void);  /**< @brief Pause/resume, drop to 1x, or replay at end-of-clip. */

/** @brief Leave the transport bar and return to the recordings list (BACK). @p return_live stops
 *  the pipeline; pass 0 when the pipeline already left playback on its own. */
void player_close(int return_live);

/** @brief Abandon a clip still loading (BACK during the spinner): stop the pipeline, drop the
 *  spinner. The menu was never hidden, so focus stays put. */
void player_cancel_loading(void);

/** @brief The menu is being torn down while the player is active: stop the pipeline and forget the
 *  spinner (a child of the content pane, it dies with it). Call from menu_close. */
void player_on_menu_closing(void);

#endif /* HUD_UI_PLAYER_H */
