#ifndef HIDRELAY_LED_UI_H
#define HIDRELAY_LED_UI_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LED_UI_STATE_IDLE = 0,
    LED_UI_STATE_PAIRING,
    LED_UI_STATE_CONNECTED,
    LED_UI_STATE_ERROR
} led_ui_state_t;

typedef struct {
    led_ui_state_t state;
    bool led_on;
    bool cue_active;
    bool cue_led_on;
    uint8_t cue_remaining_blink;
    uint32_t last_transition_ms;
    uint32_t cue_phase_started_ms;
} led_ui_t;

void led_ui_init(led_ui_t *ui);
void led_ui_set_state(led_ui_t *ui, led_ui_state_t state, uint32_t now_ms);
void led_ui_trigger_long_blink(led_ui_t *ui, uint8_t blink_count, uint32_t now_ms);
bool led_ui_tick(led_ui_t *ui, uint32_t now_ms);

#endif
