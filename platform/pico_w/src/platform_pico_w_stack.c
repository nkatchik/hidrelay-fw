#include "platform_pico_w_stack.h"

#include <stddef.h>
#include <string.h>

#ifdef APP_PICO_HAS_BTSTACK
#include "ad_parser.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/gatt_client.h"
#include "ble/le_device_db.h"
#include "ble/le_device_db_tlv.h"
#include "ble/sm.h"
#include "bluetooth.h"
#include "bluetooth_data_types.h"
#include "bluetooth_gatt.h"
#include "btstack_event.h"
#include "btstack_tlv.h"
#include "btstack_tlv_flash_bank.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/hid_host.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "pico/btstack_cyw43.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#endif

#include "hid_report_policy.h"
#include "hid_report_remap.h"
#include "platform_pico_w_tinyusb_runtime.h"

enum {
    PICO_W_STACK_MAX_USB_INTERFACE = 8U,
    PICO_W_STACK_EVENT_QUEUE_SIZE = 16U,
    PICO_W_STACK_INQUIRY_DURATION_UNITS = 0x08U,
    PICO_W_STACK_MAJOR_CLASS_PERIPHERAL = 0x05U,
};

static uint8_t g_usb_interface_count = 1U;
static uint32_t g_usb_descriptor_generation = 0U;
static hid_transport_usb_interface_plan_t g_usb_interface_plan[PICO_W_STACK_MAX_USB_INTERFACE] = {
    0
};
static hid_transport_event_t g_event_queue[PICO_W_STACK_EVENT_QUEUE_SIZE] = {0};
static uint8_t g_event_queue_head = 0U;
static uint8_t g_event_queue_tail = 0U;
static uint8_t g_event_queue_count = 0U;
static uint8_t g_event_queue_high_watermark = 0U;
static uint32_t g_event_queue_dropped = 0U;
static uint8_t g_stack_last_connect_status = 0U;

static bool pico_w_stack_event_is_report_type(uint8_t event_type) {
    return (event_type == HID_TRANSPORT_EVENT_BT_HID_REPORT)
        || (event_type == HID_TRANSPORT_EVENT_USB_HID_REPORT);
}

static bool pico_w_stack_drop_oldest_report_event(void) {
    hid_transport_event_t ordered[PICO_W_STACK_EVENT_QUEUE_SIZE] = {0};
    uint8_t ordered_count = 0U;
    uint8_t drop_index = 0U;
    bool found_report = false;
    uint8_t index = 0U;

    if (g_event_queue_count == 0U) {
        return false;
    }

    for (index = 0U; index < g_event_queue_count; index++) {
        ordered[index] =
            g_event_queue[(g_event_queue_head + index) % PICO_W_STACK_EVENT_QUEUE_SIZE];
        ordered_count = (uint8_t)(ordered_count + 1U);
    }

    for (index = 0U; index < ordered_count; index++) {
        if (!pico_w_stack_event_is_report_type(ordered[index].type)) {
            continue;
        }

        drop_index = index;
        found_report = true;
        break;
    }

    if (!found_report) {
        return false;
    }

    (void)memset(g_event_queue, 0, sizeof(g_event_queue));
    g_event_queue_head = 0U;
    g_event_queue_tail = 0U;
    g_event_queue_count = 0U;

    for (index = 0U; index < ordered_count; index++) {
        if (index == drop_index) {
            continue;
        }

        g_event_queue[g_event_queue_tail] = ordered[index];
        g_event_queue_tail = (uint8_t)((g_event_queue_tail + 1U) % PICO_W_STACK_EVENT_QUEUE_SIZE);
        g_event_queue_count = (uint8_t)(g_event_queue_count + 1U);
    }

    return true;
}

static bool pico_w_stack_push_event(const hid_transport_event_t * event) {
    if ((event == NULL) || (event->type == HID_TRANSPORT_EVENT_NONE)) {
        return false;
    }

    if (g_event_queue_count >= PICO_W_STACK_EVENT_QUEUE_SIZE) {
        /*
         * Keep control-path events (open/close/reconnect outcomes) flowing even
         * when report traffic bursts saturate the queue.
         */
        if (!pico_w_stack_event_is_report_type(event->type)
            && pico_w_stack_drop_oldest_report_event()) {
            /* fallthrough: queue has space now */
        } else {
            g_event_queue_dropped = g_event_queue_dropped + 1U;
            return false;
        }
    }

    if (g_event_queue_count >= PICO_W_STACK_EVENT_QUEUE_SIZE) {
        g_event_queue_dropped = g_event_queue_dropped + 1U;
        return false;
    }

    g_event_queue[g_event_queue_tail] = *event;
    g_event_queue_tail = (uint8_t)((g_event_queue_tail + 1U) % PICO_W_STACK_EVENT_QUEUE_SIZE);
    g_event_queue_count = (uint8_t)(g_event_queue_count + 1U);

    if (g_event_queue_count > g_event_queue_high_watermark) {
        g_event_queue_high_watermark = g_event_queue_count;
    }

    return true;
}

static bool pico_w_stack_device_id_equal(
    const pair_device_id_t * lhs,
    const pair_device_id_t * rhs
) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static bool pico_w_stack_find_hid_cid_for_device(
    const pair_device_id_t * device_id,
    uint16_t * out_hid_cid,
    uint8_t * out_bt_link_type
) {
    uint8_t index = 0U;

    if ((device_id == NULL) || (out_hid_cid == NULL) || (out_bt_link_type == NULL)) {
        return false;
    }

    for (index = 0U; index < g_usb_interface_count; index++) {
        if (!pico_w_stack_device_id_equal(&g_usb_interface_plan[index].device_id, device_id)
            || (g_usb_interface_plan[index].hid_cid == 0U)) {
            continue;
        }

        *out_hid_cid = g_usb_interface_plan[index].hid_cid;
        *out_bt_link_type = g_usb_interface_plan[index].bt_link_type;
        return true;
    }

    return false;
}

#ifdef APP_PICO_HAS_BTSTACK
enum {
    PICO_W_STACK_LE_SCAN_INTERVAL = 48U,
    PICO_W_STACK_LE_SCAN_WINDOW = 48U,
    PICO_W_STACK_LE_CONN_SCAN_INTERVAL = 48U,
    PICO_W_STACK_LE_CONN_SCAN_WINDOW = 48U,
    PICO_W_STACK_LE_CONN_INTERVAL_MIN = 9U,
    PICO_W_STACK_LE_CONN_INTERVAL_MAX = 12U,
    PICO_W_STACK_LE_CONN_LATENCY = 0U,
    PICO_W_STACK_LE_CONN_SUPERVISION_TIMEOUT = 40U,
    PICO_W_STACK_LE_CONN_CE_LENGTH_MIN = 0U,
    PICO_W_STACK_LE_CONN_CE_LENGTH_MAX = 0U,
    PICO_W_STACK_LE_HIDS_SERVICE_INDEX = 0U,
    PICO_W_STACK_RECONNECT_MAX_ATTEMPT = 4U,
    PICO_W_STACK_RECONNECT_CMD_DISALLOWED_TIMEOUT_MS = 1500U,
    PICO_W_STACK_RECONNECT_CONNECT_PENDING_TIMEOUT_MS = 7000U,
    PICO_W_STACK_RECONNECT_LE_DIRECT_PENDING_TIMEOUT_MS = 3500U,
    PICO_W_STACK_RECONNECT_HIDS_PENDING_TIMEOUT_MS = 3000U,
    PICO_W_STACK_RECONNECT_SECURITY_PENDING_TIMEOUT_MS = 30000U,
    PICO_W_STACK_LE_SCAN_TYPE_ACTIVE = 1U,
    PICO_W_STACK_LE_ADV_EVENT_CONNECTABLE_UNDIRECTED = 0x00U,
    PICO_W_STACK_LE_ADV_EVENT_CONNECTABLE_DIRECTED = 0x01U,
    PICO_W_STACK_LE_ADV_EVENT_SCAN_RESPONSE = 0x04U,
    PICO_W_STACK_LE_HID_APPEARANCE_MASK = 0xFFC0U,
    PICO_W_STACK_LE_HID_APPEARANCE_BASE = 0x03C0U,
};

typedef enum {
    PICO_W_STACK_CONNECT_MODE_NONE = 0,
    PICO_W_STACK_CONNECT_MODE_CLASSIC,
    PICO_W_STACK_CONNECT_MODE_LE,
    PICO_W_STACK_CONNECT_MODE_LE_WHITELIST
} pico_w_stack_connect_mode_t;

typedef enum {
    PICO_W_STACK_PAIRING_FAILURE_PHASE_NONE = 0,
    PICO_W_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT,
    PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT,
    PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_HIDS
} pico_w_stack_pairing_failure_phase_t;

static uint8_t pico_w_stack_diag_connect_mode(pico_w_stack_connect_mode_t mode) {
    switch (mode) {
        case PICO_W_STACK_CONNECT_MODE_CLASSIC:
            return 1U;
        case PICO_W_STACK_CONNECT_MODE_LE_WHITELIST:
        case PICO_W_STACK_CONNECT_MODE_LE:
            return 2U;
        case PICO_W_STACK_CONNECT_MODE_NONE:
        default:
            return 0U;
    }
}

typedef struct {
    bool connected;
    hci_con_handle_t con_handle;
    uint16_t hids_cid;
    bd_addr_t addr;
    bd_addr_type_t addr_type;
} pico_w_stack_le_session_t;

typedef struct {
    pico_w_stack_connect_mode_t mode;
    bd_addr_type_t addr_type;
} pico_w_stack_reconnect_attempt_t;

typedef struct {
    bool used;
    hci_con_handle_t con_handle;
    uint16_t hid_cid;
} pico_w_stack_classic_session_t;

static uint8_t g_btstack_hid_descriptor_storage[1024] = {0};
static btstack_tlv_flash_bank_t g_btstack_tlv_flash_bank_context = {0};
static btstack_packet_callback_registration_t g_btstack_hci_event_callback_registration = {0};
static btstack_packet_callback_registration_t g_btstack_sm_event_callback_registration = {0};
static bool g_btstack_available = false;
static bool g_btstack_hci_ready = false;
static bool g_btstack_resolving_list_requested = false;
static bool g_btstack_pairing_active = false;
static bool g_btstack_pairing_attempt_consumed = false;
static bool g_btstack_pairing_auth_attempted = false;
static uint8_t g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
static bool g_btstack_inquiry_active = false;
static bool g_btstack_scan_active = false;
static bool g_btstack_connect_pending = false;
static bool g_btstack_connect_command_issued = false;
static bool g_btstack_reconnect_pending = false;
static bool g_btstack_reconnect_auth_attempted = false;
static bool g_btstack_le_security_pending = false;
static bool g_btstack_le_hids_connect_pending = false;
static uint32_t g_btstack_now_ms = 0U;
static uint32_t g_btstack_connect_pending_since_ms = 0U;
static uint32_t g_btstack_le_security_pending_since_ms = 0U;
static uint32_t g_btstack_le_hids_pending_since_ms = 0U;
static bd_addr_t g_btstack_candidate_addr = {0};
static bd_addr_type_t g_btstack_candidate_addr_type = BD_ADDR_TYPE_UNKNOWN;
static pico_w_stack_connect_mode_t g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
static pico_w_stack_pairing_failure_phase_t g_btstack_pairing_failure_phase =
    PICO_W_STACK_PAIRING_FAILURE_PHASE_NONE;
