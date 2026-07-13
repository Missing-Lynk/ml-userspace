/** @file input.c @brief See input.h. */
#include "input.h"
#include "board.h"

#include <fcntl.h>
#include <linux/input.h>
#include <stdint.h>
#include <unistd.h>

/* Map an evdev key code to its button. The codes come from the board profile (runtime), so this is
 * an if-chain, not a switch. */
static hud_button_t map_code(uint16_t code)
{
    const board_keymap_t *keys = &board_current()->keys;
    if (code == keys->up) {
        return HUD_BTN_UP;
    }

    if (code == keys->down) {
        return HUD_BTN_DOWN;
    }

    if (code == keys->center) {
        return HUD_BTN_CENTER;
    }

    if (code == keys->back) {
        return HUD_BTN_BACK;
    }

    if (code == keys->left) {
        return HUD_BTN_LEFT;
    }

    if (code == keys->right) {
        return HUD_BTN_RIGHT;
    }

    if (keys->record != 0 && code == keys->record) {
        return HUD_BTN_RECORD;
    }

    if (keys->bind != 0 && code == keys->bind) {
        return HUD_BTN_BIND;
    }

    return HUD_BTN_NONE;
}

const char *input_button_name(hud_button_t button)
{
    switch (button) {
        case HUD_BTN_UP: {
            return "up";
        } break;

        case HUD_BTN_DOWN: {
            return "down";
        } break;

        case HUD_BTN_CENTER: {
            return "center";
        } break;

        case HUD_BTN_BACK: {
            return "back";
        } break;

        case HUD_BTN_LEFT: {
            return "left";
        } break;

        case HUD_BTN_RIGHT: {
            return "right";
        } break;

        case HUD_BTN_RECORD: {
            return "record";
        } break;

        case HUD_BTN_BIND: {
            return "bind";
        } break;

        default: {
            return "?";
        } break;
    }
}

const char *input_edge_name(hud_button_edge_t edge)
{
    switch (edge) {
        case HUD_EDGE_UP: {
            return "up";
        } break;

        case HUD_EDGE_DOWN: {
            return "down";
        } break;

        case HUD_EDGE_REPEAT: {
            return "repeat";
        } break;

        default: {
            return "?";
        } break;
    }
}

int input_open(void)
{
    return open(board_current()->input_device, O_RDONLY | O_NONBLOCK);
}

void input_drain(int fd, input_button_cb_t cb, void *ctx)
{
    struct input_event event;
    while (read(fd, &event, sizeof(event)) == (ssize_t) sizeof(event)) {
        if (event.type != EV_KEY) {
            continue;
        }

        hud_button_t button = map_code(event.code);
        if (button == HUD_BTN_NONE) {
            continue;
        }

        if (cb != NULL) {
            cb(ctx, button, (hud_button_edge_t) event.value);
        }
    }
}

void input_close(int fd)
{
    if (fd >= 0) {
        close(fd);
    }
}
