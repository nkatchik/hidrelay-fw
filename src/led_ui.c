#include "led_ui.h"

#include <stddef.h>

enum {
    LED_UI_PAIRING_TOGGLE_BLE_MS = 100U,
    LED_UI_PAIRING_TOGGLE_CLASSIC_MS = 300U,
    LED_UI_CONNECTED_CUE_MS = 3000U,
    LED_UI_DISCONNECT_CUE_MS = 1000U,
    LED_UI_STARTUP_CUE_MS = 200U,
    LED_UI_SIGNAL_PREEMPT_DARK_MS = 80U,
    LED_UI_ERROR_TOGGLE_MS = 120U,
    LED_UI_ERROR_PRE_BLINK_DARK_MS = 2000U,
    LED_UI_ERROR_BLINK_ON_MS = 1000U,
    LED_UI_ERROR_BLINK_OFF_MS = 1000U,
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

static void led_ui_clear_transient_signals(led_ui_t * ui) {
    if (ui == NULL) {
        return;
    }

    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->signal_dark_until_ms = 0U;
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
        || ((ui->state == LED_UI_STATE_PAIRING) && ui->pairing_attempt_active)
        || (ui->state == LED_UI_STATE_ERROR)
        || led_ui_deadline_active(now_ms, ui->signal_dark_until_ms);
    if (!signal_active) {
        return false;
    }

    led_ui_clear_transient_signals(ui);
    ui->signal_dark_until_ms = now_ms + LED_UI_SIGNAL_PREEMPT_DARK_MS;
    ui->led_on = false;
    return true;
}

static void led_ui_start_blink_cue(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t on_ms,
    uint32_t off_ms,
    uint32_t pre_blink_dark_ms,
    uint32_t now_ms,
    bool allow_while_pairing
) {
    uint32_t cue_start_ms = now_ms;

    if ((ui == NULL) || (blink_count == 0U) || (on_ms == 0U)) {
        return;
    }

    if (!allow_while_pairing && (ui->state == LED_UI_STATE_PAIRING)) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    if (pre_blink_dark_ms > 0U) {
        cue_start_ms = now_ms + pre_blink_dark_ms;
        ui->signal_dark_until_ms = cue_start_ms;
    }

    ui->pairing_attempt_active = false;
    ui->cue_active = true;
    ui->cue_led_on = true;
    ui->cue_remaining_blink = blink_count;
    ui->cue_phase_started_ms = cue_start_ms;
    ui->cue_on_ms = on_ms;
    ui->cue_off_ms = off_ms;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->led_on = false;
}

void led_ui_init(led_ui_t * ui) {
    if (ui == NULL) {
        return;
    }

    ui->state = LED_UI_STATE_IDLE;
    ui->led_on = false;
    ui->pairing_attempt_active = false;
    ui->pairing_toggle_ms = LED_UI_PAIRING_TOGGLE_BLE_MS;
    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->last_transition_ms = 0U;
    ui->connected_cue_until_ms = 0U;
    ui->cue_phase_started_ms = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->signal_dark_until_ms = 0U;
}

void led_ui_set_state(
    led_ui_t * ui,
    led_ui_state_t state,
    uint32_t now_ms
) {
    const led_ui_state_t prev_state = (ui != NULL) ? ui->state : LED_UI_STATE_IDLE;
    uint32_t connected_cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (ui->state == state) {
        return;
    }

    if (state == LED_UI_STATE_PAIRING) {
        ui->state = state;
        ui->last_transition_ms = now_ms;
        led_ui_clear_transient_signals(ui);
        ui->pairing_attempt_active = false;
        ui->led_on = true;
        return;
    }

    if ((state == LED_UI_STATE_CONNECTED)
        && (prev_state != LED_UI_STATE_PAIRING)
        && led_ui_interrupt_for_new_signal(ui, now_ms)) {
        connected_cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->state = state;
    ui->last_transition_ms = now_ms;
    ui->connected_cue_until_ms = 0U;
    if (state != LED_UI_STATE_PAIRING) {
        ui->pairing_attempt_active = false;
    }

    if (state == LED_UI_STATE_IDLE) {
        ui->led_on = false;
        return;
    }

    if (state == LED_UI_STATE_CONNECTED) {
        if (prev_state == LED_UI_STATE_PAIRING) {
            led_ui_clear_transient_signals(ui);
            ui->led_on = false;
            return;
        }

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
    led_ui_start_blink_cue(
        ui,
        blink_count,
        LED_UI_LONG_BLINK_ON_MS,
        LED_UI_LONG_BLINK_OFF_MS,
        0U,
        now_ms,
        false
    );
}

void led_ui_trigger_error_blink(
    led_ui_t * ui,
    uint8_t blink_count,
    uint32_t now_ms
) {
    led_ui_start_blink_cue(
        ui,
        blink_count,
        LED_UI_ERROR_BLINK_ON_MS,
        LED_UI_ERROR_BLINK_OFF_MS,
        LED_UI_ERROR_PRE_BLINK_DARK_MS,
        now_ms,
        true
    );
}

void led_ui_trigger_connected_cue(
    led_ui_t * ui,
    uint32_t now_ms
) {
    uint32_t cue_start_ms = now_ms;

    if (ui == NULL) {
        return;
    }

    if (ui->state == LED_UI_STATE_PAIRING) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->pairing_attempt_active = false;
    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
    ui->connected_cue_until_ms = cue_start_ms + LED_UI_CONNECTED_CUE_MS;
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

    if (ui->state == LED_UI_STATE_PAIRING) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->pairing_attempt_active = false;
    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
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

    if (ui->state == LED_UI_STATE_PAIRING) {
        return;
    }

    if (led_ui_interrupt_for_new_signal(ui, now_ms)) {
        cue_start_ms = ui->signal_dark_until_ms;
    }

    ui->pairing_attempt_active = false;
    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = cue_start_ms + LED_UI_STARTUP_CUE_MS;
    ui->led_on = false;
}

void led_ui_set_pairing_attempt_active(
    led_ui_t * ui,
    bool attempt_active,
    uint32_t now_ms
) {
    if (ui == NULL) {
        return;
    }

    if (!attempt_active) {
        ui->pairing_attempt_active = false;
        if (ui->state == LED_UI_STATE_PAIRING) {
            ui->led_on = false;
            ui->last_transition_ms = now_ms;
        }
        return;
    }

    if (ui->state != LED_UI_STATE_PAIRING) {
        return;
    }

    ui->cue_active = false;
    ui->cue_led_on = false;
    ui->cue_remaining_blink = 0U;
    ui->cue_on_ms = LED_UI_LONG_BLINK_ON_MS;
    ui->cue_off_ms = LED_UI_LONG_BLINK_OFF_MS;
    ui->connected_cue_until_ms = 0U;
    ui->disconnect_cue_until_ms = 0U;
    ui->startup_cue_until_ms = 0U;
    ui->signal_dark_until_ms = 0U;
    ui->pairing_attempt_active = true;
    ui->led_on = true;
    ui->last_transition_ms = now_ms;
}

void led_ui_set_pairing_classic_mode(
    led_ui_t * ui,
    bool classic_mode,
    uint32_t now_ms
) {
    const uint32_t next_toggle_ms =
        classic_mode ? LED_UI_PAIRING_TOGGLE_CLASSIC_MS : LED_UI_PAIRING_TOGGLE_BLE_MS;

    if (ui == NULL) {
        return;
    }

    if (ui->pairing_toggle_ms == next_toggle_ms) {
        return;
    }

    ui->pairing_toggle_ms = next_toggle_ms;
    if ((ui->state == LED_UI_STATE_PAIRING) && !ui->pairing_attempt_active) {
        ui->led_on = true;
        ui->last_transition_ms = now_ms;
    }
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
        const uint32_t cue_on_ms = (ui->cue_on_ms > 0U) ? ui->cue_on_ms : LED_UI_LONG_BLINK_ON_MS;
        const uint32_t cue_off_ms =
            (ui->cue_off_ms > 0U) ? ui->cue_off_ms : LED_UI_LONG_BLINK_OFF_MS;

        if (ui->cue_led_on) {
            if ((now_ms - ui->cue_phase_started_ms) >= cue_on_ms) {
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

        if ((ui->cue_remaining_blink > 0U) && ((now_ms - ui->cue_phase_started_ms) >= cue_off_ms)) {
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
        if (ui->pairing_attempt_active) {
            ui->led_on = true;
            return ui->led_on;
        }

        if ((now_ms - ui->last_transition_ms) >= ui->pairing_toggle_ms) {
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