static pico_w_stack_reconnect_attempt_t
    g_btstack_reconnect_attempt[PICO_W_STACK_RECONNECT_MAX_ATTEMPT] = {0};
static uint8_t g_btstack_reconnect_attempt_count = 0U;
static uint8_t g_btstack_reconnect_attempt_index = 0U;
static pico_w_stack_le_session_t g_btstack_le_session = {
    .connected = false,
    .con_handle = HCI_CON_HANDLE_INVALID,
    .hids_cid = 0U,
    .addr = {0},
    .addr_type = BD_ADDR_TYPE_UNKNOWN,
};
static pico_w_stack_classic_session_t g_btstack_classic_session[PICO_W_STACK_MAX_USB_INTERFACE] = {
    0
};

static void pico_w_btstack_packet_handler(
    uint8_t packet_type,
    uint16_t channel,
    uint8_t * packet,
    uint16_t size
);

static bool pico_w_stack_device_id_valid(const pair_device_id_t * device_id) {
    static const pair_device_id_t zero_id = {.bytes = {0U, 0U, 0U, 0U, 0U, 0U}};

    if (device_id == NULL) {
        return false;
    }

    return memcmp(device_id->bytes, zero_id.bytes, sizeof(device_id->bytes)) != 0;
}

static void pico_w_stack_copy_device_id_from_addr(
    pair_device_id_t * device_id,
    const bd_addr_t addr
) {
    if ((device_id == NULL) || (addr == NULL)) {
        return;
    }

    (void)memcpy(device_id->bytes, addr, sizeof(device_id->bytes));
}

static void pico_w_stack_copy_addr_from_device_id(
    bd_addr_t addr,
    const pair_device_id_t * device_id
) {
    if ((addr == NULL) || (device_id == NULL)) {
        return;
    }

    (void)memcpy(addr, device_id->bytes, sizeof(device_id->bytes));
}

static void pico_w_stack_reset_le_session(void) {
    g_btstack_le_session.connected = false;
    g_btstack_le_session.con_handle = HCI_CON_HANDLE_INVALID;
    g_btstack_le_session.hids_cid = 0U;
    g_btstack_le_security_pending = false;
    g_btstack_le_security_pending_since_ms = 0U;
    g_btstack_le_hids_connect_pending = false;
    g_btstack_le_hids_pending_since_ms = 0U;
    (void)memset(g_btstack_le_session.addr, 0, sizeof(g_btstack_le_session.addr));
    g_btstack_le_session.addr_type = BD_ADDR_TYPE_UNKNOWN;
}

static void pico_w_stack_abandon_unconnected_le_session(void) {
    if (g_btstack_le_session.connected) {
        return;
    }

    if (g_btstack_le_session.hids_cid != 0U) {
        (void)hids_client_disconnect(g_btstack_le_session.hids_cid);
    }

    if (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID) {
        (void)gap_disconnect(g_btstack_le_session.con_handle);
    }

    pico_w_stack_reset_le_session();
}

static void pico_w_stack_update_le_identity(
    hci_con_handle_t con_handle,
    bd_addr_type_t identity_addr_type,
    const bd_addr_t identity_addr
) {
    if ((con_handle == HCI_CON_HANDLE_INVALID)
        || (con_handle != g_btstack_le_session.con_handle)
        || (identity_addr == NULL)) {
        return;
    }

    (void)memcpy(g_btstack_le_session.addr, identity_addr, sizeof(g_btstack_le_session.addr));
    g_btstack_le_session.addr_type = identity_addr_type;
}

static void pico_w_stack_reset_classic_session(void) {
    (void)memset(g_btstack_classic_session, 0, sizeof(g_btstack_classic_session));
}

static void pico_w_stack_remember_classic_session(
    uint16_t hid_cid,
    hci_con_handle_t con_handle
) {
    uint8_t index = 0U;

    if ((hid_cid == 0U) || (con_handle == HCI_CON_HANDLE_INVALID)) {
        return;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used) {
            continue;
        }

        if ((g_btstack_classic_session[index].hid_cid == hid_cid)
            || (g_btstack_classic_session[index].con_handle == con_handle)) {
            g_btstack_classic_session[index].hid_cid = hid_cid;
            g_btstack_classic_session[index].con_handle = con_handle;
            return;
        }
    }

    for (index = 0U; index < PICO_W_STACK_MAX_USB_INTERFACE; index++) {
        if (g_btstack_classic_session[index].used) {
            continue;
        }

        g_btstack_classic_session[index].used = true;
        g_btstack_classic_session[index].hid_cid = hid_cid;
        g_btstack_classic_session[index].con_handle = con_handle;
        return;
    }

    g_btstack_classic_session[0].used = true;
    g_btstack_classic_session[0].hid_cid = hid_cid;
    g_btstack_classic_session[0].con_handle = con_handle;
}

static void pico_w_stack_forget_classic_session_by_hid_cid(uint16_t hid_cid) {
    uint8_t index = 0U;

    if (hid_cid == 0U) {
        return;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used
            || (g_btstack_classic_session[index].hid_cid != hid_cid)) {
            continue;
        }

        (void)
            memset(&g_btstack_classic_session[index], 0, sizeof(g_btstack_classic_session[index]));
        return;
    }
}

static bool pico_w_stack_find_classic_hid_cid_by_con_handle(
    hci_con_handle_t con_handle,
    uint16_t * out_hid_cid
) {
    uint8_t index = 0U;

    if ((con_handle == HCI_CON_HANDLE_INVALID) || (out_hid_cid == NULL)) {
        return false;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used
            || (g_btstack_classic_session[index].con_handle != con_handle)) {
            continue;
        }

        *out_hid_cid = g_btstack_classic_session[index].hid_cid;
        return true;
    }

    return false;
}

static bool pico_w_stack_security_accept(void) {
    return g_btstack_pairing_active
        || g_btstack_reconnect_pending
        || (g_btstack_connect_mode != PICO_W_STACK_CONNECT_MODE_NONE);
}

static bool pico_w_stack_valid_pairing_link_type(uint8_t bt_link_type) {
    return (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC);
}

static bool pico_w_stack_status_is_auth_related(uint8_t status_code) {
    switch (status_code) {
        case ERROR_CODE_AUTHENTICATION_FAILURE:
        case ERROR_CODE_PAIRING_NOT_ALLOWED:
        case ERROR_CODE_PIN_OR_KEY_MISSING:
        case ERROR_CODE_CONNECTION_REJECTED_DUE_TO_SECURITY_REASONS:
        case ERROR_CODE_INSUFFICIENT_SECURITY:
            return true;
        default:
            return false;
    }
}

static uint8_t pico_w_stack_classify_reconnect_failure(
    uint8_t status_code,
    bool auth_attempted
) {
    if ((status_code == ERROR_CODE_COMMAND_DISALLOWED)
        || (status_code == BTSTACK_MEMORY_ALLOC_FAILED)) {
        return HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED;
    }

    if (auth_attempted || pico_w_stack_status_is_auth_related(status_code)) {
        return HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED;
    }

    return HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
}

static uint8_t pico_w_stack_classify_pairing_failure(
    uint8_t status_code,
    bool auth_attempted
) {
    const uint8_t base_result =
        pico_w_stack_classify_reconnect_failure(status_code, auth_attempted);

    if (base_result != HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED) {
        return base_result;
    }

    switch (g_btstack_pairing_failure_phase) {
        case PICO_W_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_CLASSIC_CONNECT_FAILED;
        case PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_CONNECT_FAILED;
        case PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_HIDS:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_HIDS_FAILED;
        case PICO_W_STACK_PAIRING_FAILURE_PHASE_NONE:
        default:
            return base_result;
    }
}

static void pico_w_stack_emit_reconnect_result(
    const pair_device_id_t * device_id,
    uint8_t reconnect_result,
    uint8_t status_code
) {
    hid_transport_event_t event = {0};

    if (device_id == NULL) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = *device_id;
    event.reconnect_result = reconnect_result;
    event.status_code = status_code;
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_reconnect_result_for_candidate(
    uint8_t reconnect_result,
    uint8_t status_code
) {
    pair_device_id_t device_id = {0};

    pico_w_stack_copy_device_id_from_addr(&device_id, g_btstack_candidate_addr);
    pico_w_stack_emit_reconnect_result(&device_id, reconnect_result, status_code);
}

static void pico_w_stack_clear_reconnect_state(void) {
    g_btstack_reconnect_pending = false;
    g_btstack_reconnect_auth_attempted = false;
    g_btstack_reconnect_attempt_count = 0U;
    g_btstack_reconnect_attempt_index = 0U;
    (void)memset(g_btstack_reconnect_attempt, 0, sizeof(g_btstack_reconnect_attempt));
}

static void pico_w_stack_mark_pairing_auth_attempted(void) {
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        g_btstack_pairing_auth_attempted = true;
    }
}

static void pico_w_stack_clear_pairing_failure_phase(void) {
    g_btstack_pairing_failure_phase = PICO_W_STACK_PAIRING_FAILURE_PHASE_NONE;
}

static void pico_w_stack_set_pairing_failure_phase(pico_w_stack_pairing_failure_phase_t phase) {
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        g_btstack_pairing_failure_phase = phase;
    }
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

static uint8_t pico_w_stack_map_bt_addr_type_from_stack(bd_addr_type_t addr_type) {
    switch (addr_type) {
        case BD_ADDR_TYPE_ACL:
            return HID_TRANSPORT_BT_ADDR_TYPE_ACL;
        case BD_ADDR_TYPE_LE_PUBLIC:
            return HID_TRANSPORT_BT_ADDR_TYPE_LE_PUBLIC;
        case BD_ADDR_TYPE_LE_PUBLIC_IDENTITY:
            return HID_TRANSPORT_BT_ADDR_TYPE_LE_PUBLIC_IDENTITY;
        case BD_ADDR_TYPE_LE_RANDOM:
            return HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM;
        case BD_ADDR_TYPE_LE_RANDOM_IDENTITY:
            return HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM_IDENTITY;
        default:
            return HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN;
    }
}

