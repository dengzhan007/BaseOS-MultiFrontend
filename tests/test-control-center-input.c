#include <assert.h>
#include <stdio.h>
#include "baseos-control-center-input.h"

int main(void)
{
    BaseOSInputState state = {0, 0};

    assert(baseos_map_input_event(&state, EV_KEY, KEY_UP, 1) == KEY_UP);
    assert(baseos_map_input_event(&state, EV_KEY, BTN_DPAD_DOWN, 1) == KEY_DOWN);
    assert(baseos_map_input_event(&state, EV_KEY, KEY_LEFT, 2) == KEY_LEFT);
    assert(baseos_map_input_event(&state, EV_KEY, BTN_DPAD_RIGHT, 0) == -1);

    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0Y, -1) == KEY_UP);
    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0Y, -1) == -1);
    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0Y, 0) == -1);
    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0Y, 1) == KEY_DOWN);
    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0X, -1) == KEY_LEFT);
    assert(baseos_map_input_event(&state, EV_ABS, ABS_HAT0X, 1) == KEY_RIGHT);

    puts("control-center input mapping tests passed");
    return 0;
}
