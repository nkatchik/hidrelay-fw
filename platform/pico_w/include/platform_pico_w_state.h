#ifndef HIDRELAY_PLATFORM_PICO_W_STATE_H
#define HIDRELAY_PLATFORM_PICO_W_STATE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool initialized;
} pico_w_state_t;

void pico_w_state_reset(pico_w_state_t *state);
void pico_w_state_mark_initialized(pico_w_state_t *state);
bool pico_w_state_is_initialized(const pico_w_state_t *state);

#endif