static bd_addr_type_t pico_w_stack_map_bt_addr_type_to_stack(uint8_t addr_type) {
    switch (addr_type) {
        case HID_TRANSPORT_BT_ADDR_TYPE_ACL:
            return BD_ADDR_TYPE_ACL;
        case HID_TRANSPORT_BT_ADDR_TYPE_LE_PUBLIC:
            return BD_ADDR_TYPE_LE_PUBLIC;
        case HID_TRANSPORT_BT_ADDR_TYPE_LE_PUBLIC_IDENTITY:
            return BD_ADDR_TYPE_LE_PUBLIC_IDENTITY;
        case HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM:
            return BD_ADDR_TYPE_LE_RANDOM;
        case HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM_IDENTITY:
            return BD_ADDR_TYPE_LE_RANDOM_IDENTITY;
        default:
            return BD_ADDR_TYPE_UNKNOWN;
    }
}

static bool pico_w_stack_is_valid_le_addr_type(bd_addr_type_t addr_type) {
    return (addr_type == BD_ADDR_TYPE_LE_PUBLIC)
        || (addr_type == BD_ADDR_TYPE_LE_RANDOM)
        || (addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY)
        || (addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
}

static void pico_w_stack_request_le_low_latency_params(void) {
    if (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    /*
     * Best-effort latency reduction: peripherals may reject or ignore this
     * request, in which case the existing negotiated parameters remain active.
     */
    (void)gap_request_connection_parameter_update(
        g_btstack_le_session.con_handle,
        PICO_W_STACK_LE_CONN_INTERVAL_MIN,
        PICO_W_STACK_LE_CONN_INTERVAL_MAX,
        PICO_W_STACK_LE_CONN_LATENCY,
        PICO_W_STACK_LE_CONN_SUPERVISION_TIMEOUT
    );
}

static uint8_t pico_w_stack_connect_with_reconnect_whitelist(void) {
    int entry_index = 0;
    int entry_count = 0;
    int max_entry_count = 0;
    int selected_index = -1;
    int entry_addr_type = BD_ADDR_TYPE_UNKNOWN;
    bd_addr_t entry_addr = {0};
    sm_key_t entry_irk = {0};
    uint8_t status = ERROR_CODE_SUCCESS;

    status = gap_whitelist_clear();
    if ((status != ERROR_CODE_SUCCESS) && (status != ERROR_CODE_COMMAND_DISALLOWED)) {
        return status;
    }

    entry_count = le_device_db_count();
    max_entry_count = le_device_db_max_count();

    for (entry_index = 0; entry_index < max_entry_count; entry_index++) {
        entry_addr_type = BD_ADDR_TYPE_UNKNOWN;
        (void)memset(entry_addr, 0, sizeof(entry_addr));
        (void)memset(entry_irk, 0, sizeof(entry_irk));
        le_device_db_info(entry_index, &entry_addr_type, entry_addr, entry_irk);

        if (!pico_w_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
            continue;
        }

        if (memcmp(entry_addr, g_btstack_candidate_addr, sizeof(entry_addr)) == 0) {
            selected_index = entry_index;
            break;
        }
    }

    if ((selected_index < 0) && (entry_count == 1)) {
        /*
         * Recover stale app-side LE address metadata by falling back to the
         * single bonded LE entry when only one device exists.
         */
        for (entry_index = 0; entry_index < max_entry_count; entry_index++) {
            entry_addr_type = BD_ADDR_TYPE_UNKNOWN;
            (void)memset(entry_addr, 0, sizeof(entry_addr));
            (void)memset(entry_irk, 0, sizeof(entry_irk));
            le_device_db_info(entry_index, &entry_addr_type, entry_addr, entry_irk);

            if (!pico_w_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
                continue;
            }

            selected_index = entry_index;
            break;
        }
    }

    if (selected_index < 0) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    entry_addr_type = BD_ADDR_TYPE_UNKNOWN;
    (void)memset(entry_addr, 0, sizeof(entry_addr));
    (void)memset(entry_irk, 0, sizeof(entry_irk));
    le_device_db_info(selected_index, &entry_addr_type, entry_addr, entry_irk);
    if (!pico_w_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    status = gap_whitelist_add((bd_addr_type_t)entry_addr_type, entry_addr);
    if ((status != ERROR_CODE_SUCCESS) && (status != ERROR_CODE_COMMAND_DISALLOWED)) {
        return status;
    }

    return gap_connect_with_whitelist();
}

static bool pico_w_stack_pairing_policy_allows_cod(uint32_t class_of_device) {
    const uint8_t major_class = (uint8_t)((class_of_device >> 8U) & 0x1FU);
    return major_class == PICO_W_STACK_MAJOR_CLASS_PERIPHERAL;
}

static bool pico_w_stack_pairing_policy_adv_has_hid_uuid(const uint8_t * packet) {
    const uint8_t * ad_data = NULL;
    uint8_t ad_len = 0U;

    if (packet == NULL) {
        return false;
    }

    ad_data = gap_event_advertising_report_get_data(packet);
    ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static char pico_w_stack_ascii_lower(char value) {
    if ((value >= 'A') && (value <= 'Z')) {
        return (char)(value + ('a' - 'A'));
    }

    return value;
}

static bool pico_w_stack_ascii_contains_fragment(
    const uint8_t * data,
    uint8_t data_len,
    const char * fragment
) {
    uint8_t fragment_len = 0U;
    uint8_t data_index = 0U;

    if ((data == NULL) || (fragment == NULL)) {
        return false;
    }

    while (fragment[fragment_len] != '\0') {
        fragment_len++;
    }

    if ((fragment_len == 0U) || (fragment_len > data_len)) {
        return false;
    }

    for (data_index = 0U; data_index <= (uint8_t)(data_len - fragment_len); data_index++) {
        uint8_t fragment_index = 0U;
        bool matched = true;

        for (fragment_index = 0U; fragment_index < fragment_len; fragment_index++) {
            if (pico_w_stack_ascii_lower((char)data[data_index + fragment_index])
                != pico_w_stack_ascii_lower(fragment[fragment_index])) {
                matched = false;
                break;
            }
        }

        if (matched) {
            return true;
        }
    }

    return false;
}

static bool pico_w_stack_pairing_policy_adv_has_hid_name(const uint8_t * packet) {
    ad_context_t ad_context = {0};
    const uint8_t * ad_data = NULL;
    uint8_t ad_len = 0U;

    if (packet == NULL) {
        return false;
    }

    ad_data = gap_event_advertising_report_get_data(packet);
    ad_len = gap_event_advertising_report_get_data_length(packet);
    if ((ad_data == NULL) || (ad_len == 0U)) {
        return false;
    }

    for (ad_iterator_init(&ad_context, ad_len, ad_data); ad_iterator_has_more(&ad_context);
        ad_iterator_next(&ad_context)) {
        const uint8_t data_type = ad_iterator_get_data_type(&ad_context);
        const uint8_t data_len = ad_iterator_get_data_len(&ad_context);
        const uint8_t * data = ad_iterator_get_data(&ad_context);

        if (((data_type != BLUETOOTH_DATA_TYPE_SHORTENED_LOCAL_NAME)
                && (data_type != BLUETOOTH_DATA_TYPE_COMPLETE_LOCAL_NAME))
            || (data_len == 0U)
            || (data == NULL)) {
            continue;
        }

        if (pico_w_stack_ascii_contains_fragment(data, data_len, "keyboard")
            || pico_w_stack_ascii_contains_fragment(data, data_len, "mouse")
            || pico_w_stack_ascii_contains_fragment(data, data_len, "trackpad")) {
            return true;
        }
    }

    return false;
}

static bool pico_w_stack_pairing_policy_adv_connectable(const uint8_t * packet) {
    const uint8_t advertising_event_type =
        gap_event_advertising_report_get_advertising_event_type(packet);
    return (advertising_event_type == PICO_W_STACK_LE_ADV_EVENT_CONNECTABLE_UNDIRECTED)
        || (advertising_event_type == PICO_W_STACK_LE_ADV_EVENT_CONNECTABLE_DIRECTED);
}

static bool pico_w_stack_pairing_policy_adv_scan_response(const uint8_t * packet) {
    const uint8_t advertising_event_type =
        gap_event_advertising_report_get_advertising_event_type(packet);
    return advertising_event_type == PICO_W_STACK_LE_ADV_EVENT_SCAN_RESPONSE;
}

static bool pico_w_stack_pairing_policy_adv_has_hid_appearance(const uint8_t * packet) {
    ad_context_t ad_context = {0};
    const uint8_t * ad_data = NULL;
    uint8_t ad_len = 0U;

    if (packet == NULL) {
        return false;
    }

    ad_data = gap_event_advertising_report_get_data(packet);
    ad_len = gap_event_advertising_report_get_data_length(packet);
    if ((ad_data == NULL) || (ad_len == 0U)) {
        return false;
    }

    for (ad_iterator_init(&ad_context, ad_len, ad_data); ad_iterator_has_more(&ad_context);
        ad_iterator_next(&ad_context)) {
        const uint8_t data_type = ad_iterator_get_data_type(&ad_context);
        const uint8_t data_len = ad_iterator_get_data_len(&ad_context);
        const uint8_t * data = ad_iterator_get_data(&ad_context);
        uint16_t appearance = 0U;

        if ((data_type != BLUETOOTH_DATA_TYPE_APPEARANCE) || (data_len < 2U) || (data == NULL)) {
            continue;
        }

        appearance = (uint16_t)data[0] | ((uint16_t)data[1] << 8U);
        return (appearance & PICO_W_STACK_LE_HID_APPEARANCE_MASK)
            == PICO_W_STACK_LE_HID_APPEARANCE_BASE;
    }

    return false;
}

static void pico_w_stack_stop_inquiry(void) {
    if (!g_btstack_inquiry_active) {
        return;
    }

    (void)gap_inquiry_stop();
    g_btstack_inquiry_active = false;
}

static void pico_w_stack_stop_scan(void) {
    if (!g_btstack_scan_active) {
        return;
    }

    gap_stop_scan();
    g_btstack_scan_active = false;
}

static void pico_w_stack_stop_discovery(void) {
    pico_w_stack_stop_inquiry();
    pico_w_stack_stop_scan();
}

static void pico_w_stack_sync_hci_ready(void) {
    if (!g_btstack_available || g_btstack_hci_ready) {
        return;
    }

    if (hci_get_state() != HCI_STATE_WORKING) {
        return;
    }

    g_btstack_hci_ready = true;
    if (!g_btstack_resolving_list_requested) {
        (void)gap_load_resolving_list_from_le_device_db();
        g_btstack_resolving_list_requested = true;
    }
}

static void pico_w_stack_try_start_inquiry(void) {
    if (!g_btstack_hci_ready
        || !g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_CLASSIC)
        || g_btstack_pairing_attempt_consumed
        || (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID)
        || g_btstack_connect_pending
        || g_btstack_inquiry_active) {
        return;
    }

    if (gap_inquiry_start(PICO_W_STACK_INQUIRY_DURATION_UNITS) == ERROR_CODE_SUCCESS) {
        g_btstack_inquiry_active = true;
    }
}

static void pico_w_stack_try_start_scan(void) {
    if (!g_btstack_hci_ready
        || !g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)
        || g_btstack_pairing_attempt_consumed
        || (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID)
        || g_btstack_connect_pending
        || g_btstack_scan_active) {
        return;
    }

    gap_start_scan();
    g_btstack_scan_active = true;
}

static void pico_w_stack_try_start_discovery(void) {
    pico_w_stack_try_start_scan();
    pico_w_stack_try_start_inquiry();
}

static void pico_w_stack_schedule_candidate(
    const bd_addr_t addr,
    bd_addr_type_t addr_type,
    pico_w_stack_connect_mode_t mode,
    bool reconnect_pending
) {
    if ((addr == NULL)
        || g_btstack_connect_pending
        || (mode == PICO_W_STACK_CONNECT_MODE_NONE)
        || (g_btstack_pairing_active && !reconnect_pending && g_btstack_pairing_attempt_consumed)) {
        return;
    }

    (void)memcpy(g_btstack_candidate_addr, addr, sizeof(g_btstack_candidate_addr));
    g_btstack_candidate_addr_type = addr_type;
    g_btstack_connect_mode = mode;
    g_btstack_connect_pending = true;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_pending_since_ms = g_btstack_now_ms;
    g_btstack_reconnect_pending = reconnect_pending;
    g_btstack_le_hids_pending_since_ms = 0U;
    if (!reconnect_pending) {
        pico_w_stack_clear_reconnect_state();
        if (g_btstack_pairing_active) {
            g_btstack_pairing_attempt_consumed = true;
            g_btstack_pairing_auth_attempted = false;
            if (mode == PICO_W_STACK_CONNECT_MODE_CLASSIC) {
                pico_w_stack_set_pairing_failure_phase(
                    PICO_W_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT
                );
            } else if (mode == PICO_W_STACK_CONNECT_MODE_LE) {
                pico_w_stack_set_pairing_failure_phase(
                    PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT
                );
            } else {
                pico_w_stack_clear_pairing_failure_phase();
            }
            pico_w_stack_emit_reconnect_result_for_candidate(
                HID_TRANSPORT_RECONNECT_RESULT_REQUESTED,
                ERROR_CODE_SUCCESS
            );
        }
    }
    pico_w_stack_stop_discovery();
}

static bool pico_w_stack_reconnect_add_attempt(
    pico_w_stack_connect_mode_t mode,
    bd_addr_type_t addr_type
) {
    uint8_t index = 0U;

    if ((mode == PICO_W_STACK_CONNECT_MODE_NONE)
        || (g_btstack_reconnect_attempt_count >= PICO_W_STACK_RECONNECT_MAX_ATTEMPT)) {
        return false;
    }

    for (index = 0U; index < g_btstack_reconnect_attempt_count; index++) {
        if ((g_btstack_reconnect_attempt[index].mode == mode)
            && (g_btstack_reconnect_attempt[index].addr_type == addr_type)) {
            return false;
        }
    }

    g_btstack_reconnect_attempt[g_btstack_reconnect_attempt_count].mode = mode;
    g_btstack_reconnect_attempt[g_btstack_reconnect_attempt_count].addr_type = addr_type;
    g_btstack_reconnect_attempt_count = (uint8_t)(g_btstack_reconnect_attempt_count + 1U);
    return true;
}

static bool pico_w_stack_reconnect_apply_attempt(uint8_t attempt_index) {
    if (attempt_index >= g_btstack_reconnect_attempt_count) {
        return false;
    }

    g_btstack_reconnect_attempt_index = attempt_index;
    g_btstack_connect_mode = g_btstack_reconnect_attempt[attempt_index].mode;
    g_btstack_candidate_addr_type = g_btstack_reconnect_attempt[attempt_index].addr_type;
    g_btstack_connect_pending = true;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_pending_since_ms = g_btstack_now_ms;
    g_btstack_le_hids_pending_since_ms = 0U;
    return true;
}

static bool pico_w_stack_try_reconnect_next_attempt(void) {
    uint8_t next_index = 0U;

    if (!g_btstack_reconnect_pending) {
        return false;
    }

    next_index = (uint8_t)(g_btstack_reconnect_attempt_index + 1U);
    if (next_index >= g_btstack_reconnect_attempt_count) {
        return false;
    }

    return pico_w_stack_reconnect_apply_attempt(next_index);
}

static uint32_t pico_w_stack_reconnect_connect_timeout_ms(void) {
    if (g_btstack_reconnect_pending && (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE)) {
        return PICO_W_STACK_RECONNECT_LE_DIRECT_PENDING_TIMEOUT_MS;
    }

    return PICO_W_STACK_RECONNECT_CONNECT_PENDING_TIMEOUT_MS;
}

static void pico_w_stack_handle_connect_failure(uint8_t status_code);

static void pico_w_stack_request_le_pairing_before_hids(void) {
    if (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID) {
        pico_w_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    pico_w_stack_mark_pairing_auth_attempted();
    g_btstack_le_security_pending = true;
    g_btstack_le_security_pending_since_ms = g_btstack_now_ms;
    g_btstack_le_hids_connect_pending = false;
    g_btstack_le_hids_pending_since_ms = 0U;
    sm_request_pairing(g_btstack_le_session.con_handle);
}

static void pico_w_stack_handle_connect_failure(uint8_t status_code) {
    const bool auth_attempted =
        g_btstack_reconnect_auth_attempted || g_btstack_pairing_auth_attempted;

    if ((g_btstack_connect_mode
            == PICO_W_STACK_CONNECT_MODE_LE
            || g_btstack_connect_mode
            == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST)
        && (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID)) {
        (void)gap_connect_cancel();
    }

    g_stack_last_connect_status = status_code;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    g_btstack_le_security_pending = false;
    g_btstack_le_security_pending_since_ms = 0U;
    g_btstack_le_hids_connect_pending = false;
    g_btstack_le_hids_pending_since_ms = 0U;
    g_btstack_pairing_auth_attempted = false;

    if (g_btstack_reconnect_pending) {
        if (pico_w_stack_try_reconnect_next_attempt()) {
            return;
        }

        pico_w_stack_emit_reconnect_result_for_candidate(
            pico_w_stack_classify_reconnect_failure(status_code, auth_attempted),
            status_code
        );
        pico_w_stack_clear_reconnect_state();
    } else if (g_btstack_pairing_active) {
        pico_w_stack_emit_reconnect_result_for_candidate(
            pico_w_stack_classify_pairing_failure(status_code, auth_attempted),
            status_code
        );
    }

    pico_w_stack_clear_pairing_failure_phase();
    pico_w_stack_try_start_discovery();
}

static bool pico_w_stack_status_requires_pairing_retry(uint8_t status_code) {
    return (status_code == ERROR_CODE_AUTHENTICATION_FAILURE)
        || (status_code == ERROR_CODE_PIN_OR_KEY_MISSING)
        || (status_code == ERROR_CODE_PAIRING_NOT_ALLOWED)
        || (status_code == ERROR_CODE_INSUFFICIENT_SECURITY)
        || (status_code == ATT_ERROR_INSUFFICIENT_AUTHORIZATION)
        || (status_code == ATT_ERROR_INSUFFICIENT_ENCRYPTION)
        || (status_code == ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE);
}

static void pico_w_stack_handle_connect_success(void) {
    g_stack_last_connect_status = ERROR_CODE_SUCCESS;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    g_btstack_le_security_pending = false;
    g_btstack_le_security_pending_since_ms = 0U;
    g_btstack_pairing_auth_attempted = false;
    pico_w_stack_clear_pairing_failure_phase();

    if (g_btstack_reconnect_pending) {
        pico_w_stack_emit_reconnect_result_for_candidate(
            HID_TRANSPORT_RECONNECT_RESULT_SUCCESS,
            ERROR_CODE_SUCCESS
        );
        pico_w_stack_clear_reconnect_state();
    }
}

static void pico_w_stack_emit_classic_open_event(uint8_t * packet) {
    hid_transport_event_t event = {0};
    bd_addr_t device_addr = {0};

    if (packet == NULL) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    event.hid_cid = hid_subevent_connection_opened_get_hid_cid(packet);
    event.vendor_id = 0U;
    event.product_id = 0U;
    event.report_descriptor_len = 0U;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    hid_subevent_connection_opened_get_bd_addr(packet, device_addr);
    (void)memcpy(event.device_id.bytes, device_addr, sizeof(event.device_id.bytes));
    pico_w_stack_remember_classic_session(
        event.hid_cid,
        hid_subevent_connection_opened_get_con_handle(packet)
    );
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_classic_descriptor_event(uint8_t * packet) {
    hid_transport_event_t event = {0};

    if (packet == NULL) {
        return;
    }

    if (hid_subevent_descriptor_available_get_status(packet) != ERROR_CODE_SUCCESS) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.hid_cid = hid_subevent_descriptor_available_get_hid_cid(packet);
    event.report_descriptor_len = hid_descriptor_storage_get_descriptor_len(event.hid_cid);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_bt_protocol_event(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode
) {
    hid_transport_event_t event = {0};

    if ((hid_cid == 0U) || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_PROTOCOL;
    event.hid_cid = hid_cid;
    event.bt_link_type = bt_link_type;
    event.protocol_mode = pico_w_stack_map_protocol_mode(protocol_mode);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_classic_close_by_hid_cid(uint16_t hid_cid) {
    hid_transport_event_t event = {0};

    if (hid_cid == 0U) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.hid_cid = hid_cid;
    pico_w_stack_forget_classic_session_by_hid_cid(hid_cid);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_classic_close_event(uint8_t * packet) {
    if (packet == NULL) {
        return;
    }

    pico_w_stack_emit_classic_close_by_hid_cid(hid_subevent_connection_closed_get_hid_cid(packet));
}

static void pico_w_stack_emit_classic_report_event(uint8_t * packet) {
    const uint8_t * report = NULL;
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
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.hid_cid = hid_subevent_report_get_hid_cid(packet);
    event.report_len = report_len;

    if ((report != NULL) && (report_len > 0U)) {
        (void)memcpy(event.report, report, report_len);
    }

    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_le_open_event(
    uint16_t hids_cid,
    const bd_addr_t addr,
    bd_addr_type_t addr_type
) {
    hid_transport_event_t event = {0};

    if ((hids_cid == 0U) || (addr == NULL)) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = pico_w_stack_map_bt_addr_type_from_stack(addr_type);
    event.hid_cid = hids_cid;
    event.vendor_id = 0U;
    event.product_id = 0U;
    event.report_descriptor_len = 0U;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    (void)memcpy(event.device_id.bytes, addr, sizeof(event.device_id.bytes));
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_le_descriptor_event(
    uint16_t hids_cid,
    uint8_t service_index
) {
    hid_transport_event_t event = {0};

    if (hids_cid == 0U) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.hid_cid = hids_cid;
    event.report_descriptor_len =
        hids_client_descriptor_storage_get_descriptor_len(hids_cid, service_index);
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_le_close_event(
    uint16_t hids_cid,
    const bd_addr_t addr,
    bd_addr_type_t addr_type
) {
    hid_transport_event_t event = {0};

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = pico_w_stack_map_bt_addr_type_from_stack(addr_type);
    event.hid_cid = hids_cid;
    if (addr != NULL) {
        (void)memcpy(event.device_id.bytes, addr, sizeof(event.device_id.bytes));
    }
    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_emit_le_report_event(uint8_t * packet) {
    const uint8_t * report = NULL;
    uint16_t report_len = 0U;
    hid_transport_event_t event = {0};

    if (packet == NULL) {
        return;
    }

    report = gattservice_subevent_hid_report_get_report(packet);
    report_len = gattservice_subevent_hid_report_get_report_len(packet);

    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        report_len = HID_TRANSPORT_REPORT_MAX_LEN;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.hid_cid = gattservice_subevent_hid_report_get_hids_cid(packet);
    event.report_len = report_len;

    if ((report != NULL) && (report_len > 0U)) {
        (void)memcpy(event.report, report, report_len);
    }

    (void)pico_w_stack_push_event(&event);
}

static void pico_w_stack_try_connect_candidate(void) {
    uint8_t connect_status = ERROR_CODE_COMMAND_DISALLOWED;
    uint16_t hid_cid = 0U;

    if (!g_btstack_hci_ready || !g_btstack_connect_pending || g_btstack_connect_command_issued) {
        return;
    }

    if (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_CLASSIC) {
        connect_status = hid_host_connect(
            g_btstack_candidate_addr,
            HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT,
            &hid_cid
        );
    } else if (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE) {
        connect_status = gap_connect(g_btstack_candidate_addr, g_btstack_candidate_addr_type);
    } else if (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST) {
        connect_status = pico_w_stack_connect_with_reconnect_whitelist();
    } else {
        g_stack_last_connect_status = ERROR_CODE_COMMAND_DISALLOWED;
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        return;
    }

    if (connect_status == ERROR_CODE_SUCCESS) {
        g_stack_last_connect_status = ERROR_CODE_SUCCESS;
        g_btstack_connect_command_issued = true;
        g_btstack_connect_pending_since_ms = g_btstack_now_ms;
        return;
    }

    if (connect_status == ERROR_CODE_COMMAND_DISALLOWED) {
        g_stack_last_connect_status = connect_status;
        if (g_btstack_reconnect_pending
            && g_btstack_connect_pending
            && ((int32_t)(g_btstack_now_ms - g_btstack_connect_pending_since_ms)
                >= (int32_t)PICO_W_STACK_RECONNECT_CMD_DISALLOWED_TIMEOUT_MS)) {
            pico_w_stack_handle_connect_failure(connect_status);
        }
        return;
    }

    pico_w_stack_handle_connect_failure(connect_status);
}

static void pico_w_stack_start_le_hids_client(void) {
    uint16_t hids_cid = 0U;
    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;

    if (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID) {
        g_btstack_le_hids_connect_pending = false;
        g_btstack_le_hids_pending_since_ms = 0U;
        pico_w_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    pico_w_stack_set_pairing_failure_phase(PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_HIDS);

    status = hids_client_connect(
        g_btstack_le_session.con_handle,
        &pico_w_btstack_packet_handler,
        HID_PROTOCOL_MODE_REPORT,
        &hids_cid
    );

    if (status == ERROR_CODE_SUCCESS) {
        g_btstack_le_session.hids_cid = hids_cid;
        g_btstack_le_hids_connect_pending = false;
        if (g_btstack_le_hids_pending_since_ms == 0U) {
            g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
        }
        return;
    }

    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        g_stack_last_connect_status = status;
        g_btstack_le_hids_connect_pending = true;
        if (g_btstack_le_hids_pending_since_ms == 0U) {
            g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
        }
        return;
    }

    g_btstack_le_hids_connect_pending = false;
    g_btstack_le_hids_pending_since_ms = 0U;
    pico_w_stack_handle_connect_failure(status);
}

static void pico_w_stack_try_start_le_hids_client(void) {
    if (!g_btstack_hci_ready
        || !g_btstack_le_hids_connect_pending
        || g_btstack_le_security_pending
        || (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID)
        || g_btstack_le_session.connected) {
        return;
    }

    pico_w_stack_start_le_hids_client();
}

static void pico_w_stack_handle_hci_inquiry_result(
    uint8_t * packet,
    bool with_rssi
) {
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

    if (!g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_CLASSIC)
        || g_btstack_connect_pending
        || !pico_w_stack_pairing_policy_allows_cod(class_of_device)) {
        return;
    }

    pico_w_stack_schedule_candidate(
        addr,
        BD_ADDR_TYPE_ACL,
        PICO_W_STACK_CONNECT_MODE_CLASSIC,
        false
    );
}

static void pico_w_stack_handle_gap_advertising_report(uint8_t * packet) {
    bd_addr_t addr = {0};
    bd_addr_type_t addr_type = BD_ADDR_TYPE_UNKNOWN;
    bool has_hid_uuid = false;
    bool connectable = false;
    bool scan_response = false;
    bool has_hid_appearance = false;
    bool has_hid_name = false;

    if ((packet == NULL)
        || !g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)
        || g_btstack_connect_pending) {
        return;
    }

    has_hid_uuid = pico_w_stack_pairing_policy_adv_has_hid_uuid(packet);
    connectable = pico_w_stack_pairing_policy_adv_connectable(packet);
    scan_response = pico_w_stack_pairing_policy_adv_scan_response(packet);
    has_hid_appearance = pico_w_stack_pairing_policy_adv_has_hid_appearance(packet);
    has_hid_name = pico_w_stack_pairing_policy_adv_has_hid_name(packet);
    if (!has_hid_uuid
        && !(connectable && (has_hid_appearance || has_hid_name))
        && !(scan_response && (has_hid_appearance || has_hid_name))) {
        return;
    }

    gap_event_advertising_report_get_address(packet, addr);
    addr_type = gap_event_advertising_report_get_address_type(packet);

    pico_w_stack_schedule_candidate(addr, addr_type, PICO_W_STACK_CONNECT_MODE_LE, false);
}

static void pico_w_stack_handle_sm_event(uint8_t * packet) {
    const bool security_accept = pico_w_stack_security_accept();
    bd_addr_t identity_addr = {0};

    if (packet == NULL) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PAIRING_STARTED:
            pico_w_stack_mark_pairing_auth_attempted();
            break;
        case SM_EVENT_JUST_WORKS_REQUEST:
            pico_w_stack_mark_pairing_auth_attempted();
            if (security_accept) {
                (void)sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            }
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
        case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
            pico_w_stack_mark_pairing_auth_attempted();
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            pico_w_stack_mark_pairing_auth_attempted();
            if (security_accept) {
                (void)sm_numeric_comparison_confirm(
                    sm_event_numeric_comparison_request_get_handle(packet)
                );
            }
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            if (sm_event_pairing_complete_get_handle(packet) != g_btstack_le_session.con_handle) {
                break;
            }

            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                g_btstack_le_security_pending = false;
                g_btstack_le_security_pending_since_ms = 0U;
                g_btstack_le_hids_connect_pending = true;
                g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
                pico_w_stack_start_le_hids_client();
            } else {
                const uint8_t status = sm_event_pairing_complete_get_status(packet);
                const uint8_t reason = sm_event_pairing_complete_get_reason(packet);
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                pico_w_stack_mark_pairing_auth_attempted();
                g_btstack_le_security_pending = false;
                g_btstack_le_security_pending_since_ms = 0U;
                g_btstack_le_hids_connect_pending = false;
                pico_w_stack_handle_connect_failure((reason != 0U) ? reason : status);
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            if (sm_event_reencryption_complete_get_handle(packet)
                != g_btstack_le_session.con_handle) {
                break;
            }

            if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                g_btstack_le_security_pending = false;
                g_btstack_le_security_pending_since_ms = 0U;
                g_btstack_le_hids_connect_pending = true;
                g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
                pico_w_stack_start_le_hids_client();
            } else {
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                pico_w_stack_mark_pairing_auth_attempted();
                g_btstack_le_security_pending = false;
                g_btstack_le_security_pending_since_ms = 0U;
                g_btstack_le_hids_connect_pending = false;
                pico_w_stack_handle_connect_failure(
                    sm_event_reencryption_complete_get_status(packet)
                );
            }
            break;
        case SM_EVENT_IDENTITY_CREATED:
            sm_event_identity_created_get_identity_address(packet, identity_addr);
            pico_w_stack_update_le_identity(
                sm_event_identity_created_get_handle(packet),
                (bd_addr_type_t)sm_event_identity_created_get_identity_addr_type(packet),
                identity_addr
            );
            break;
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
            sm_event_identity_resolving_succeeded_get_identity_address(packet, identity_addr);
            pico_w_stack_update_le_identity(
                sm_event_identity_resolving_succeeded_get_handle(packet),
                (bd_addr_type_t)sm_event_identity_resolving_succeeded_get_identity_addr_type(
                    packet
                ),
                identity_addr
            );
            break;
        default:
            break;
    }
}

static void pico_w_stack_handle_gattservice_meta(uint8_t * packet) {
    uint16_t hids_cid = 0U;
    uint8_t status = 0U;

    if (packet == NULL) {
        return;
    }

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            hids_cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            if (status != ERROR_CODE_SUCCESS) {
                if (!g_btstack_le_security_pending
                    && (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID)
                    && pico_w_stack_status_requires_pairing_retry(status)) {
                    if (g_btstack_reconnect_pending) {
                        g_btstack_reconnect_auth_attempted = true;
                    }
                    pico_w_stack_mark_pairing_auth_attempted();
                    g_btstack_le_security_pending = true;
                    g_btstack_le_security_pending_since_ms = g_btstack_now_ms;
                    g_btstack_le_hids_connect_pending = true;
                    g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
                    sm_request_pairing(g_btstack_le_session.con_handle);
                    break;
                }

                if (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID) {
                    (void)gap_disconnect(g_btstack_le_session.con_handle);
                }
                pico_w_stack_handle_connect_failure(status);
                break;
            }

            g_btstack_le_hids_connect_pending = false;
            g_btstack_le_session.connected = true;
            g_btstack_le_session.hids_cid = hids_cid;
            g_btstack_le_hids_pending_since_ms = 0U;
            pico_w_stack_handle_connect_success();
            pico_w_stack_request_le_low_latency_params();
            pico_w_stack_emit_le_open_event(
                hids_cid,
                g_btstack_le_session.addr,
                g_btstack_le_session.addr_type
            );
            pico_w_stack_emit_bt_protocol_event(
                hids_cid,
                HID_TRANSPORT_BT_LINK_TYPE_LE,
                gattservice_subevent_hid_service_connected_get_protocol_mode(packet)
            );
            pico_w_stack_emit_le_descriptor_event(hids_cid, PICO_W_STACK_LE_HIDS_SERVICE_INDEX);
            break;
        case GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE:
            pico_w_stack_emit_bt_protocol_event(
                gattservice_subevent_hid_protocol_mode_get_hids_cid(packet),
                HID_TRANSPORT_BT_LINK_TYPE_LE,
                gattservice_subevent_hid_protocol_mode_get_protocol_mode(packet)
            );
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            pico_w_stack_emit_le_report_event(packet);
            break;
        default:
            break;
    }
}

static void pico_w_stack_handle_le_connection_complete(uint8_t * packet) {
    uint8_t status = ERROR_CODE_SUCCESS;
    static const bd_addr_t zero_addr = {0};

    if (packet == NULL) {
        return;
    }

    status = gap_subevent_le_connection_complete_get_status(packet);

    if ((g_btstack_connect_mode != PICO_W_STACK_CONNECT_MODE_LE)
        && (g_btstack_connect_mode != PICO_W_STACK_CONNECT_MODE_LE_WHITELIST)) {
        if (status == ERROR_CODE_SUCCESS) {
            (void)gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(packet));
        }
        return;
    }

    if (status != ERROR_CODE_SUCCESS) {
        pico_w_stack_reset_le_session();
        pico_w_stack_handle_connect_failure(status);
        return;
    }

    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    g_btstack_le_session.connected = false;
    g_btstack_le_session.hids_cid = 0U;
    g_btstack_le_session.con_handle =
        gap_subevent_le_connection_complete_get_connection_handle(packet);
    g_btstack_le_session.addr_type =
        (bd_addr_type_t)gap_subevent_le_connection_complete_get_peer_address_type(packet);
    gap_subevent_le_connection_complete_get_peer_address(packet, g_btstack_le_session.addr);
    if (g_btstack_reconnect_pending
        && (memcmp(g_btstack_candidate_addr, zero_addr, sizeof(g_btstack_candidate_addr)) != 0)) {
        /*
         * During reconnect, keep app-facing device identity stable to the
         * paired candidate address. Some keyboards reconnect with a rotating
         * private address even when bonded, which can otherwise prevent
         * app-side device-id matching/reconnect bookkeeping.
         */
        (void)memcpy(g_btstack_le_session.addr, g_btstack_candidate_addr, sizeof(bd_addr_t));
        if (g_btstack_candidate_addr_type != BD_ADDR_TYPE_UNKNOWN) {
            g_btstack_le_session.addr_type = g_btstack_candidate_addr_type;
        }
    }
    pico_w_stack_request_le_low_latency_params();
    g_btstack_le_security_pending = false;
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        pico_w_stack_request_le_pairing_before_hids();
        return;
    }

    g_btstack_le_hids_connect_pending = true;
    g_btstack_le_hids_pending_since_ms = g_btstack_now_ms;
    pico_w_stack_start_le_hids_client();
}

static void pico_w_stack_handle_disconnect(uint8_t * packet) {
    const hci_con_handle_t con_handle =
        hci_event_disconnection_complete_get_connection_handle(packet);
    uint16_t classic_hid_cid = 0U;
    uint16_t le_close_hid_cid = 0U;

    if ((packet == NULL) || (con_handle == HCI_CON_HANDLE_INVALID)) {
        return;
    }

    if (con_handle == g_btstack_le_session.con_handle) {
        const bool pairing_attempt_failed = g_btstack_pairing_active
            && !g_btstack_reconnect_pending
            && !g_btstack_le_session.connected;

        le_close_hid_cid = g_btstack_le_session.hids_cid;

        if (le_close_hid_cid == 0U) {
            pair_device_id_t device_id = {0};
            uint8_t bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
            pico_w_stack_copy_device_id_from_addr(&device_id, g_btstack_le_session.addr);
            if (pico_w_stack_find_hid_cid_for_device(&device_id, &le_close_hid_cid, &bt_link_type)
                && (bt_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)) {
                le_close_hid_cid = 0U;
            }
        }

        /*
         * Emit close before reconnect handling so app-level state and LED cues
         * stay consistent even if LE hids_cid tracking was stale.
         */
        pico_w_stack_emit_le_close_event(
            le_close_hid_cid,
            g_btstack_le_session.addr,
            g_btstack_le_session.addr_type
        );

        if (le_close_hid_cid != 0U) {
            /*
             * BTstack HIDS clients are pooled and must be explicitly finalized
             * on disconnect. Without this, the single HIDS slot can remain
             * occupied after a normal keyboard power-off, causing subsequent
             * reconnect attempts to fail with alloc/command-disallowed errors
             * until reboot.
             */
            (void)hids_client_disconnect(le_close_hid_cid);
        }

        if (g_btstack_reconnect_pending || pairing_attempt_failed) {
            pico_w_stack_handle_connect_failure(
                hci_event_disconnection_complete_get_reason(packet)
            );
        }

        g_btstack_le_security_pending = false;
        pico_w_stack_reset_le_session();
        pico_w_stack_try_start_discovery();
        return;
    }

    if (pico_w_stack_find_classic_hid_cid_by_con_handle(con_handle, &classic_hid_cid)) {
        pico_w_stack_emit_classic_close_by_hid_cid(classic_hid_cid);
        return;
    }

    if (g_btstack_pairing_active && g_btstack_connect_pending) {
        pico_w_stack_handle_connect_failure(hci_event_disconnection_complete_get_reason(packet));
    }
}

static void pico_w_btstack_packet_handler(
    uint8_t packet_type,
    uint16_t channel,
    uint8_t * packet,
    uint16_t size
) {
    (void)channel;

    if ((packet == NULL) || (size < 2U)) {
        return;
    }

    if (packet_type == HCI_EVENT_GATTSERVICE_META) {
        if (size < 3U) {
            return;
        }

        pico_w_stack_handle_gattservice_meta(packet);
        return;
    }

    if (packet_type != HCI_EVENT_PACKET || (size < 3U)) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t address = {0};
            const bool security_accept = pico_w_stack_security_accept();

            hci_event_pin_code_request_get_bd_addr(packet, address);
            if (security_accept) {
                pico_w_stack_mark_pairing_auth_attempted();
            }
            if (security_accept) {
                (void)gap_pin_code_response(address, "0000");
            } else {
                (void)gap_pin_code_negative(address);
            }
        } break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t address = {0};
            const bool security_accept = pico_w_stack_security_accept();

            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (security_accept) {
                pico_w_stack_mark_pairing_auth_attempted();
            }
            if (security_accept) {
                (void)gap_ssp_confirmation_response(address);
            } else {
                (void)gap_ssp_confirmation_negative(address);
            }
        } break;
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                g_btstack_hci_ready = true;
                if (!g_btstack_resolving_list_requested) {
                    (void)gap_load_resolving_list_from_le_device_db();
                    g_btstack_resolving_list_requested = true;
                }
                pico_w_stack_try_start_discovery();
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
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            pico_w_stack_handle_gap_advertising_report(packet);
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet)
                == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                pico_w_stack_handle_le_connection_complete(packet);
            }
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            pico_w_stack_handle_disconnect(packet);
            break;
        case HCI_EVENT_GATTSERVICE_META:
            pico_w_stack_handle_gattservice_meta(packet);
            break;
        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    if (pico_w_stack_security_accept()) {
                        (void)hid_host_accept_connection(
                            hid_subevent_incoming_connection_get_hid_cid(packet),
                            HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT
                        );
                    } else {
                        bd_addr_t incoming_addr = {0};
                        link_key_t link_key = {0};
                        link_key_type_t link_key_type = INVALID_LINK_KEY;

                        hid_subevent_incoming_connection_get_address(packet, incoming_addr);
                        if (gap_get_link_key_for_bd_addr(incoming_addr, link_key, &link_key_type)) {
                            /*
                             * Allow previously bonded keyboards to reconnect
                             * even when app-level reconnect backoff is active.
                             */
                            (void)hid_host_accept_connection(
                                hid_subevent_incoming_connection_get_hid_cid(packet),
                                HID_PROTOCOL_MODE_REPORT_WITH_FALLBACK_TO_BOOT
                            );
                        } else {
                            (void)hid_host_decline_connection(
                                hid_subevent_incoming_connection_get_hid_cid(packet)
                            );
                        }
                    }
                    break;
                case HID_SUBEVENT_CONNECTION_OPENED: {
                    const uint8_t status = hid_subevent_connection_opened_get_status(packet);

                    if (status == ERROR_CODE_SUCCESS) {
                        pico_w_stack_handle_connect_success();
                        pico_w_stack_emit_classic_open_event(packet);
                        (void)hid_host_send_get_protocol(
                            hid_subevent_connection_opened_get_hid_cid(packet)
                        );
                    } else {
                        pico_w_stack_handle_connect_failure(status);
                    }
                } break;
                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE:
                    pico_w_stack_emit_classic_descriptor_event(packet);
                    break;
                case HID_SUBEVENT_GET_PROTOCOL_RESPONSE:
                    if (hid_subevent_get_protocol_response_get_handshake_status(packet)
                        == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        pico_w_stack_emit_bt_protocol_event(
                            hid_subevent_get_protocol_response_get_hid_cid(packet),
                            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                            hid_subevent_get_protocol_response_get_protocol_mode(packet)
                        );
                    }
                    break;
                case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
                    if (hid_subevent_set_protocol_response_get_handshake_status(packet)
                        == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        pico_w_stack_emit_bt_protocol_event(
                            hid_subevent_set_protocol_response_get_hid_cid(packet),
                            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                            hid_subevent_set_protocol_response_get_protocol_mode(packet)
                        );
                    }
                    break;
                case HID_SUBEVENT_CONNECTION_CLOSED:
                    pico_w_stack_emit_classic_close_event(packet);
                    pico_w_stack_try_start_discovery();
                    break;
                case HID_SUBEVENT_REPORT:
                    pico_w_stack_emit_classic_report_event(packet);
                    break;
                default:
                    break;
            }
            break;
        default:
            pico_w_stack_handle_sm_event(packet);
            break;
    }
}
#endif

