#include "button_fsm.h"

#include <stddef.h>

enum {
    BUTTON_LONG_PRESS_MIN_MS = 1000U,
    BUTTON_CLASSIC_PAIR_PRESS_MS = 5000U,
    BUTTON_VERY_LONG_PRESS_MS = 10000U
};

void button_fsm_init(button_fsm_t * fsm) {
    if (fsm == NULL) {
        return;
    }

    fsm->stable_pressed = false;
    fsm->very_long_emitted = false;
    fsm->long_press_emitted = false;
    fsm->classic_pair_emitted = false;
    fsm->arm_pending_on_release = false;
    fsm->press_started_ms = 0U;
}

button_command_t button_fsm_update(
    button_fsm_t * fsm,
    bool pressed,
    uint32_t now_ms
) {
    if (fsm == NULL) {
        return BUTTON_COMMAND_NONE;
    }

    if (pressed && !fsm->stable_pressed) {
        fsm->stable_pressed = true;
        fsm->very_long_emitted = false;
        fsm->long_press_emitted = false;
        fsm->classic_pair_emitted = false;
        fsm->arm_pending_on_release = false;
        fsm->press_started_ms = now_ms;
    }

    if (pressed && fsm->stable_pressed && !fsm->very_long_emitted) {
        const uint32_t press_duration_ms = now_ms - fsm->press_started_ms;

        if (press_duration_ms >= BUTTON_VERY_LONG_PRESS_MS) {
            fsm->very_long_emitted = true;
            fsm->long_press_emitted = true;
            fsm->classic_pair_emitted = true;
            fsm->arm_pending_on_release = false;
            return BUTTON_COMMAND_REMOVE_ALL;
        }

        if (fsm->long_press_emitted
            && !fsm->classic_pair_emitted
            && fsm->arm_pending_on_release
            && (press_duration_ms >= BUTTON_CLASSIC_PAIR_PRESS_MS)) {
            fsm->classic_pair_emitted = true;
            fsm->arm_pending_on_release = false;
            return BUTTON_COMMAND_PAIR_CLASSIC;
        }

        if (!fsm->long_press_emitted && (press_duration_ms >= BUTTON_CLASSIC_PAIR_PRESS_MS)) {
            fsm->long_press_emitted = true;
            fsm->classic_pair_emitted = true;
            fsm->arm_pending_on_release = false;
            return BUTTON_COMMAND_PAIR_CLASSIC;
        }

        if (!fsm->long_press_emitted && (press_duration_ms >= BUTTON_LONG_PRESS_MIN_MS)) {
            fsm->long_press_emitted = true;
            fsm->arm_pending_on_release = true;
            return BUTTON_COMMAND_PAIR_BLE;
        }
    }

    if (!pressed && fsm->stable_pressed) {
        const uint32_t press_duration_ms = now_ms - fsm->press_started_ms;

        fsm->stable_pressed = false;

        if (fsm->very_long_emitted) {
            fsm->very_long_emitted = false;
            fsm->long_press_emitted = false;
            fsm->classic_pair_emitted = false;
            fsm->arm_pending_on_release = false;
            return BUTTON_COMMAND_NONE;
        }

        if (fsm->long_press_emitted) {
            fsm->long_press_emitted = false;
            fsm->classic_pair_emitted = false;
            fsm->arm_pending_on_release = false;
            return BUTTON_COMMAND_NONE;
        }

        if (press_duration_ms >= BUTTON_LONG_PRESS_MIN_MS) {
            if (press_duration_ms >= BUTTON_CLASSIC_PAIR_PRESS_MS) {
                return BUTTON_COMMAND_PAIR_CLASSIC;
            }

            return BUTTON_COMMAND_PAIR_BLE;
        }

        return BUTTON_COMMAND_SINGLE_CLICK;
    }

    return BUTTON_COMMAND_NONE;
}
