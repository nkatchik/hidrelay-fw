#include "platform_pico_w_stack.h"

#include <stddef.h>
#include <string.h>

#ifdef APP_PICO_HAS_BTSTACK
#include "ble/sm.h"
#include "btstack_event.h"
#include "classic/hid_host.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#endif

#include "platform_pico_w_tinyusb_runtime.h"

enum {
    PICO_W_STACK_MAX_USB_INTERFACE = 8U,
    PICO_W_STACK_EVENT_QUEUE_SIZE = 16U
};

static uint8_t g_usb_interface_count = 0U;
static uint32_t g_usb_descriptor_generation = 0U;
static hid_transport_event_t g_event_queue[PICO_W_STACK_EVENT_QUEUE_SIZE] = {0};
static uint8_t g_event_queue_head = 0U;
static uint8_t g_event_queue_tail = 0U;
static uint8_t g_event_queue_count = 0U;

static bool pico_w_stack_push_event(const hid_transport_event_t *event) {
    if ((event == NULL) || (event->type == HID_TRANSPORT_EVENT_NONE)) {
        return false;
    }

    if (g_event_queue_count >= PICO_W_STACK_EVENT_QUEUE_SIZE) {
        return false;
    }

    g_event_queue[g_event_queue_tail] = *event;
    g_event_queue_tail = (uint8_t)((g_event_queue_tail + 1U) % PICO_W_STACK_EVENT_QUEUE_SIZE);
    g_event_queue_count = (uint8_t)(g_event_queue_count + 1U);
    return true;
}

#ifdef APP_PICO_HAS_BTSTACK
static uint8_t g_btstack_hid_descriptor_storage[1024] = {0};

static void pico_w_btstack_hid_packet_handler(uint8_t packet_type,
                                              uint16_t channel,
                                              uint8_t *packet,
                                              uint16_t size) {
    (void)channel;

    if ((packet == NULL) || (size < 3U) || (packet_type != HCI_EVENT_PACKET)) {
        return;
    }

    if (hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) {
        return;
    }

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
    case HID_SUBEVENT_CONNECTION_OPENED:
        if (hid_subevent_connection_opened_get_status(packet) == ERROR_CODE_SUCCESS) {
            hid_transport_event_t event = {0};
            bd_addr_t device_addr = {0};

            event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
            event.hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
            event.vendor_id = 0U;
            event.product_id = 0U;
            event.report_descriptor_len = hid_descriptor_storage_get_descriptor_len(event.hid_cid);
            hid_subevent_connection_opened_get_bd_addr(packet, device_addr);
            (void)memcpy(event.device_id.bytes, device_addr, sizeof(event.device_id.bytes));
            (void)pico_w_stack_push_event(&event);
        }
        break;
    case HID_SUBEVENT_CONNECTION_CLOSED:
        {
            hid_transport_event_t event = {0};

            event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
            event.hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
            (void)pico_w_stack_push_event(&event);
        }
        break;
    case HID_SUBEVENT_REPORT:
        {
            const uint8_t *report = hid_subevent_report_get_report(packet);
            uint16_t report_len = hid_subevent_report_get_report_len(packet);
            hid_transport_event_t event = {0};

            if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
                report_len = HID_TRANSPORT_REPORT_MAX_LEN;
            }

            event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
            event.hid_cid = hid_subevent_report_get_hid_cid(packet);
            event.report_len = report_len;

            if ((report != NULL) && (report_len > 0U)) {
                (void)memcpy(event.report, report, report_len);
            }

            (void)pico_w_stack_push_event(&event);
        }
        break;
    default:
        break;
    }
}
#endif

bool pico_w_stack_init(void) {
    (void)memset(g_event_queue, 0, sizeof(g_event_queue));
    g_event_queue_head = 0U;
    g_event_queue_tail = 0U;
    g_event_queue_count = 0U;

#ifdef APP_PICO_HAS_BTSTACK
    async_context_t *context = cyw43_arch_async_context();

    if ((context == NULL) || !btstack_cyw43_init(context)) {
        return false;
    }

    l2cap_init();
    sm_init();
    hid_host_init(g_btstack_hid_descriptor_storage, sizeof(g_btstack_hid_descriptor_storage));
    hid_host_register_packet_handler(&pico_w_btstack_hid_packet_handler);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    (void)hci_power_control(HCI_POWER_ON);
#endif

    if (!pico_w_tinyusb_runtime_init()) {
        return false;
    }

    return true;
}

void pico_w_stack_poll(uint32_t now_ms) {
    (void)now_ms;
    pico_w_tinyusb_runtime_poll();
}

void pico_w_stack_set_usb_plan(uint8_t interface_count, uint32_t descriptor_generation) {
    if (descriptor_generation == g_usb_descriptor_generation) {
        return;
    }

    if (interface_count > PICO_W_STACK_MAX_USB_INTERFACE) {
        interface_count = PICO_W_STACK_MAX_USB_INTERFACE;
    }

    g_usb_interface_count = interface_count;
    g_usb_descriptor_generation = descriptor_generation;
}

uint8_t pico_w_stack_usb_interface_count(void) {
    return g_usb_interface_count;
}

bool pico_w_stack_take_event(hid_transport_event_t *out_event) {
    if (out_event == NULL) {
        return false;
    }

    (void)memset(out_event, 0, sizeof(*out_event));

    if (g_event_queue_count == 0U) {
        return false;
    }

    *out_event = g_event_queue[g_event_queue_head];
    (void)memset(&g_event_queue[g_event_queue_head], 0, sizeof(g_event_queue[g_event_queue_head]));
    g_event_queue_head = (uint8_t)((g_event_queue_head + 1U) % PICO_W_STACK_EVENT_QUEUE_SIZE);
    g_event_queue_count = (uint8_t)(g_event_queue_count - 1U);
    return true;
}

void pico_w_stack_ingest_usb_report(uint8_t interface_number, const uint8_t *report, uint16_t report_len) {
    hid_transport_event_t event = {0};

    if ((report_len > HID_TRANSPORT_REPORT_MAX_LEN) || ((report_len > 0U) && (report == NULL))) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_USB_HID_REPORT;
    event.interface_number = interface_number;
    event.report_len = report_len;

    if (report_len > 0U) {
        (void)memcpy(event.report, report, report_len);
    }

    (void)pico_w_stack_push_event(&event);
}

bool pico_w_stack_send_usb_report(uint8_t interface_number, const uint8_t *report, uint16_t report_len) {
    return pico_w_tinyusb_runtime_send_in_report(interface_number, report, report_len);
}

bool pico_w_stack_send_bt_report(uint16_t hid_cid, const uint8_t *report, uint16_t report_len) {
#ifdef APP_PICO_HAS_BTSTACK
    if ((hid_cid == 0U) || (report_len == 0U) || (report == NULL) || (report_len > UINT8_MAX)) {
        return false;
    }

    return hid_host_send_set_report(hid_cid,
                                    HID_REPORT_TYPE_OUTPUT,
                                    0U,
                                    report,
                                    (uint8_t)report_len) == ERROR_CODE_SUCCESS;
#else
    (void)hid_cid;
    (void)report;
    (void)report_len;
    return false;
#endif
}