bool pico_w_stack_init(bool radio_ready) {
    g_usb_interface_count = 1U;
    g_usb_descriptor_generation = 1U;
    (void)memset(g_usb_interface_plan, 0, sizeof(g_usb_interface_plan));

    (void)memset(g_event_queue, 0, sizeof(g_event_queue));
    g_event_queue_head = 0U;
    g_event_queue_tail = 0U;
    g_event_queue_count = 0U;
    g_event_queue_high_watermark = 0U;
    g_event_queue_dropped = 0U;
    g_stack_last_connect_status = 0U;

    (void)pico_w_tinyusb_runtime_init();

#ifdef APP_PICO_HAS_BTSTACK
    async_context_t * context = NULL;
    const hal_flash_bank_t * flash_bank = NULL;
    const btstack_tlv_t * tlv_impl = NULL;

    g_btstack_available = false;
    g_btstack_hci_ready = false;
    g_btstack_resolving_list_requested = false;
    g_btstack_pairing_active = false;
    g_btstack_pairing_attempt_consumed = false;
    g_btstack_pairing_auth_attempted = false;
    g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    g_btstack_inquiry_active = false;
    g_btstack_scan_active = false;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_reconnect_pending = false;
    g_btstack_le_security_pending = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_now_ms = 0U;
    g_btstack_connect_pending_since_ms = 0U;
    g_btstack_le_security_pending_since_ms = 0U;
    g_btstack_le_hids_pending_since_ms = 0U;
    pico_w_stack_clear_reconnect_state();
    (void)memset(g_btstack_candidate_addr, 0, sizeof(g_btstack_candidate_addr));
    g_btstack_candidate_addr_type = BD_ADDR_TYPE_UNKNOWN;
    pico_w_stack_clear_pairing_failure_phase();
    pico_w_stack_reset_le_session();
    pico_w_stack_reset_classic_session();

    if (!radio_ready) {
        return true;
    }

    context = cyw43_arch_async_context();
    flash_bank = pico_flash_bank_instance();

    if ((context == NULL) || (flash_bank == NULL) || !btstack_cyw43_init(context)) {
        return true;
    }

    l2cap_init();
    sm_init();
    gatt_client_init();
    tlv_impl =
        btstack_tlv_flash_bank_init_instance(&g_btstack_tlv_flash_bank_context, flash_bank, NULL);
    btstack_tlv_set_instance(tlv_impl, &g_btstack_tlv_flash_bank_context);
    hci_set_link_key_db(
        btstack_link_key_db_tlv_get_instance(tlv_impl, &g_btstack_tlv_flash_bank_context)
    );
    le_device_db_tlv_configure(tlv_impl, &g_btstack_tlv_flash_bank_context);
    gap_set_bondable_mode(1);
    (void)gap_set_security_mode(GAP_SECURITY_MODE_4);
    gap_set_security_level(LEVEL_2);
    gap_ssp_set_enable(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    gap_ssp_set_authentication_requirement(
        SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING
    );
    gap_ssp_set_auto_accept(0);
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements(SM_AUTHREQ_BONDING);
    gap_set_scan_parameters(
        PICO_W_STACK_LE_SCAN_TYPE_ACTIVE,
        PICO_W_STACK_LE_SCAN_INTERVAL,
        PICO_W_STACK_LE_SCAN_WINDOW
    );
    gap_set_connection_parameters(
        PICO_W_STACK_LE_CONN_SCAN_INTERVAL,
        PICO_W_STACK_LE_CONN_SCAN_WINDOW,
        PICO_W_STACK_LE_CONN_INTERVAL_MIN,
        PICO_W_STACK_LE_CONN_INTERVAL_MAX,
        PICO_W_STACK_LE_CONN_LATENCY,
        PICO_W_STACK_LE_CONN_SUPERVISION_TIMEOUT,
        PICO_W_STACK_LE_CONN_CE_LENGTH_MIN,
        PICO_W_STACK_LE_CONN_CE_LENGTH_MAX
    );
    hids_client_init(g_btstack_hid_descriptor_storage, sizeof(g_btstack_hid_descriptor_storage));
    hid_host_init(g_btstack_hid_descriptor_storage, sizeof(g_btstack_hid_descriptor_storage));
    hid_host_register_packet_handler(&pico_w_btstack_packet_handler);
    g_btstack_hci_event_callback_registration.callback = &pico_w_btstack_packet_handler;
    hci_add_event_handler(&g_btstack_hci_event_callback_registration);
    g_btstack_sm_event_callback_registration.callback = &pico_w_btstack_packet_handler;
    sm_add_event_handler(&g_btstack_sm_event_callback_registration);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    (void)hci_power_control(HCI_POWER_ON);
    g_btstack_available = true;
#else
    (void)radio_ready;
#endif

    return true;
}

void pico_w_stack_poll(uint32_t now_ms) {
    (void)now_ms;

#ifdef APP_PICO_HAS_BTSTACK
    g_btstack_now_ms = now_ms;
    pico_w_stack_sync_hci_ready();

    /*
     * Reconnect attempts can legitimately start at uptime 0. Treat 0ms start
     * timestamps as valid so timeout recovery still triggers.
     */
    if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
        && g_btstack_connect_pending
        && ((int32_t)(now_ms - g_btstack_connect_pending_since_ms)
            >= (int32_t)pico_w_stack_reconnect_connect_timeout_ms())) {
        const uint8_t timeout_status = (g_stack_last_connect_status != ERROR_CODE_SUCCESS)
            ? g_stack_last_connect_status
            : ERROR_CODE_CONNECTION_TIMEOUT;
        pico_w_stack_handle_connect_failure(timeout_status);
    }

    if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
        && (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID)
        && g_btstack_le_security_pending
        && (g_btstack_le_security_pending_since_ms != 0U)
        && ((int32_t)(now_ms - g_btstack_le_security_pending_since_ms)
            >= (int32_t)PICO_W_STACK_RECONNECT_SECURITY_PENDING_TIMEOUT_MS)) {
        (void)gap_disconnect(g_btstack_le_session.con_handle);
        pico_w_stack_handle_connect_failure(ERROR_CODE_CONNECTION_TIMEOUT);
    }

    if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
        && (g_btstack_le_session.con_handle != HCI_CON_HANDLE_INVALID)
        && !g_btstack_le_session.connected
        && (g_btstack_le_hids_pending_since_ms != 0U)
        && ((int32_t)(now_ms - g_btstack_le_hids_pending_since_ms)
            >= (int32_t)PICO_W_STACK_RECONNECT_HIDS_PENDING_TIMEOUT_MS)) {
        pico_w_stack_handle_connect_failure(ERROR_CODE_COMMAND_DISALLOWED);
    }

    if (g_btstack_available) {
        pico_w_stack_try_start_discovery();
        pico_w_stack_try_connect_candidate();
        pico_w_stack_try_start_le_hids_client();
    }
