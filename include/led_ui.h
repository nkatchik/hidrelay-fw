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
    bool pairing_attempt_active;
    uint32_t pairing_toggle_ms;
    bool cue_active;
    bool cue_led_on;
    uint8_t cue_remaining_blink;
    uint32_t last_transition_ms;
    uint32_t connected_cue_until_ms;
    uint32_t cue_phase_started_ms;
    uint32_t cue_on_ms;
    uint32_t cue_off_ms;
    uint32_t disconnect_cue_until_ms;
    uint32_t startup_cue_until_ms;
    uint32_t signal_dark_until_ms;
} led_ui_t;

void led_ui_init(led_ui_t * ui);
void led_ui_set_state(
    led_ui_t * ui,
    led_ui_state_t state,
    uint32_t now_ms
);
void led_ui_trigger_long_blink(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t now_ms
);
void led_ui_trigger_error_blink(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t now_ms
);
void led_ui_trigger_disconnect_cue(
    led_ui_t * ui,
    uint32_t now_ms
);
void led_ui_trigger_connected_cue(
    led_ui_t * ui,
    uint32_t now_ms
);
void led_ui_trigger_startup_cue(
    led_ui_t * ui,
    uint32_t now_ms
);
void led_ui_set_pairing_attempt_active(
    led_ui_t * ui,
    bool attempt_active,
    uint32_t now_ms
);
void led_ui_set_pairing_classic_mode(
    led_ui_t * ui,
    bool classic_mode,
    uint32_t now_ms
);
bool led_ui_tick(
    led_ui_t * ui,
    uint32_t now_ms
);

#endif
