#include "led_ui.h"

#include <stddef.h>

enum {
    LED_UI_PAIRING_TOGGLE_MS = 500U,
    LED_UI_ERROR_TOGGLE_MS = 120U,
    LED_UI_CONNECTED_PULSE_MS = 80U,
    LED_UI_CONNECTED_PERIOD_MS = 1000U,
    LED_UI_LONG_BLINK_ON_MS = 600U,
    LED_UI_LONG_BLINK_OFF_MS = 350U
};

void led_ui_init(led_ui_t * ui) {
    if (ui == NULL) {
        return;
    }

    ui->state = LED_UI_STATE_IDLE;
    ui->led_on = false;
    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->last_transition_ms = 0U;
    ui->cue_phase_started_ms = 0U;
}

void led_ui_set_state(
    led_ui_t * ui,
    led_ui_state_t state,
    uint32_t now_ms
) {
    if (ui == NULL) {
        return;
    }

    if (ui->state == state) {
        return;
    }

    ui->state = state;
    ui->last_transition_ms = now_ms;

    if ((state == LED_UI_STATE_IDLE) || (state == LED_UI_STATE_CONNECTED)) {
        ui->led_on = false;
        return;
    }

    ui->led_on = true;
}

void led_ui_trigger_long_blink(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t now_ms
) {
    if (ui == NULL) {
        return;
    }

    if (blink_count == 0U) {
        return;
    }

    ui->cue_active = true;
    ui->cue_led_on = true;
    ui->cue_remaining_blink = blink_count;
    ui->cue_phase_started_ms = now_ms;
    ui->led_on = true;
}

bool led_ui_tick(
    led_ui_t * ui,
    uint32_t now_ms
) {
    if (ui == NULL) {
        return false;
    }

    if (ui->cue_active) {
        if (ui->cue_led_on) {
            if ((now_ms - ui->cue_phase_started_ms) >= LED_UI_LONG_BLINK_ON_MS) {
                ui->cue_led_on = false;
                ui->cue_phase_started_ms = now_ms;
                ui->cue_remaining_blink = (uint8_t)(ui->cue_remaining_blink - 1U);

                if (ui->cue_remaining_blink == 0U) {
                    ui->cue_active = false;
                    ui->last_transition_ms = now_ms;
                }
            }

            ui->led_on = ui->cue_led_on;
            return ui->led_on;
        }

        if ((ui->cue_remaining_blink > 0U)
            && ((now_ms - ui->cue_phase_started_ms) >= LED_UI_LONG_BLINK_OFF_MS)) {
            ui->cue_led_on = true;
            ui->cue_phase_started_ms = now_ms;
        }

        ui->led_on = ui->cue_led_on;
        return ui->led_on;
    }

    if (ui->state == LED_UI_STATE_IDLE) {
        ui->led_on = false;
        return ui->led_on;
    }

    if (ui->state == LED_UI_STATE_CONNECTED) {
        const uint32_t phase_ms = now_ms % LED_UI_CONNECTED_PERIOD_MS;
        ui->led_on = (phase_ms < LED_UI_CONNECTED_PULSE_MS);
        return ui->led_on;
    }

    if (ui->state == LED_UI_STATE_PAIRING) {
        if ((now_ms - ui->last_transition_ms) >= LED_UI_PAIRING_TOGGLE_MS) {
            ui->led_on = !ui->led_on;
            ui->last_transition_ms = now_ms;
        }

        return ui->led_on;
    }

    if ((now_ms - ui->last_transition_ms) >= LED_UI_ERROR_TOGGLE_MS) {
        ui->led_on = !ui->led_on;
        ui->last_transition_ms = now_ms;
    }

    return ui->led_on;
}