#endif

    pico_w_tinyusb_runtime_poll();
}

void pico_w_stack_set_usb_plan(
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan
) {
    hid_transport_usb_interface_plan_t next_plan[PICO_W_STACK_MAX_USB_INTERFACE] = {0};
    size_t copy_len = 0U;
    bool descriptor_changed = false;
    bool plan_changed = false;

    if (interface_count > PICO_W_STACK_MAX_USB_INTERFACE) {
        interface_count = PICO_W_STACK_MAX_USB_INTERFACE;
    }

    copy_len = (size_t)interface_count * sizeof(next_plan[0]);
    if ((interface_plan != NULL) && (copy_len > 0U)) {
        (void)memcpy(next_plan, interface_plan, copy_len);
    }

    descriptor_changed = descriptor_generation != g_usb_descriptor_generation;
    plan_changed = (interface_count != g_usb_interface_count)
        || (memcmp(next_plan, g_usb_interface_plan, sizeof(next_plan)) != 0);

    if (!descriptor_changed && !plan_changed) {
        return;
    }

    g_usb_interface_count = interface_count;
    (void)memcpy(g_usb_interface_plan, next_plan, sizeof(g_usb_interface_plan));

    if (descriptor_generation == g_usb_descriptor_generation) {
        return;
    }
    g_usb_descriptor_generation = descriptor_generation;
    pico_w_tinyusb_runtime_request_reenumeration();
}

