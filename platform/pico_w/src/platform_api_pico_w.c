#include <stddef.h>

#include "hardware/watchdog.h"
#include "pico/stdlib.h"
#include "platform_api.h"
#include "platform_pico_w_hw.h"
#include "platform_pico_w_pair_store.h"
#include "platform_pico_w_stack.h"
#include "platform_pico_w_state.h"
#include "platform_pico_w_tinyusb_runtime.h"

static pico_w_state_t g_state = {
    .initialized = false,
};

#if defined(APP_PICO_HAS_TINYUSB)
enum {
    PICO_W_TINYUSB_MAX_POLL_SLEEP_US = 500U,
};
#endif

static bool platform_ready(void) {
    return pico_w_state_is_initialized(&g_state);
}

bool platform_init(void) {
#if defined(APP_PICO_HAS_TELEMETRY)
    stdio_init_all();
#endif
    pico_w_state_reset(&g_state);

    if (!pico_w_hw_init_radio()) {
        return false;
    }

    if (!pico_w_stack_init(true)) {
        return false;
    }

    pico_w_state_mark_initialized(&g_state);
    return true;
}

bool platform_button_pressed(void) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_hw_bootsel_pressed();
}

uint32_t platform_uptime_ms(void) {
    return pico_w_hw_uptime_ms();
}

void platform_set_led(bool led_on) {
    if (!platform_ready()) {
        return;
    }

    pico_w_hw_set_led(led_on);
}

void platform_sleep_us(uint32_t sleep_us) {
    if (!platform_ready()) {
        return;
    }

#if defined(APP_PICO_HAS_TINYUSB)
    if (sleep_us > PICO_W_TINYUSB_MAX_POLL_SLEEP_US) {
        sleep_us = PICO_W_TINYUSB_MAX_POLL_SLEEP_US;
    }
#endif

    pico_w_hw_sleep_us(sleep_us);
}

bool platform_factory_reset_erase_persistent_data(void) {
    return pico_w_pair_store_factory_reset_all();
}

void platform_reboot(void) {
    watchdog_reboot(0U, 0U, 0U);

    for (;;) {
        tight_loop_contents();
    }
}

void platform_transport_poll(uint32_t now_ms) {
    if (!platform_ready()) {
        return;
    }

    pico_w_stack_poll(now_ms);
}

bool platform_transport_take_event(hid_transport_event_t * out_event) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_stack_take_event(out_event);
}

void platform_transport_set_usb_plan(
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan
) {
    if (!platform_ready()) {
        return;
    }

    pico_w_stack_set_usb_plan(interface_count, descriptor_generation, interface_plan);
}

void platform_transport_set_pairing(
    bool pairing_active,
    uint8_t bt_link_type
) {
    if (!platform_ready()) {
        return;
    }

    pico_w_stack_set_pairing(pairing_active, bt_link_type);
}

bool platform_transport_request_reconnect(
    const pair_device_id_t * device_id,
    uint8_t bt_link_type_hint,
    uint8_t bt_addr_type_hint
) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_stack_request_reconnect(device_id, bt_link_type_hint, bt_addr_type_hint);
}

bool platform_transport_forget_device(const pair_device_id_t * device_id) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_stack_forget_device(device_id);
}

bool platform_transport_send_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_stack_send_usb_report(interface_number, report, report_len);
}

bool platform_transport_send_bt_report(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len
) {
    if (!platform_ready()) {
        return false;
    }

    return pico_w_stack_send_bt_report(hid_cid, bt_link_type, protocol_mode, report, report_len);
}

bool platform_transport_state_get(platform_transport_state_t * out_state) {
    pico_w_stack_runtime_state_t stack_state = {0};

    if ((out_state == NULL) || !platform_ready()) {
        return false;
    }

    if (!pico_w_stack_runtime_state_get(&stack_state)) {
        return false;
    }

    out_state->event_queue_depth = stack_state.event_queue_depth;
    out_state->event_queue_high_watermark = stack_state.event_queue_high_watermark;
    out_state->event_queue_dropped = stack_state.event_queue_dropped;
    out_state->connect_pending = stack_state.connect_pending;
    out_state->reconnect_pending = stack_state.reconnect_pending;
    out_state->connect_mode = stack_state.connect_mode;
    out_state->reconnect_attempt_index = stack_state.reconnect_attempt_index;
    out_state->reconnect_attempt_count = stack_state.reconnect_attempt_count;
    out_state->last_connect_status = stack_state.last_connect_status;
    return true;
}

bool platform_pair_db_load(pair_db_t * db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_load(db);
}

bool platform_pair_db_save(const pair_db_t * db) {
    if (db == NULL) {
        return false;
    }

    return pico_w_pair_store_save(db);
}

bool platform_diag_write(
    const uint8_t * data,
    uint16_t data_len
) {
    return pico_w_tinyusb_runtime_diag_write(data, data_len);
}
