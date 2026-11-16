#ifndef BASEOS_CONTROL_CENTER_INPUT_H
#define BASEOS_CONTROL_CENTER_INPUT_H

#include <linux/input.h>

typedef struct {
    int hat_x;
    int hat_y;
} BaseOSInputState;

/*
 * Normalize controller navigation into Linux KEY_* codes. RG34XX firmware
 * may expose the D-pad as EV_KEY, BTN_DPAD_* or as EV_ABS ABS_HAT0X/Y.
 */
static inline int baseos_map_input_event(BaseOSInputState *state,
                                         unsigned int type,
                                         unsigned int code,
                                         int value)
{
    if (type == EV_KEY) {
        if (value != 1 && value != 2) return -1;
        switch (code) {
        case KEY_UP:
        case BTN_DPAD_UP: return KEY_UP;
        case KEY_DOWN:
        case BTN_DPAD_DOWN: return KEY_DOWN;
        case KEY_LEFT:
        case BTN_DPAD_LEFT: return KEY_LEFT;
        case KEY_RIGHT:
        case BTN_DPAD_RIGHT: return KEY_RIGHT;
        default: return (int)code;
        }
    }

    if (type == EV_ABS && state) {
        if (code == ABS_HAT0X) {
            int previous = state->hat_x;
            state->hat_x = value;
            if (value == 0 || value == previous) return -1;
            return value < 0 ? KEY_LEFT : KEY_RIGHT;
        }
        if (code == ABS_HAT0Y) {
            int previous = state->hat_y;
            state->hat_y = value;
            if (value == 0 || value == previous) return -1;
            return value < 0 ? KEY_UP : KEY_DOWN;
        }
    }
    return -1;
}

#endif