void pico_w_stack_set_pairing(
    bool pairing_active,
    uint8_t bt_link_type
) {
#ifdef APP_PICO_HAS_BTSTACK
    bool pairing_mode_changed = false;

    if (!g_btstack_available) {
        return;
    }

    if (!pairing_active) {
        bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    } else if (!pico_w_stack_valid_pairing_link_type(bt_link_type)) {
        bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    }

    if ((g_btstack_pairing_active == pairing_active)
        && (g_btstack_pairing_link_type == bt_link_type)) {
        return;
    }

    pairing_mode_changed =
        g_btstack_pairing_active && pairing_active && (g_btstack_pairing_link_type != bt_link_type);
    g_btstack_pairing_active = pairing_active;
    g_btstack_pairing_link_type = bt_link_type;

    if (g_btstack_connect_pending
        && ((g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE)
            || (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST))
        && (g_btstack_le_session.con_handle == HCI_CON_HANDLE_INVALID)) {
        (void)gap_connect_cancel();
    }

    if (!g_btstack_pairing_active) {
        g_btstack_pairing_attempt_consumed = false;
        g_btstack_pairing_auth_attempted = false;
        g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        pico_w_stack_clear_pairing_failure_phase();
        pico_w_stack_clear_reconnect_state();
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
        g_btstack_le_security_pending = false;
        g_btstack_le_hids_connect_pending = false;
        g_btstack_connect_pending_since_ms = 0U;
        g_btstack_le_security_pending_since_ms = 0U;
        g_btstack_le_hids_pending_since_ms = 0U;
        pico_w_stack_abandon_unconnected_le_session();
        pico_w_stack_stop_discovery();
        return;
    }

    if (pairing_mode_changed) {
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
        g_btstack_connect_pending_since_ms = 0U;
        g_btstack_le_security_pending = false;
        g_btstack_le_security_pending_since_ms = 0U;
        g_btstack_le_hids_connect_pending = false;
        g_btstack_le_hids_pending_since_ms = 0U;
        pico_w_stack_abandon_unconnected_le_session();
        pico_w_stack_stop_discovery();
    }

    g_btstack_pairing_attempt_consumed = false;
    g_btstack_pairing_auth_attempted = false;
    pico_w_stack_clear_pairing_failure_phase();
    pico_w_stack_try_start_discovery();
#else
    (void)pairing_active;
    (void)bt_link_type;
#endif
}

