#ifndef HIDRELAY_PLATFORM_API_H
#define HIDRELAY_PLATFORM_API_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"
#include "pair_db.h"

typedef struct {
    uint8_t event_queue_depth;
    uint8_t event_queue_high_watermark;
    uint32_t event_queue_dropped;
    uint8_t connect_pending;
    uint8_t reconnect_pending;
    uint8_t connect_mode;
    uint8_t reconnect_attempt_index;
    uint8_t reconnect_attempt_count;
    uint8_t last_connect_status;
} platform_transport_state_t;

bool platform_init(void);
bool platform_button_pressed(void);
uint32_t platform_uptime_ms(void);
void platform_set_led(bool led_on);
void platform_sleep_us(uint32_t sleep_us);
bool platform_factory_reset_erase_persistent_data(void);
void platform_reboot(void);

void platform_transport_poll(uint32_t now_ms);
bool platform_transport_take_event(hid_transport_event_t * out_event);
void platform_transport_set_usb_plan(
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan
);
void platform_transport_set_pairing(
    bool pairing_active,
    uint8_t bt_link_type
);
bool platform_transport_request_reconnect(
    const pair_device_id_t * device_id,
    uint8_t bt_link_type_hint,
    uint8_t bt_addr_type_hint
);
bool platform_transport_forget_device(const pair_device_id_t * device_id);
bool platform_transport_send_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
bool platform_transport_send_bt_report(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len
);
bool platform_transport_state_get(platform_transport_state_t * out_state);

bool platform_pair_db_load(pair_db_t * db);
bool platform_pair_db_save(const pair_db_t * db);
bool platform_diag_write(
    const uint8_t * data,
    uint16_t data_len
);

#endif
