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
    PICO_W_STACK_EVENT_QUEUE_SIZE = 16U,
    PICO_W_STACK_INQUIRY_DURATION_UNITS = 0x08U,
    PICO_W_STACK_MAJOR_CLASS_PERIPHERAL = 0x05U,
};

static uint8_t g_usb_interface_count = 0U;
static uint32_t g_usb_descriptor_generation = 0U;
static hid_transport_usb_interface_plan_t g_usb_interface_plan[PICO_W_STACK_MAX_USB_INTERFACE] = {0};
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
static btstack_packet_callback_registration_t g_btstack_hci_event_callback_registration = {0};
static bool g_btstack_hci_ready = false;
static bool g_btstack_pairing_active = false;
static bool g_btstack_inquiry_active = false;
static bool g_btstack_connect_pending = false;
static bd_addr_t g_btstack_candidate_addr = {0};

static bool pico_w_stack_device_id_valid(const pair_device_id_t *device_id) {
    static const pair_device_id_t zero_id = { .bytes = { 0U, 0U, 0U, 0U, 0U, 0U } };

    if (device_id == NULL) {
        return false;
    }

    return memcmp(device_id->bytes, zero_id.bytes, sizeof(device_id->bytes)) != 0;
}

static uint8_t pico_w_stack_map_protocol_mode(uint8_t mode) {
    switch ((hid_protocol_mode_t)mode) {
    case HID_PROTOCOL_MODE_BOOT:
        return HID_TRANSPORT_PROTOCOL_BOOT;
    case HID_PROTOCOL_MODE_REPORT:
        return HID_TRANSPORT_PROTOCOL_REPORT;
    default:
        return HID_TRANSPORT_PROTOCOL_UNKNOWN;
    }
}

static bool pico_w_stack_pairing_policy_allows_cod(uint32_t class_of_device) {
    const uint8_t major_class = (uint8_t)((class_of_device >> 8U) & 0x1FU);
    return major_class == PICO_W_STACK_MAJOR_CLASS_PERIPHERAL;
}

static void pico_w_stack_try_start_inquiry(void) {
    if (!g_btstack_hci_ready || !g_btstack_pairing_active || g_btstack_connect_pending || g_btstack_inquiry_active) {
        return;
    }

    if (gap_inquiry_start(PICO_W_STACK_INQUIRY_DURATION_UNITS) == ERROR_CODE_SUCCESS) {
        g_btstack_inquiry_active = true;
    }
}

static void pico_w_stack_stop_inquiry(void) {
    if (!g_btstack_inquiry_active) {
        return;
    }

    (void)gap_inquiry_stop();
    g_btstack_inquiry_active = false;
}

static void pico_w_stack_schedule_candidate(const bd_addr_t addr) {
    if ((addr == NULL) || g_btstack_connect_pending) {
        return;
    }

    (void)memcpy(g_btstack_candidate_addr, addr, sizeof(g_btstack_candidate_addr));
    g_btstack_connect_pending = true;
    pico_w_stack_stop_inquiry();
}

static void pico_w_stack_try_connect_candidate(void) {
    uint16_t hid_cid = 0U;

    if (!g_btstack_hci_ready || !g_btstack_connect_pending) {
        return;
    }

    if (hid_host_connect(g_btstack_candidate_addr, HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT, &hid_cid) !=
        ERROR_CODE_SUCCESS) {
        g_btstack_connect_pending = false;
        pico_w_stack_try_start_inquiry();
    }
}

