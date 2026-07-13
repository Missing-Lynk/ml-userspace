/**
 * @file input.h
 * @brief The goggle keypad as button events.
 *
 * The keypad is an ADC ladder the kernel exposes as an evdev device (the board profile's
 * input_device). The kernel driver debounces; userspace receives clean EV_KEY edges. Each key
 * reports press, release and autorepeat (while held), mapped through the board keymap into a
 * hud_button_t plus an edge.
 */
#ifndef HUD_INPUT_H
#define HUD_INPUT_H

typedef enum {
    HUD_BTN_NONE = 0,
    HUD_BTN_UP,
    HUD_BTN_DOWN,
    HUD_BTN_CENTER,
    HUD_BTN_BACK,
    HUD_BTN_LEFT,
    HUD_BTN_RIGHT,
    HUD_BTN_RECORD,
    HUD_BTN_BIND,
} hud_button_t;

typedef enum {
    HUD_EDGE_UP     = 0,   /* release */
    HUD_EDGE_DOWN   = 1,   /* press */
    HUD_EDGE_REPEAT = 2,   /* autorepeat while held */
} hud_button_edge_t;

/** @brief Called for each decoded key edge. */
typedef void (*input_button_cb_t)(void *ctx, hud_button_t button, hud_button_edge_t edge);

/** @brief Human-readable button name ("up", "center", ...). */
const char *input_button_name(hud_button_t button);

/** @brief Human-readable edge name ("down", "up", "repeat"). */
const char *input_edge_name(hud_button_edge_t edge);

/** @brief Open the board's keypad evdev device (non-blocking). Returns the fd, or -1 if it cannot
 *         be opened. */
int input_open(void);

/** @brief Read all pending key events without blocking, invoking @p cb for each mapped edge. */
void input_drain(int fd, input_button_cb_t cb, void *ctx);

/** @brief Close the keypad device. */
void input_close(int fd);

#endif /* HUD_INPUT_H */
