#ifndef HIDRELAY_PLATFORM_PICO_W_STACK_H
#define HIDRELAY_PLATFORM_PICO_W_STACK_H

#include <stdbool.h>
#include <stdint.h>

#include "hid_transport.h"

bool pico_w_stack_init(void);
void pico_w_stack_poll(uint32_t now_ms);
void pico_w_stack_set_usb_plan(uint8_t interface_count, uint32_t descriptor_generation);
void pico_w_stack_set_pairing_active(bool pairing_active);
uint8_t pico_w_stack_usb_interface_count(void);
bool pico_w_stack_take_event(hid_transport_event_t *out_event);
void pico_w_stack_ingest_usb_report(uint8_t interface_number, const uint8_t *report, uint16_t report_len);
bool pico_w_stack_send_usb_report(uint8_t interface_number, const uint8_t *report, uint16_t report_len);
bool pico_w_stack_send_bt_report(uint16_t hid_cid, uint8_t protocol_mode, const uint8_t *report, uint16_t report_len);

#endif