static void pico_w_stack_emit_bt_open_event(uint8_t *packet) {
    hid_transport_event_t event = {0};
    bd_addr_t device_addr = {0};

    if (packet == NULL) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
    event.vendor_id = 0U;
    event.product_id = 0U;
    event.report_descriptor_len = 0U;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    hid_subevent_connection_opened_get_bd_addr(packet, device_addr);
    (void)memcpy(event.device_id.bytes, device_addr, sizeof(event.device_id.bytes));
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_bt_descriptor_event(uint8_t *packet) {
    hid_transport_event_t event = {0};

    if (packet == NULL) {
        return;
    }

    if (hid_subevent_descriptor_available_get_status(packet) != ERROR_CODE_SUCCESS) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR;
    event.hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
    event.report_descriptor_len = hid_descriptor_storage_get_descriptor_len(event.hid_cid);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_bt_protocol_event(uint16_t hid_cid, uint8_t protocol_mode) {
    hid_transport_event_t event = {0};

    if (hid_cid == 0U) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_PROTOCOL;
    event.hid_cid = hid_cid;
    event.protocol_mode = pico_w_stack_map_protocol_mode(protocol_mode);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_bt_close_event(uint8_t *packet) {
    hid_transport_event_t event = {0};

    if (packet == NULL) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.hid_cid = hid_subevent_connection_closed_get_hid_cid(packet);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_bt_report_event(uint8_t *packet) {
    const uint8_t *report = NULL;
    uint16_t report_len = 0U;
    hid_transport_event_t event = {0};

    if (packet == NULL) {
        return;
    }

    report = hid_subevent_report_get_report(packet);
    report_len = hid_subevent_report_get_report_len(packet);

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

static void pico_w_stack_handle_hci_inquiry_result(uint8_t *packet, bool with_rssi) {
    bd_addr_t addr = {0};
    uint32_t class_of_device = 0U;

    if (packet == NULL) {
        return;
    }

    if (with_rssi) {
        hci_event_inquiry_result_with_rssi_get_bd_addr(packet, addr);
        class_of_device = hci_event_inquiry_result_with_rssi_get_class_of_device(packet);
    } else {
        hci_event_inquiry_result_get_bd_addr(packet, addr);
        class_of_device = hci_event_inquiry_result_get_class_of_device(packet);
    }

    if (!g_btstack_pairing_active || g_btstack_connect_pending || !pico_w_stack_pairing_policy_allows_cod(class_of_device)) {
        return;
    }

    pico_w_stack_schedule_candidate(addr);
}

static void pico_w_btstack_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;

    if ((packet == NULL) || (size < 3U) || (packet_type != HCI_EVENT_PACKET)) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
    case HCI_EVENT_PIN_CODE_REQUEST:
        {
            bd_addr_t address = {0};

            hci_event_pin_code_request_get_bd_addr(packet, address);
            if (g_btstack_pairing_active) {
                (void)gap_pin_code_response(address, "0000");
            } else {
                (void)gap_pin_code_negative(address);
            }
        }
        break;
    case HCI_EVENT_USER_CONFIRMATION_REQUEST:
        {
            bd_addr_t address = {0};

            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (g_btstack_pairing_active) {
                (void)gap_ssp_confirmation_response(address);
            } else {
                (void)gap_ssp_confirmation_negative(address);
            }
        }
        break;
    case BTSTACK_EVENT_STATE:
        if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
            g_btstack_hci_ready = true;
            pico_w_stack_try_start_inquiry();
        }
        break;
    case HCI_EVENT_INQUIRY_RESULT:
        pico_w_stack_handle_hci_inquiry_result(packet, false);
        break;
    case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
        pico_w_stack_handle_hci_inquiry_result(packet, true);
        break;
    case HCI_EVENT_INQUIRY_COMPLETE:
        g_btstack_inquiry_active = false;

        if (g_btstack_connect_pending) {
            pico_w_stack_try_connect_candidate();
        } else {
            pico_w_stack_try_start_inquiry();
        }
        break;
    case HCI_EVENT_HID_META:
        switch (hci_event_hid_meta_get_subevent_code(packet)) {
        case HID_SUBEVENT_INCOMING_CONNECTION:
            if (g_btstack_pairing_active) {
                (void)hid_host_accept_connection(hid_subevent_incoming_connection_get_hid_cid(packet),
                                                 HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT);
            } else {
                (void)hid_host_decline_connection(hid_subevent_incoming_connection_get_hid_cid(packet));
            }
            break;
        case HID_SUBEVENT_CONNECTION_OPENED:
            if (hid_subevent_connection_opened_get_status(packet) == ERROR_CODE_SUCCESS) {
                g_btstack_connect_pending = false;
                g_btstack_inquiry_active = false;
                pico_w_stack_emit_bt_open_event(packet);
                (void)hid_host_send_get_protocol(hid_subevent_connection_opened_get_hid_cid(packet));
            } else {
                g_btstack_connect_pending = false;
                pico_w_stack_try_start_inquiry();
            }
            break;
        case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
            pico_w_stack_emit_bt_descriptor_event(packet);
            break;
        case HID_SUBEVENT_GET_PROTOCOL_RESPONSE:
            if (hid_subevent_get_protocol_response_get_handshake_status(packet) == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                pico_w_stack_emit_bt_protocol_event(hid_subevent_get_protocol_response_get_hid_cid(packet),
                                                    hid_subevent_get_protocol_response_get_protocol_mode(packet));
            }
            break;
        case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
            if (hid_subevent_set_protocol_response_get_handshake_status(packet) == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                pico_w_stack_emit_bt_protocol_event(hid_subevent_set_protocol_response_get_hid_cid(packet),
                                                    hid_subevent_set_protocol_response_get_protocol_mode(packet));
            }
            break;
        case HID_SUBEVENT_CONNECTION_CLOSED:
            g_btstack_connect_pending = false;
            pico_w_stack_emit_bt_close_event(packet);
            pico_w_stack_try_start_inquiry();
            break;
        case HID_SUBEVENT_REPORT:
            pico_w_stack_emit_bt_report_event(packet);
            break;
        default:
            break;
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

    g_btstack_hci_ready = false;
    g_btstack_pairing_active = false;
    g_btstack_inquiry_active = false;
    g_btstack_connect_pending = false;
    (void)memset(g_btstack_candidate_addr, 0, sizeof(g_btstack_candidate_addr));

    if ((context == NULL) || !btstack_cyw43_init(context)) {
        return false;
    }

    l2cap_init();
    sm_init();
    gap_set_bondable_mode(1);
    (void)gap_set_security_mode(GAP_SECURITY_MODE_4);
    gap_set_security_level(LEVEL_2);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    hid_host_init(g_btstack_hid_descriptor_storage, sizeof(g_btstack_hid_descriptor_storage));
    hid_host_register_packet_handler(&pico_w_btstack_packet_handler);
    g_btstack_hci_event_callback_registration.callback = &pico_w_btstack_packet_handler;
    hci_add_event_handler(&g_btstack_hci_event_callback_registration);
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

#ifdef APP_PICO_HAS_BTSTACK
    pico_w_stack_try_start_inquiry();
#endif

    pico_w_tinyusb_runtime_poll();
}

void pico_w_stack_set_usb_plan(uint8_t interface_count,
                               uint32_t descriptor_generation,
                               const hid_transport_usb_interface_plan_t *interface_plan) {
    if (descriptor_generation == g_usb_descriptor_generation) {
        return;
    }

    if (interface_count > PICO_W_STACK_MAX_USB_INTERFACE) {
        interface_count = PICO_W_STACK_MAX_USB_INTERFACE;
    }

    (void)memset(g_usb_interface_plan, 0, sizeof(g_usb_interface_plan));
    if (interface_plan != NULL) {
        (void)memcpy(g_usb_interface_plan,
                     interface_plan,
                     (size_t)interface_count * sizeof(g_usb_interface_plan[0]));
    }

    g_usb_interface_count = interface_count;
    g_usb_descriptor_generation = descriptor_generation;
}

void pico_w_stack_set_pairing_active(bool pairing_active) {
#ifdef APP_PICO_HAS_BTSTACK
    if (g_btstack_pairing_active == pairing_active) {
        return;
    }

    g_btstack_pairing_active = pairing_active;

    if (!g_btstack_pairing_active) {
        g_btstack_connect_pending = false;
        pico_w_stack_stop_inquiry();
        return;
    }

    pico_w_stack_try_start_inquiry();
#else
    (void)pairing_active;
#endif
}

bool pico_w_stack_request_reconnect(const pair_device_id_t *device_id) {
#ifdef APP_PICO_HAS_BTSTACK
    if (!pico_w_stack_device_id_valid(device_id) || !g_btstack_hci_ready || g_btstack_pairing_active ||
        g_btstack_connect_pending || g_btstack_inquiry_active) {
        return false;
    }

    (void)memcpy(g_btstack_candidate_addr, device_id->bytes, sizeof(g_btstack_candidate_addr));
    g_btstack_connect_pending = true;
    pico_w_stack_try_connect_candidate();
    return true;
#else
    (void)device_id;
    return false;
#endif
}

uint8_t pico_w_stack_usb_interface_count(void) {
    return g_usb_interface_count;
}

uint16_t pico_w_stack_usb_report_descriptor_len(uint8_t interface_number) {
    if (interface_number >= g_usb_interface_count) {
        return 0U;
    }

    return g_usb_interface_plan[interface_number].report_descriptor_len;
}

uint8_t pico_w_stack_usb_protocol_mode(uint8_t interface_number) {
    if (interface_number >= g_usb_interface_count) {
        return HID_TRANSPORT_PROTOCOL_UNKNOWN;
    }

    return g_usb_interface_plan[interface_number].protocol_mode;
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

bool pico_w_stack_send_bt_report(uint16_t hid_cid, uint8_t protocol_mode, const uint8_t *report, uint16_t report_len) {
#ifdef APP_PICO_HAS_BTSTACK
    uint8_t report_id = 0U;
    const uint8_t *payload = report;
    uint8_t payload_len = 0U;

    if ((hid_cid == 0U) || (report_len == 0U) || (report == NULL) || (report_len > UINT8_MAX)) {
        return false;
    }

    payload_len = (uint8_t)report_len;

    if ((protocol_mode == HID_TRANSPORT_PROTOCOL_REPORT) && (report_len > 0U)) {
        report_id = report[0];
        payload = &report[1];
        payload_len = (uint8_t)(report_len - 1U);
    }

    if ((protocol_mode == HID_TRANSPORT_PROTOCOL_REPORT) || (protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT)) {
        return hid_host_send_report(hid_cid, report_id, payload, payload_len) == ERROR_CODE_SUCCESS;
    }

    return hid_host_send_set_report(hid_cid,
                                    HID_REPORT_TYPE_OUTPUT,
                                    0U,
                                    report,
                                    (uint8_t)report_len) == ERROR_CODE_SUCCESS;
#else
    (void)hid_cid;
    (void)protocol_mode;
    (void)report;
    (void)report_len;
    return false;
#endif
}
