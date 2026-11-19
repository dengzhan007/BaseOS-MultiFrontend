#include <assert.h>
#include <stdio.h>
#include "baseos-control-center-input.h"

int main(void)
{
    BaseOSInputState state = {0, 0};

    /* The RG34XX device tree assigns BTN_WEST/BTN_Z to L1/R1;
     * BTN_TL/BTN_TR are Select/Start and must not open the selector. */
    assert(baseos_is_boot_selector_key(BTN_WEST));
    assert(baseos_is_boot_selector_key(BTN_Z));
    assert(!baseos_is_boot_selector_key(BTN_TL));
    assert(!baseos_is_boot_selector_key(BTN_TR));

    /* Same device tree puts L2/R2 on BTN_SELECT/BTN_START and MENU on
     * BTN_TL2. UI actions answer to the printed button, so both codes of a
     * pair must be accepted and neither may leak into the other pair. */
    assert(baseos_is_select_key(BTN_TL));
    assert(baseos_is_select_key(BTN_SELECT));
    assert(!baseos_is_select_key(BTN_TR));
    assert(!baseos_is_select_key(BTN_START));
    assert(baseos_is_start_key(BTN_TR));
    assert(baseos_is_start_key(BTN_START));

    /* The button test needs a chord that users can actually press: SELECT +
     * START, not the old MENU + START. The caller resolves either code of a
     * pair, so the helper only sees two already-resolved flags. */
    assert(baseos_is_button_test_exit_chord(1, 1));
    assert(!baseos_is_button_test_exit_chord(1, 0));
    assert(!baseos_is_button_test_exit_chord(0, 1));
    assert(!baseos_is_button_test_exit_chord(0, 0));
    assert(!baseos_is_start_key(BTN_TL));
    assert(!baseos_is_start_key(BTN_SELECT));

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
