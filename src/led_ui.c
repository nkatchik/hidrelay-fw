#include "led_ui.h"

#include <stddef.h>

enum {
    LED_UI_PAIRING_TOGGLE_MS = 100U,
    LED_UI_CONNECTED_CUE_MS = 3000U,
    LED_UI_DISCONNECT_CUE_MS = 1000U,
    LED_UI_STARTUP_CUE_MS = 200U,
    LED_UI_SIGNAL_PREEMPT_DARK_MS = 80U,
    LED_UI_ERROR_TOGGLE_MS = 120U,
    /* Long-blink cues confirm commands (remove-last / remove-all). */
    LED_UI_LONG_BLINK_ON_MS = 900U,
    LED_UI_LONG_BLINK_OFF_MS = 600U
};

static bool led_ui_deadline_active(
    uint32_t now_ms,
    uint32_t until_ms
) {
    return (until_ms != 0U) && ((int32_t)(now_ms - until_ms) < 0);
}

static bool led_ui_interrupt_for_new_signal(
    led_ui_t * ui,
    uint32_t now_ms
) {
    bool signal_active = false;

    if (ui == NULL) {
        return false;
    }

    signal_active = ui->cue_active
        || led_ui_deadline_active(now_ms, ui->connected_cue_until_ms)
        || led_ui_deadline_active(now_ms, ui->disconnect_cue_until_ms)
        || led_ui_deadline_active(now_ms, ui->startup_cue_until_ms)
        || (ui->state == LED_UI_STATE_PAIRING)
        || (ui->state == LED_UI_STATE_ERROR)
        || led_ui_deadline_active(now_ms, ui->signal_dark_until_ms);
    if (!signal_active) {
        return false;
    }

    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->signal_dark_until_ms = now_ms + LED_UI_SIGNAL_PREEMPT_DARK_MS;
    ui->led_on = false;
    return true;
}

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
    ui->connected_cue_until_ms = 0U;
    ui->cue_phase_started_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->signal_dark_until_ms = 0U;
}

void led_ui_set_state(
    led_ui_t * ui,
    led_ui_state_t state,
    uint32_t now_ms
) {
    uint32_t connected_cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (ui->state == state) {
        return;
    }

    if ((state == LED_UI_STATE_CONNECTED) && led_ui_interrupt_for_new_signal(ui, now_ms)) {
        connected_cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->state = state;
    ui->last_transition_ms = now_ms;
    ui->connected_cue_until_ms = 0U;

    if (state == LED_UI_STATE_IDLE) {
        ui->led_on = false;
        return;
    }

    if (state == LED_UI_STATE_CONNECTED) {
        ui->connected_cue_until_ms = connected_cue_start_ms + LED_UI_CONNECTED_CUE_MS;
        ui->led_on = true;
        return;
    }

    ui->led_on = true;
}

void led_ui_trigger_long_blink(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t now_ms
) {
    uint32_t cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (blink_count == 0U) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->cue_active = true;
    ui->cue_led_on = true;
    ui->cue_remaining_blink = blink_count;
    ui->cue_phase_started_ms = cue_start_ms;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->led_on = false;
}

void led_ui_trigger_disconnect_cue(
    led_ui_t * ui,
    uint32_t now_ms
) {
    uint32_t cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = cue_start_ms + LED_UI_DISCONNECT_CUE_MS;
    ui->startup_cue_until_ms = 0U;
    ui->led_on = false;
}

void led_ui_trigger_startup_cue(
    led_ui_t * ui,
    uint32_t now_ms
) {
    uint32_t cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = cue_start_ms + LED_UI_STARTUP_CUE_MS;
    ui->led_on = false;
}

bool led_ui_tick(
    led_ui_t * ui,
    uint32_t now_ms
) {
    if (ui == NULL) {
        return false;
    }

    if (led_ui_deadline_active(now_ms, ui->signal_dark_until_ms)) {
        ui->led_on = false;
        return ui->led_on;
    }
    ui->signal_dark_until_ms = 0U;

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

    if (led_ui_deadline_active(now_ms, ui->startup_cue_until_ms)) {
        ui->led_on = true;
        return ui->led_on;
    }
    ui->startup_cue_until_ms = 0U;

    if (led_ui_deadline_active(now_ms, ui->disconnect_cue_until_ms)) {
        ui->led_on = true;
        return ui->led_on;
    }
    ui->disconnect_cue_until_ms = 0U;

    if (ui->state == LED_UI_STATE_IDLE) {
        ui->led_on = false;
        return ui->led_on;
    }

    if (ui->state == LED_UI_STATE_CONNECTED) {
        ui->led_on = led_ui_deadline_active(now_ms, ui->connected_cue_until_ms);
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
