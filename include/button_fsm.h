#ifndef HIDRELAY_BUTTON_FSM_H
#define HIDRELAY_BUTTON_FSM_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BUTTON_COMMAND_NONE = 0,
    BUTTON_COMMAND_PAIR_BLE,
    BUTTON_COMMAND_PAIR_CLASSIC,
    BUTTON_COMMAND_REMOVE_ALL,
    BUTTON_COMMAND_SINGLE_CLICK
} button_command_t;

typedef struct {
    bool stable_pressed;
    bool very_long_emitted;
    bool long_press_emitted;
    bool classic_pair_emitted;
    bool arm_pending_on_release;
    uint32_t press_started_ms;
} button_fsm_t;

void button_fsm_init(button_fsm_t * fsm);
button_command_t button_fsm_update(
    button_fsm_t * fsm,
    bool pressed,
    uint32_t now_ms
);

#endif
