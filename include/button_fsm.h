#ifndef HIDRELAY_BUTTON_FSM_H
#define HIDRELAY_BUTTON_FSM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BUTTON_COMMAND_NONE = 0,
    BUTTON_COMMAND_PAIR_ANY,
    BUTTON_COMMAND_REMOVE_LAST,
    BUTTON_COMMAND_REMOVE_ALL,
    BUTTON_COMMAND_SINGLE_CLICK
} button_command_t;

typedef struct {
    bool stable_pressed;
    bool very_long_emitted;
    bool long_press_emitted;
    bool arm_pending_on_release;
    bool pending_long_press;
    uint32_t press_started_ms;
    uint32_t first_long_released_ms;
} button_fsm_t;

void button_fsm_init(button_fsm_t * fsm);
button_command_t button_fsm_update(
    button_fsm_t * fsm,
    bool pressed,
    uint32_t now_ms
);

#endif