bool pico_w_stack_request_reconnect(
    const pair_device_id_t * device_id,
    uint8_t bt_link_type_hint,
    uint8_t bt_addr_type_hint
) {
#ifdef APP_PICO_HAS_BTSTACK
    bd_addr_type_t preferred_addr_type = BD_ADDR_TYPE_UNKNOWN;

    if (!pico_w_stack_device_id_valid(device_id)) {
        if (device_id != NULL) {
            pico_w_stack_emit_reconnect_result(
                device_id,
                HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
                1U
            );
        }

        return false;
    }

    if (!g_btstack_available || !g_btstack_hci_ready) {
        pico_w_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            2U
        );
        return false;
    }

    if (g_btstack_pairing_active) {
        pico_w_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            3U
        );
        return false;
    }

    /*
     * App-level reconnect retries can overlap with an already pending stack reconnect.
     * Coalesce duplicate requests for the same device so we don't self-trigger a
     * 1s STACK_REJECTED loop while the original connect attempt is still in flight.
     */
    if (g_btstack_connect_pending || g_btstack_inquiry_active || g_btstack_scan_active) {
        if (g_btstack_connect_pending
            && g_btstack_reconnect_pending
            && (memcmp(g_btstack_candidate_addr, device_id->bytes, sizeof(device_id->bytes))
                == 0)) {
            return true;
        }

        pico_w_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            4U
        );
        return false;
    }

    preferred_addr_type = pico_w_stack_map_bt_addr_type_to_stack(bt_addr_type_hint);

    g_stack_last_connect_status = ERROR_CODE_SUCCESS;
    pico_w_stack_clear_reconnect_state();
    (void)memcpy(g_btstack_candidate_addr, device_id->bytes, sizeof(g_btstack_candidate_addr));

    if (bt_link_type_hint == HID_TRANSPORT_BT_LINK_TYPE_LE) {
        /*
         * Prefer last-known LE path first. If that address is stale after a
         * long offline window, try LE whitelist immediately before spending
         * time on lower-probability fallbacks.
         */
        if (preferred_addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY) {
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
        } else if (preferred_addr_type == BD_ADDR_TYPE_LE_RANDOM) {
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_RANDOM
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC
            );
        } else if (preferred_addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY) {
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
        } else {
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_PUBLIC
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)pico_w_stack_reconnect_add_attempt(
                PICO_W_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM
            );
        }
    } else {
        (void)
            pico_w_stack_reconnect_add_attempt(PICO_W_STACK_CONNECT_MODE_CLASSIC, BD_ADDR_TYPE_ACL);
        (void)pico_w_stack_reconnect_add_attempt(
            PICO_W_STACK_CONNECT_MODE_LE_WHITELIST,
            BD_ADDR_TYPE_UNKNOWN
        );
        (void)pico_w_stack_reconnect_add_attempt(
            PICO_W_STACK_CONNECT_MODE_LE,
            BD_ADDR_TYPE_LE_PUBLIC
        );
        (void)pico_w_stack_reconnect_add_attempt(
            PICO_W_STACK_CONNECT_MODE_LE,
            BD_ADDR_TYPE_LE_RANDOM
        );
    }

    g_btstack_reconnect_pending = true;
    if (!pico_w_stack_reconnect_apply_attempt(0U)) {
        pico_w_stack_clear_reconnect_state();
        pico_w_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            5U
        );
        return false;
    }

    pico_w_stack_try_connect_candidate();
    return true;
