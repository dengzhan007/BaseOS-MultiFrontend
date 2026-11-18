#ifndef BASEOS_CONTROL_CENTER_INPUT_H
#define BASEOS_CONTROL_CENTER_INPUT_H

#include <linux/input.h>

typedef struct {
    int hat_x;
    int hat_y;
} BaseOSInputState;

/* RG34XX H700 DT maps the physical shoulder buttons to BTN_WEST (L1)
 * and BTN_Z (R1). BTN_TL/BTN_TR are Select/Start on this device. */
static inline int baseos_is_boot_selector_key(int code)
{
    return code == BTN_WEST || code == BTN_Z;
}

/* The same vendor device tree puts L2/R2 on BTN_SELECT/BTN_START and MENU on
 * BTN_TL2, so a UI that binds actions to BTN_START/BTN_SELECT ends up driven by
 * the shoulder triggers while the labelled Select/Start keys do nothing. Accept
 * both codes: the physical key the user is told to press works, and a pad that
 * follows the conventional mapping keeps working too. */
static inline int baseos_is_select_key(int code)
{
    return code == BTN_TL || code == BTN_SELECT;
}

static inline int baseos_is_start_key(int code)
{
    return code == BTN_TR || code == BTN_START;
}

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
