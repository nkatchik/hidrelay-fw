#include "button_fsm.h"

#include <stddef.h>

enum {
    BUTTON_LONG_PRESS_MIN_MS = 2000U,
    BUTTON_DOUBLE_LONG_WINDOW_MS = 2500U,
    BUTTON_VERY_LONG_PRESS_MS = 10000U
};

void button_fsm_init(button_fsm_t *fsm) {
    if (fsm == NULL) {
        return;
    }

    fsm->stable_pressed = false;
    fsm->very_long_emitted = false;
    fsm->pending_long_press = false;
    fsm->press_started_ms = 0U;
    fsm->first_long_released_ms = 0U;
}

button_command_t button_fsm_update(button_fsm_t *fsm, bool pressed, uint32_t now_ms) {
    if (fsm == NULL) {
        return BUTTON_COMMAND_NONE;
    }

    if (pressed && !fsm->stable_pressed) {
        fsm->stable_pressed = true;
        fsm->very_long_emitted = false;
        fsm->press_started_ms = now_ms;
    }

    if (pressed && fsm->stable_pressed && !fsm->very_long_emitted) {
        if ((now_ms - fsm->press_started_ms) >= BUTTON_VERY_LONG_PRESS_MS) {
            fsm->very_long_emitted = true;
            fsm->pending_long_press = false;
            return BUTTON_COMMAND_REMOVE_ALL;
        }
    }

    if (!pressed && fsm->stable_pressed) {
        const uint32_t press_duration_ms = now_ms - fsm->press_started_ms;

        fsm->stable_pressed = false;

        if (fsm->very_long_emitted) {
            fsm->very_long_emitted = false;
            return BUTTON_COMMAND_NONE;
        }

        if (press_duration_ms >= BUTTON_LONG_PRESS_MIN_MS) {
            if (fsm->pending_long_press &&
                ((now_ms - fsm->first_long_released_ms) <= BUTTON_DOUBLE_LONG_WINDOW_MS)) {
                fsm->pending_long_press = false;
                return BUTTON_COMMAND_REMOVE_LAST;
            }

            fsm->pending_long_press = true;
            fsm->first_long_released_ms = now_ms;
            return BUTTON_COMMAND_NONE;
        }

        return BUTTON_COMMAND_NONE;
    }

    if (!pressed && !fsm->stable_pressed && fsm->pending_long_press) {
        if ((now_ms - fsm->first_long_released_ms) > BUTTON_DOUBLE_LONG_WINDOW_MS) {
            fsm->pending_long_press = false;
            return BUTTON_COMMAND_PAIR_ANY;
        }
    }

    return BUTTON_COMMAND_NONE;
}