#else
    (void)device_id;
    (void)bt_link_type_hint;
    (void)bt_addr_type_hint;
    return false;
#endif
}

bool pico_w_stack_forget_device(const pair_device_id_t * device_id) {
#ifdef APP_PICO_HAS_BTSTACK
    bd_addr_t device_addr = {0};
    uint16_t hid_cid = 0U;
    uint8_t bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    int entry_index = 0;

    if (!g_btstack_available || !pico_w_stack_device_id_valid(device_id) || !g_btstack_hci_ready) {
        return false;
    }

    pico_w_stack_copy_addr_from_device_id(device_addr, device_id);

    if (pico_w_stack_find_hid_cid_for_device(device_id, &hid_cid, &bt_link_type)) {
        if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) {
            hid_host_disconnect(hid_cid);
        } else if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE) {
            (void)hids_client_disconnect(hid_cid);
        }
    }

    gap_drop_link_key_for_bd_addr(device_addr);
    gap_delete_bonding(BD_ADDR_TYPE_ACL, device_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC, device_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM, device_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_PUBLIC_IDENTITY, device_addr);
    gap_delete_bonding(BD_ADDR_TYPE_LE_RANDOM_IDENTITY, device_addr);

    for (entry_index = 0; entry_index < le_device_db_max_count(); entry_index++) {
        int entry_addr_type = BD_ADDR_TYPE_UNKNOWN;
        bd_addr_t entry_addr = {0};
        sm_key_t entry_irk = {0};

        le_device_db_info(entry_index, &entry_addr_type, entry_addr, entry_irk);
        (void)entry_addr_type;

        if (memcmp(entry_addr, device_addr, sizeof(entry_addr)) != 0) {
            continue;
        }

        le_device_db_remove(entry_index);
    }

    return true;
#else
    (void)device_id;
    return false;
#endif
}

uint8_t pico_w_stack_usb_interface_count(void) {
    return g_usb_interface_count;
}

const uint8_t * pico_w_stack_usb_report_descriptor(
    uint8_t interface_number,
    uint16_t * out_len
) {
    uint16_t fallback_len = 0U;

    if (out_len != NULL) {
        *out_len = 0U;
    }

    if (interface_number >= g_usb_interface_count) {
        return NULL;
    }

    fallback_len = g_usb_interface_plan[interface_number].report_descriptor_len;

#ifdef APP_PICO_HAS_BTSTACK
    {
        const uint16_t hid_cid = g_usb_interface_plan[interface_number].hid_cid;
        const uint8_t bt_link_type = g_usb_interface_plan[interface_number].bt_link_type;

        if (hid_cid != 0U) {
            const uint8_t * descriptor = NULL;
            uint16_t descriptor_len = 0U;

            if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) {
                descriptor = hid_descriptor_storage_get_descriptor_data(hid_cid);
                descriptor_len = hid_descriptor_storage_get_descriptor_len(hid_cid);
            } else if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE) {
                descriptor = hids_client_descriptor_storage_get_descriptor_data(
                    hid_cid,
                    PICO_W_STACK_LE_HIDS_SERVICE_INDEX
                );
                descriptor_len = hids_client_descriptor_storage_get_descriptor_len(
                    hid_cid,
                    PICO_W_STACK_LE_HIDS_SERVICE_INDEX
                );
            }

            if ((descriptor != NULL) && (descriptor_len > 0U)) {
                if (out_len != NULL) {
                    *out_len = descriptor_len;
                }

                return descriptor;
            }
        }
    }
#endif

    if (out_len != NULL) {
        *out_len = fallback_len;
    }

    return NULL;
}

uint16_t pico_w_stack_usb_report_descriptor_len(uint8_t interface_number) {
    uint16_t descriptor_len = 0U;

    (void)pico_w_stack_usb_report_descriptor(interface_number, &descriptor_len);
    return descriptor_len;
}

uint8_t pico_w_stack_usb_protocol_mode(uint8_t interface_number) {
    if (interface_number >= g_usb_interface_count) {
        return HID_TRANSPORT_PROTOCOL_UNKNOWN;
    }

    return g_usb_interface_plan[interface_number].protocol_mode;
}

static uint8_t pico_w_stack_report_remap_profile(uint8_t interface_number) {
    const uint8_t * descriptor = NULL;
    uint16_t descriptor_len = 0U;
    const uint8_t protocol_mode = pico_w_stack_usb_protocol_mode(interface_number);
    hid_report_policy_decision_t decision = {0};

    descriptor = pico_w_stack_usb_report_descriptor(interface_number, &descriptor_len);
    hid_report_policy_decide(descriptor, descriptor_len, protocol_mode, &decision);
    return hid_report_remap_profile_from_policy(&decision);
}

bool pico_w_stack_take_event(hid_transport_event_t * out_event) {
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

void pico_w_stack_ingest_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    hid_transport_event_t event = {0};
    uint8_t remapped_report[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t remapped_report_len = 0U;
    const uint8_t remap_profile = pico_w_stack_report_remap_profile(interface_number);
    const uint8_t protocol_mode = pico_w_stack_usb_protocol_mode(interface_number);

    if (!hid_report_remap_usb_to_bt(
            remap_profile,
            protocol_mode,
            report,
            report_len,
            remapped_report,
            &remapped_report_len
        )) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_USB_HID_REPORT;
    event.interface_number = interface_number;
    event.report_len = remapped_report_len;

    if (remapped_report_len > 0U) {
        (void)memcpy(event.report, remapped_report, remapped_report_len);
    }

    (void)pico_w_stack_push_event(&event);
}

bool pico_w_stack_send_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    uint8_t remapped_report[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t remapped_report_len = 0U;
    const uint8_t remap_profile = pico_w_stack_report_remap_profile(interface_number);

    if (!hid_report_remap_bt_to_usb(
            remap_profile,
            report,
            report_len,
            remapped_report,
            &remapped_report_len
        )) {
        return false;
    }

    return pico_w_tinyusb_runtime_send_in_report(
        interface_number,
        remapped_report,
        remapped_report_len
    );
}

bool pico_w_stack_send_bt_report(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len
) {
#ifdef APP_PICO_HAS_BTSTACK
    uint8_t report_id = 0U;
    const uint8_t * payload = report;
    uint8_t payload_len = 0U;

    if (!g_btstack_available || !g_btstack_hci_ready) {
        return false;
    }

    if ((hid_cid == 0U)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)
        || (report_len == 0U)
        || (report == NULL)
        || (report_len > UINT8_MAX)) {
        return false;
    }

    payload_len = (uint8_t)report_len;

    if ((protocol_mode == HID_TRANSPORT_PROTOCOL_REPORT) && (report_len > 0U)) {
        report_id = report[0];
        payload = &report[1];
        payload_len = (uint8_t)(report_len - 1U);
    }

    if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) {
        if ((protocol_mode == HID_TRANSPORT_PROTOCOL_REPORT)
            || (protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT)) {
            return hid_host_send_report(hid_cid, report_id, payload, payload_len)
                == ERROR_CODE_SUCCESS;
        }

        return hid_host_send_set_report(
                   hid_cid,
                   HID_REPORT_TYPE_OUTPUT,
                   0U,
                   report,
                   (uint8_t)report_len
               )
            == ERROR_CODE_SUCCESS;
    }

    if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE) {
        if (protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT) {
            report_id = HID_BOOT_MODE_KEYBOARD_ID;
            payload = report;
            payload_len = (uint8_t)report_len;
        }

        return hids_client_send_write_report(
                   hid_cid,
                   report_id,
                   HID_REPORT_TYPE_OUTPUT,
                   payload,
                   payload_len
               )
            == ERROR_CODE_SUCCESS;
    }

    return false;
#else
    (void)hid_cid;
    (void)bt_link_type;
    (void)protocol_mode;
    (void)report;
    (void)report_len;
    return false;
#endif
}

bool pico_w_stack_runtime_state_get(pico_w_stack_runtime_state_t * out_state) {
    if (out_state == NULL) {
        return false;
    }

    out_state->event_queue_depth = g_event_queue_count;
    out_state->event_queue_high_watermark = g_event_queue_high_watermark;
    out_state->event_queue_dropped = g_event_queue_dropped;
#ifdef APP_PICO_HAS_BTSTACK
    out_state->connect_pending = g_btstack_connect_pending ? 1U : 0U;
    out_state->reconnect_pending = g_btstack_reconnect_pending ? 1U : 0U;
    out_state->connect_mode = pico_w_stack_diag_connect_mode(g_btstack_connect_mode);
    out_state->reconnect_attempt_index = g_btstack_reconnect_attempt_index;
    out_state->reconnect_attempt_count = g_btstack_reconnect_attempt_count;
    out_state->last_connect_status = g_stack_last_connect_status;
#else
    out_state->connect_pending = 0U;
    out_state->reconnect_pending = 0U;
    out_state->connect_mode = 0U;
    out_state->reconnect_attempt_index = 0U;
    out_state->reconnect_attempt_count = 0U;
    out_state->last_connect_status = 0U;
#endif
    return true;
}
