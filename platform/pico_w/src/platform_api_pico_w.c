#include "platform_api.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "platform_pico_w_hw.h"
#include "platform_pico_w_pair_store.h"
#include "platform_pico_w_stack.h"
#include "platform_pico_w_state.h"

static pico_w_state_t g_state = {
    .initialized = false,
};
static hid_transport_diag_snapshot_t g_last_diag = {0};
static bool g_last_diag_valid = false;

static void pico_w_diag_publish(const hid_transport_diag_snapshot_t *diag) {
    if (diag == NULL) {
        return;
    }

    if (g_last_diag_valid && (memcmp(&g_last_diag, diag, sizeof(*diag)) == 0)) {
        return;
    }

    printf("[diag] bt_state=%u active=%u usb_itf=%u usb_q=%u bt_q=%u usb_drop=%lu bt_drop=%lu\n",
           diag->bt_state,
           diag->active_device_count,
           diag->usb_interface_count,
           diag->usb_tx_depth,
           diag->bt_tx_depth,
           (unsigned long)diag->usb_tx_dropped,
           (unsigned long)diag->bt_tx_dropped);
    g_last_diag = *diag;
    g_last_diag_valid = true;
}

bool platform_init(void) {
    stdio_init_all();
    pico_w_state_reset(&g_state);

    if (!pico_w_hw_init_radio()) {
        return false;
    }

    if (!pico_w_stack_init()) {
        return false;
    }

    pico_w_state_mark_initialized(&g_state);
    return true;
}

void platform_poll(platform_input_t *input) {
    if ((input == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    input->button_pressed = pico_w_hw_bootsel_pressed();
    input->uptime_ms = pico_w_hw_uptime_ms();
    input->transport_event.type = HID_TRANSPORT_EVENT_NONE;

    if (!pico_w_stack_take_event(&input->transport_event)) {
        input->transport_event.type = HID_TRANSPORT_EVENT_NONE;
    }

    pico_w_stack_poll(input->uptime_ms);
}

void platform_apply(const platform_output_t *output) {
    if ((output == NULL) || !pico_w_state_is_initialized(&g_state)) {
        return;
    }

    pico_w_stack_set_usb_plan(output->usb_interface_count,
                              output->usb_descriptor_generation,
                              output->usb_interface_plan);
    pico_w_stack_set_pairing_active(output->pairing_active);

    if (output->reconnect_request.valid) {
        (void)pico_w_stack_request_reconnect(&output->reconnect_request.device_id);
    }

    if (output->usb_tx.valid) {
        (void)pico_w_stack_send_usb_report(output->usb_tx.interface_number,
                                           output->usb_tx.report,
                                           output->usb_tx.report_len);
    }

    if (output->bt_tx.valid) {
        (void)pico_w_stack_send_bt_report(output->bt_tx.hid_cid,
                                          output->bt_tx.protocol_mode,
                                          output->bt_tx.report,
                                          output->bt_tx.report_len);
    }

    pico_w_diag_publish(&output->diag);

    pico_w_hw_set_led(output->led_on);
    pico_w_hw_sleep_ms(output->sleep_ms);
}

bool platform_pair_db_load(pair_db_t *db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_load(db);
}

bool platform_pair_db_save(const pair_db_t *db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_save(db);
}
