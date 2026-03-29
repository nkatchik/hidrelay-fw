#include "platform_pico_w_state.h"

#include <stddef.h>

void pico_w_state_reset(pico_w_state_t * state) {
    if (state == NULL) {
        return;
    }

    state->initialized = false;
}

void pico_w_state_mark_initialized(pico_w_state_t * state) {
    if (state == NULL) {
        return;
    }

    state->initialized = true;
}

bool pico_w_state_is_initialized(const pico_w_state_t * state) {
    if (state == NULL) {
        return false;
    }

    return state->initialized;
}
