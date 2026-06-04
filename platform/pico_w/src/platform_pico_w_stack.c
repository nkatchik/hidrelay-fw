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

#include "hid_transport_runtime.h"
#include "platform_pico_w_tinyusb_runtime.h"

enum {
    PICO_W_STACK_MAX_USB_INTERFACE = HID_TRANSPORT_MAX_INTERFACE,
    PICO_W_STACK_MAX_ACTIVE_CONNECTION = 2U,
    PICO_W_STACK_INQUIRY_DURATION_UNITS = 0x08U,
    PICO_W_STACK_MAJOR_CLASS_PERIPHERAL = 0x05U,
};

static hid_transport_runtime_t g_transport_runtime = {0};
static uint8_t g_stack_last_connect_status = 0U;

static bool pico_w_stack_push_event(const hid_transport_event_t * event) {
    return hid_transport_runtime_push_event(&g_transport_runtime, event);
}

static bool pico_w_stack_find_hid_cid_for_device(
    const pair_device_id_t * device_id,
    uint16_t * out_hid_cid,
    uint8_t * out_bt_link_type
) {
    return hid_transport_runtime_find_hid_cid_for_device(
        &g_transport_runtime,
        device_id,
        out_hid_cid,
        out_bt_link_type
    );
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
    /*
     * Apple Magic Keyboard (and other peers that advertise
     * SSP_IO_AUTHREQ_..._DEDICATED_BONDING in their IO Capability response)
     * gate HID on first completing a dedicated bond. A cold hid_host_connect
     * runs its SDP query before the link is encrypted, which this peripheral
     * ignores -- it stays silent until our connect-pending timer fires.
     *
     * CLASSIC_DEDICATED_BONDING runs gap_dedicated_bonding() to page the peer
     * and complete SSP / link-key generation. ENABLE_EXPLICIT_DEDICATED_-
     * BONDING_DISCONNECT keeps that ACL up; on a successful
     * GAP_EVENT_DEDICATED_BONDING_COMPLETED we re-schedule the same candidate
     * as plain CLASSIC, whose hid_host_connect then runs SDP + HID L2CAP over
     * the already-bonded, encrypted ACL.
     */
    PICO_W_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING,
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
        case PICO_W_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING:
            return 3U;
        case PICO_W_STACK_CONNECT_MODE_NONE:
        default:
            return 0U;
    }
}

typedef struct {
    bool connected;
    bool security_pending;
    bool hids_connect_pending;
    hci_con_handle_t con_handle;
    uint16_t hids_cid;
    uint32_t security_pending_since_ms;
    uint32_t hids_pending_since_ms;
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

static uint8_t g_btstack_classic_hid_descriptor_storage[1024] = {0};
static uint8_t g_btstack_le_hid_descriptor_storage[1024] = {0};
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
static uint32_t g_btstack_now_ms = 0U;
static uint32_t g_btstack_connect_pending_since_ms = 0U;
static bd_addr_t g_btstack_candidate_addr = {0};
static bd_addr_type_t g_btstack_candidate_addr_type = BD_ADDR_TYPE_UNKNOWN;
/*
 * Classic ACL handle for the in-flight pairing/connect candidate. The Classic
 * flow bonds via gap_dedicated_bonding and -- through ENABLE_EXPLICIT_DEDICATED_-
 * BONDING_DISCONNECT -- holds that ACL open so the follow-up hid_host_connect
 * (SDP + HID L2CAP) reuses it. If that connect attempt fails or times out, the
 * ACL and any half-open hid_host session must be torn down explicitly: BTstack's
 * hid_host_disconnect is a no-op while a connection is still in its SDP-query
 * phase (no L2CAP channels yet), and a lingering connection makes the next
 * hid_host_connect return COMMAND_DISALLOWED -- which is exactly the cascading
 * SDP stall seen on repeat attempts. Dropping the ACL errors the pending SDP
 * client query, letting BTstack finalize the hid_host connection. Captured from
 * HCI_EVENT_CONNECTION_COMPLETE; invalid when no Classic ACL is in flight.
 */
static hci_con_handle_t g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
static pico_w_stack_connect_mode_t g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
static pico_w_stack_pairing_failure_phase_t g_btstack_pairing_failure_phase =
    PICO_W_STACK_PAIRING_FAILURE_PHASE_NONE;
static pico_w_stack_reconnect_attempt_t
    g_btstack_reconnect_attempt[PICO_W_STACK_RECONNECT_MAX_ATTEMPT] = {0};
static uint8_t g_btstack_reconnect_attempt_count = 0U;
static uint8_t g_btstack_reconnect_attempt_index = 0U;
static pico_w_stack_le_session_t g_btstack_le_session[PICO_W_STACK_MAX_ACTIVE_CONNECTION] = {0};
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

static void pico_w_stack_reset_le_session(pico_w_stack_le_session_t * session) {
    if (session == NULL) {
        return;
    }

    session->connected = false;
    session->security_pending = false;
    session->hids_connect_pending = false;
    session->con_handle = HCI_CON_HANDLE_INVALID;
    session->hids_cid = 0U;
    session->security_pending_since_ms = 0U;
    session->hids_pending_since_ms = 0U;
    (void)memset(session->addr, 0, sizeof(session->addr));
    session->addr_type = BD_ADDR_TYPE_UNKNOWN;
}

static void pico_w_stack_reset_le_sessions(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        pico_w_stack_reset_le_session(&g_btstack_le_session[index]);
    }
}

static pico_w_stack_le_session_t * pico_w_stack_find_le_session_by_con_handle(
    hci_con_handle_t con_handle
) {
    uint8_t index = 0U;

    if (con_handle == HCI_CON_HANDLE_INVALID) {
        return NULL;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == con_handle) {
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static pico_w_stack_le_session_t * pico_w_stack_find_le_session_by_hids_cid(uint16_t hids_cid) {
    uint8_t index = 0U;

    if (hids_cid == 0U) {
        return NULL;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].hids_cid == hids_cid) {
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static pico_w_stack_le_session_t * pico_w_stack_allocate_le_session(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == HCI_CON_HANDLE_INVALID) {
            pico_w_stack_reset_le_session(&g_btstack_le_session[index]);
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static bool pico_w_stack_le_session_capacity_available(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == HCI_CON_HANDLE_INVALID) {
            return true;
        }
    }

    return false;
}

static bool pico_w_stack_has_unconnected_le_session(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if ((g_btstack_le_session[index].con_handle != HCI_CON_HANDLE_INVALID)
            && !g_btstack_le_session[index].connected) {
            return true;
        }
    }

    return false;
}

static void pico_w_stack_clear_unconnected_le_pending(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        pico_w_stack_le_session_t * session = &g_btstack_le_session[index];

        if (session->connected) {
            continue;
        }

        session->security_pending = false;
        session->hids_connect_pending = false;
        session->security_pending_since_ms = 0U;
        session->hids_pending_since_ms = 0U;
    }
}

static void pico_w_stack_abandon_le_connect_attempt(pico_w_stack_le_session_t * session) {
    if ((session == NULL) || session->connected) {
        return;
    }

    if (session->hids_cid != 0U) {
        (void)hids_client_disconnect(session->hids_cid);
    }

    if (session->con_handle != HCI_CON_HANDLE_INVALID) {
        (void)gap_disconnect(session->con_handle);
    }

    pico_w_stack_reset_le_session(session);
}

static void pico_w_stack_abandon_unconnected_le_sessions(void) {
    uint8_t index = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        pico_w_stack_le_session_t * session = &g_btstack_le_session[index];

        if (session->connected) {
            continue;
        }

        pico_w_stack_abandon_le_connect_attempt(session);
    }
}

static void pico_w_stack_update_le_identity(
    hci_con_handle_t con_handle,
    bd_addr_type_t identity_addr_type,
    const bd_addr_t identity_addr
) {
    pico_w_stack_le_session_t * session = NULL;

    if ((con_handle == HCI_CON_HANDLE_INVALID) || (identity_addr == NULL)) {
        return;
    }

    session = pico_w_stack_find_le_session_by_con_handle(con_handle);
    if (session == NULL) {
        return;
    }

    (void)memcpy(session->addr, identity_addr, sizeof(session->addr));
    session->addr_type = identity_addr_type;
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

static uint8_t pico_w_stack_active_connection_count(void) {
    uint8_t index = 0U;
    uint8_t count = 0U;

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle != HCI_CON_HANDLE_INVALID) {
            count = (uint8_t)(count + 1U);
        }
    }

    for (index = 0U; index < PICO_W_STACK_MAX_USB_INTERFACE; index++) {
        if (g_btstack_classic_session[index].used) {
            count = (uint8_t)(count + 1U);
        }
    }

    return count;
}

static bool pico_w_stack_connection_capacity_available(void) {
    return pico_w_stack_active_connection_count() < PICO_W_STACK_MAX_ACTIVE_CONNECTION;
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

static void pico_w_stack_request_le_low_latency_params(pico_w_stack_le_session_t * session) {
    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        return;
    }

    /*
     * Best-effort latency reduction: peripherals may reject or ignore this
     * request, in which case the existing negotiated parameters remain active.
     */
    (void)gap_request_connection_parameter_update(
        session->con_handle,
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
        || !pico_w_stack_connection_capacity_available()
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
        || !pico_w_stack_connection_capacity_available()
        || !pico_w_stack_le_session_capacity_available()
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
        || !pico_w_stack_connection_capacity_available()
        || (((mode == PICO_W_STACK_CONNECT_MODE_LE)
                || (mode == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST))
            && !pico_w_stack_le_session_capacity_available())
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
    pico_w_stack_clear_unconnected_le_pending();
    if (!reconnect_pending) {
        pico_w_stack_clear_reconnect_state();
        if (g_btstack_pairing_active) {
            g_btstack_pairing_attempt_consumed = true;
            g_btstack_pairing_auth_attempted = false;
            if ((mode == PICO_W_STACK_CONNECT_MODE_CLASSIC)
                || (mode == PICO_W_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING)) {
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
    pico_w_stack_clear_unconnected_le_pending();
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

static void pico_w_stack_request_le_pairing_before_hids(pico_w_stack_le_session_t * session) {
    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        pico_w_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    pico_w_stack_mark_pairing_auth_attempted();
    session->security_pending = true;
    session->security_pending_since_ms = g_btstack_now_ms;
    session->hids_connect_pending = false;
    session->hids_pending_since_ms = 0U;
    sm_request_pairing(session->con_handle);
}

static void pico_w_stack_handle_connect_failure(uint8_t status_code) {
    const bool auth_attempted =
        g_btstack_reconnect_auth_attempted || g_btstack_pairing_auth_attempted;

    if ((g_btstack_connect_mode
            == PICO_W_STACK_CONNECT_MODE_LE
            || g_btstack_connect_mode
            == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST)
        && !pico_w_stack_has_unconnected_le_session()) {
        (void)gap_connect_cancel();
    }

    /*
     * Drop the Classic candidate ACL so a failed/timed-out attempt does not
     * leave a held ACL and a half-open hid_host SDP session behind -- those
     * make the next hid_host_connect return COMMAND_DISALLOWED and cascade
     * into repeated SDP stalls. The disconnect errors the pending SDP query,
     * which lets BTstack finalize the hid_host connection.
     */
    if (g_btstack_classic_candidate_con_handle != HCI_CON_HANDLE_INVALID) {
        (void)gap_disconnect(g_btstack_classic_candidate_con_handle);
        g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    }

    g_stack_last_connect_status = status_code;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    pico_w_stack_clear_unconnected_le_pending();
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
    /*
     * The candidate ACL is now a live HID session; stop tracking it as a
     * teardown target so a later unrelated connect failure cannot disconnect
     * the working keyboard.
     */
    g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    pico_w_stack_clear_unconnected_le_pending();
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

    /*
     * BTstack delivers the raw Classic HID interrupt-channel payload, which
     * begins with the Bluetooth HID transaction header (high nibble 0xA =
     * HID_MESSAGE_TYPE_DATA; 0xA1 for a DATA/Input report). That byte is
     * Bluetooth transport framing, not part of a USB HID report -- USB reports
     * start with the report ID (or the data itself for report-ID-less
     * descriptors). Forwarding it verbatim makes the USB host read 0xA1 as the
     * report ID, find no matching report, and silently drop every keystroke
     * (the device still enumerates as a keyboard, which is why pairing looked
     * fine but nothing typed). Strip the DATA transaction header so the relayed
     * report matches the passed-through report descriptor. LE/HOGP reports have
     * no such header and are emitted separately, so this is Classic-only.
     */
    if ((report != NULL) && (report_len > 0U) && ((report[0] >> 4U) == 0x0AU)) {
        report++;
        report_len = (uint16_t)(report_len - 1U);
    }

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

    if (!pico_w_stack_connection_capacity_available()
        || (((g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE)
                || (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_LE_WHITELIST))
            && !pico_w_stack_le_session_capacity_available())) {
        pico_w_stack_handle_connect_failure(BTSTACK_MEMORY_ALLOC_FAILED);
        return;
    }

    if (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_CLASSIC) {
        /*
         * Plain REPORT mode: SDP query, then open L2CAP HID-Control and
         * HID-Interrupt back-to-back, and stop -- no SET_PROTOCOL. A BT HID
         * device defaults to Report Protocol, so SET_PROTOCOL is unnecessary,
         * and the packet trace proved Apple Magic Keyboard tears the link down
         * (L2CAP Disconnection Request -> ACL disconnect, reason 0x13) the
         * instant it receives SET_PROTOCOL on the control channel. BOOT and
         * REPORT_WITH_FALLBACK_TO_BOOT both emit that SET_PROTOCOL; REPORT does
         * not, matching how Android (which pairs this keyboard in ~1s) drives
         * the connection. By this second phase the ACL is already bonded and
         * encrypted, so SDP completes normally.
         */
        connect_status =
            hid_host_connect(g_btstack_candidate_addr, HID_PROTOCOL_MODE_REPORT, &hid_cid);
    } else if (g_btstack_connect_mode == PICO_W_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING) {
        /*
         * gap_dedicated_bonding pages the peer, drives SSP, generates and
         * stores the link key, and then tears down the ACL. Completion is
         * reported via GAP_EVENT_DEDICATED_BONDING_COMPLETED -- on success
         * we re-schedule the same candidate as plain CLASSIC so
         * hid_host_connect runs against a now-bonded peer.
         */
        connect_status = (uint8_t)gap_dedicated_bonding(g_btstack_candidate_addr, 1);
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

static void pico_w_stack_start_le_hids_client(pico_w_stack_le_session_t * session) {
    uint16_t hids_cid = 0U;
    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;

    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        if (session != NULL) {
            session->hids_connect_pending = false;
            session->hids_pending_since_ms = 0U;
        }
        pico_w_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    pico_w_stack_set_pairing_failure_phase(PICO_W_STACK_PAIRING_FAILURE_PHASE_LE_HIDS);

    status = hids_client_connect(
        session->con_handle,
        &pico_w_btstack_packet_handler,
        HID_PROTOCOL_MODE_REPORT,
        &hids_cid
    );

    if (status == ERROR_CODE_SUCCESS) {
        session->hids_cid = hids_cid;
        session->hids_connect_pending = false;
        if (session->hids_pending_since_ms == 0U) {
            session->hids_pending_since_ms = g_btstack_now_ms;
        }
        return;
    }

    if (status == ERROR_CODE_COMMAND_DISALLOWED) {
        g_stack_last_connect_status = status;
        session->hids_connect_pending = true;
        if (session->hids_pending_since_ms == 0U) {
            session->hids_pending_since_ms = g_btstack_now_ms;
        }
        return;
    }

    session->hids_connect_pending = false;
    session->hids_pending_since_ms = 0U;
    pico_w_stack_abandon_le_connect_attempt(session);
    pico_w_stack_handle_connect_failure(status);
}

static void pico_w_stack_try_start_le_hids_client(void) {
    uint8_t index = 0U;

    if (!g_btstack_hci_ready) {
        return;
    }

    for (index = 0U; index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; index++) {
        pico_w_stack_le_session_t * session = &g_btstack_le_session[index];

        if (!session->hids_connect_pending
            || session->security_pending
            || (session->con_handle == HCI_CON_HANDLE_INVALID)
            || session->connected) {
            continue;
        }

        pico_w_stack_start_le_hids_client(session);
    }
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

    /*
     * Apple Magic Keyboard (Classic HID) does not answer SDP on an unencrypted
     * link, so a cold hid_host_connect -- which runs the SDP query first --
     * stalls. Pair in two phases instead: CLASSIC_DEDICATED_BONDING pages and
     * bonds first; on success the ACL is held open (via
     * ENABLE_EXPLICIT_DEDICATED_BONDING_DISCONNECT) and the bonding-complete
     * handler runs hid_host_connect against that now-encrypted ACL, where SDP
     * and the HID L2CAP channels come up immediately.
     */
    pico_w_stack_schedule_candidate(
        addr,
        BD_ADDR_TYPE_ACL,
        PICO_W_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING,
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
    pico_w_stack_le_session_t * session = NULL;

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
            session = pico_w_stack_find_le_session_by_con_handle(
                sm_event_pairing_complete_get_handle(packet)
            );
            if (session == NULL) {
                break;
            }

            if (sm_event_pairing_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                /*
                 * Only (re)start hids discovery when *we* initiated the SM flow.
                 * With gatt_client_set_required_security_level(LEVEL_2) the
                 * gatt_client can drive pairing/reencryption from inside an
                 * in-flight hids_client_connect; a second hids_client_connect
                 * would then fail with COMMAND_DISALLOWED.
                 */
                const bool self_initiated = session->security_pending;
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                if (self_initiated) {
                    session->hids_connect_pending = true;
                    session->hids_pending_since_ms = g_btstack_now_ms;
                    pico_w_stack_start_le_hids_client(session);
                }
            } else {
                const uint8_t status = sm_event_pairing_complete_get_status(packet);
                const uint8_t reason = sm_event_pairing_complete_get_reason(packet);
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                pico_w_stack_mark_pairing_auth_attempted();
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                session->hids_connect_pending = false;
                pico_w_stack_abandon_le_connect_attempt(session);
                pico_w_stack_handle_connect_failure((reason != 0U) ? reason : status);
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            session = pico_w_stack_find_le_session_by_con_handle(
                sm_event_reencryption_complete_get_handle(packet)
            );
            if (session == NULL) {
                break;
            }

            if (sm_event_reencryption_complete_get_status(packet) == ERROR_CODE_SUCCESS) {
                const bool self_initiated = session->security_pending;
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                if (self_initiated) {
                    session->hids_connect_pending = true;
                    session->hids_pending_since_ms = g_btstack_now_ms;
                    pico_w_stack_start_le_hids_client(session);
                }
            } else {
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                pico_w_stack_mark_pairing_auth_attempted();
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                session->hids_connect_pending = false;
                pico_w_stack_abandon_le_connect_attempt(session);
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
    pico_w_stack_le_session_t * session = NULL;

    if (packet == NULL) {
        return;
    }

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            hids_cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            session = pico_w_stack_find_le_session_by_hids_cid(hids_cid);
            if (session == NULL) {
                break;
            }

            if (status != ERROR_CODE_SUCCESS) {
                if (!session->security_pending
                    && (session->con_handle != HCI_CON_HANDLE_INVALID)
                    && pico_w_stack_status_requires_pairing_retry(status)) {
                    if (g_btstack_reconnect_pending) {
                        g_btstack_reconnect_auth_attempted = true;
                    }
                    pico_w_stack_mark_pairing_auth_attempted();
                    session->security_pending = true;
                    session->security_pending_since_ms = g_btstack_now_ms;
                    session->hids_connect_pending = true;
                    session->hids_pending_since_ms = g_btstack_now_ms;
                    sm_request_pairing(session->con_handle);
                    break;
                }

                pico_w_stack_abandon_le_connect_attempt(session);
                pico_w_stack_handle_connect_failure(status);
                break;
            }

            session->hids_connect_pending = false;
            session->connected = true;
            session->hids_cid = hids_cid;
            session->hids_pending_since_ms = 0U;
            pico_w_stack_handle_connect_success();
            pico_w_stack_request_le_low_latency_params(session);
            pico_w_stack_emit_le_open_event(hids_cid, session->addr, session->addr_type);
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
    pico_w_stack_le_session_t * session = NULL;

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
        pico_w_stack_handle_connect_failure(status);
        return;
    }

    session = pico_w_stack_allocate_le_session();
    if (session == NULL) {
        (void)gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(packet));
        pico_w_stack_handle_connect_failure(BTSTACK_MEMORY_ALLOC_FAILED);
        return;
    }

    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    session->connected = false;
    session->hids_cid = 0U;
    session->con_handle = gap_subevent_le_connection_complete_get_connection_handle(packet);
    session->addr_type =
        (bd_addr_type_t)gap_subevent_le_connection_complete_get_peer_address_type(packet);
    gap_subevent_le_connection_complete_get_peer_address(packet, session->addr);
    if (g_btstack_reconnect_pending
        && (memcmp(g_btstack_candidate_addr, zero_addr, sizeof(g_btstack_candidate_addr)) != 0)) {
        /*
         * During reconnect, keep app-facing device identity stable to the
         * paired candidate address. Some keyboards reconnect with a rotating
         * private address even when bonded, which can otherwise prevent
         * app-side device-id matching/reconnect bookkeeping.
         */
        (void)memcpy(session->addr, g_btstack_candidate_addr, sizeof(bd_addr_t));
        if (g_btstack_candidate_addr_type != BD_ADDR_TYPE_UNKNOWN) {
            session->addr_type = g_btstack_candidate_addr_type;
        }
    }
    pico_w_stack_request_le_low_latency_params(session);
    session->security_pending = false;
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        pico_w_stack_request_le_pairing_before_hids(session);
        return;
    }

    session->hids_connect_pending = true;
    session->hids_pending_since_ms = g_btstack_now_ms;
    pico_w_stack_start_le_hids_client(session);
}

static void pico_w_stack_handle_disconnect(uint8_t * packet) {
    hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
    uint16_t classic_hid_cid = 0U;
    uint16_t le_close_hid_cid = 0U;
    pico_w_stack_le_session_t * le_session = NULL;

    if (packet == NULL) {
        return;
    }

    con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
    if (con_handle == HCI_CON_HANDLE_INVALID) {
        return;
    }

    if (con_handle == g_btstack_classic_candidate_con_handle) {
        g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    }

    le_session = pico_w_stack_find_le_session_by_con_handle(con_handle);

    if (le_session != NULL) {
        const bool pairing_attempt_failed =
            g_btstack_pairing_active && !g_btstack_reconnect_pending && !le_session->connected;
        const bool reconnect_attempt_failed = g_btstack_reconnect_pending && !le_session->connected;

        le_close_hid_cid = le_session->hids_cid;

        if (le_close_hid_cid == 0U) {
            pair_device_id_t device_id = {0};
            uint8_t bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
            pico_w_stack_copy_device_id_from_addr(&device_id, le_session->addr);
            if (pico_w_stack_find_hid_cid_for_device(&device_id, &le_close_hid_cid, &bt_link_type)
                && (bt_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)) {
                le_close_hid_cid = 0U;
            }
        }

        /*
         * Emit close before reconnect handling so app-level state and LED cues
         * stay consistent even if LE hids_cid tracking was stale.
         */
        pico_w_stack_emit_le_close_event(le_close_hid_cid, le_session->addr, le_session->addr_type);

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

        if (reconnect_attempt_failed || pairing_attempt_failed) {
            pico_w_stack_handle_connect_failure(
                hci_event_disconnection_complete_get_reason(packet)
            );
        }

        pico_w_stack_reset_le_session(le_session);
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
        case HCI_EVENT_USER_PASSKEY_REQUEST: {
            /*
             * Defensive: with our DisplayYesNo IO capability the SSP matrix
             * never asks us to enter a passkey, but a peer with KeyboardOnly
             * IO would cause Passkey Entry where we (initiator) display a
             * value. Some controllers nevertheless deliver this event during
             * legacy/edge cases; respond with 000000 so SSP can't stall.
             */
            bd_addr_t address = {0};
            const bool security_accept = pico_w_stack_security_accept();

            hci_event_user_passkey_request_get_bd_addr(packet, address);
            if (security_accept) {
                pico_w_stack_mark_pairing_auth_attempted();
                (void)gap_ssp_passkey_response(address, 0U);
            } else {
                (void)gap_ssp_passkey_negative(address);
            }
        } break;
        case HCI_EVENT_USER_PASSKEY_NOTIFICATION:
            /*
             * Informational only: the peer asked us to display a passkey for
             * the user to type on the peer. We have no display and the peer
             * drives its own keyboard, so no response is needed; just record
             * that an authentication attempt is in flight so a later failure is
             * classified correctly.
             */
            if (pico_w_stack_security_accept()) {
                pico_w_stack_mark_pairing_auth_attempted();
            }
            break;
        case HCI_EVENT_CONNECTION_COMPLETE: {
            /*
             * Remember the Classic ACL handle for the in-flight candidate so a
             * failed/timed-out connect can drop it (and the half-open hid_host
             * SDP session that rides on it). Only the candidate's own ACL is
             * tracked; unrelated inbound ACLs are ignored.
             */
            bd_addr_t connected_addr = {0};

            hci_event_connection_complete_get_bd_addr(packet, connected_addr);
            if ((hci_event_connection_complete_get_status(packet) == ERROR_CODE_SUCCESS)
                && (memcmp(connected_addr, g_btstack_candidate_addr, sizeof(connected_addr))
                    == 0)) {
                g_btstack_classic_candidate_con_handle =
                    hci_event_connection_complete_get_connection_handle(packet);
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
        case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
            const uint8_t bonding_status = gap_event_dedicated_bonding_completed_get_status(packet);
            bd_addr_t bonded_addr = {0};

            gap_event_dedicated_bonding_completed_get_address(packet, bonded_addr);

            g_btstack_connect_pending = false;
            g_btstack_connect_command_issued = false;
            g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
            g_btstack_connect_pending_since_ms = 0U;

            if ((bonding_status == ERROR_CODE_SUCCESS)
                && g_btstack_pairing_active
                && (g_btstack_pairing_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC)) {
                /*
                 * Bond established and -- via ENABLE_EXPLICIT_DEDICATED_-
                 * BONDING_DISCONNECT -- the ACL is still up and already
                 * encrypted with the stored authenticated link key. Magic
                 * Keyboard does not open HID itself (confirmed by trace: no
                 * inbound HID and no re-page in any configuration), so we
                 * open it. hid_host_connect here reuses the existing ACL
                 * (l2cap_create_channel finds the live connection) and opens
                 * HID-Control immediately without re-paging or re-running
                 * SSP -- unlike a cold hid_host_connect, which triggers SSP
                 * from inside L2CAP channel setup and stalls on this peer.
                 *
                 * Clear pairing_attempt_consumed just long enough for the
                 * schedule to take (it is re-set immediately) so we don't loop
                 * into a second inquiry pass.
                 */
                g_btstack_pairing_auth_attempted = false;
                pico_w_stack_clear_pairing_failure_phase();
                g_btstack_pairing_attempt_consumed = false;
                pico_w_stack_schedule_candidate(
                    bonded_addr,
                    BD_ADDR_TYPE_ACL,
                    PICO_W_STACK_CONNECT_MODE_CLASSIC,
                    false
                );
            } else {
                pico_w_stack_handle_connect_failure(bonding_status);
            }
        } break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            pico_w_stack_handle_disconnect(packet);
            break;
        case HCI_EVENT_GATTSERVICE_META:
            pico_w_stack_handle_gattservice_meta(packet);
            break;
        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION:
                    if (pico_w_stack_security_accept()
                        && !g_btstack_connect_pending
                        && pico_w_stack_connection_capacity_available()) {
                        (void)hid_host_accept_connection(
                            hid_subevent_incoming_connection_get_hid_cid(packet),
                            HID_PROTOCOL_MODE_REPORT
                        );
                    } else {
                        bd_addr_t incoming_addr = {0};
                        link_key_t link_key = {0};
                        link_key_type_t link_key_type = INVALID_LINK_KEY;

                        hid_subevent_incoming_connection_get_address(packet, incoming_addr);
                        if (!g_btstack_connect_pending
                            && pico_w_stack_connection_capacity_available()
                            && gap_get_link_key_for_bd_addr(
                                incoming_addr,
                                link_key,
                                &link_key_type
                            )) {
                            /*
                             * Allow previously bonded keyboards to reconnect
                             * even when app-level reconnect backoff is active.
                             */
                            (void)hid_host_accept_connection(
                                hid_subevent_incoming_connection_get_hid_cid(packet),
                                HID_PROTOCOL_MODE_REPORT
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
    hid_transport_runtime_init(&g_transport_runtime);
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
    g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    g_btstack_inquiry_active = false;
    g_btstack_scan_active = false;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_reconnect_pending = false;
    g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
    g_btstack_now_ms = 0U;
    g_btstack_connect_pending_since_ms = 0U;
    pico_w_stack_clear_reconnect_state();
    (void)memset(g_btstack_candidate_addr, 0, sizeof(g_btstack_candidate_addr));
    g_btstack_candidate_addr_type = BD_ADDR_TYPE_UNKNOWN;
    pico_w_stack_clear_pairing_failure_phase();
    pico_w_stack_reset_le_sessions();
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
    /*
     * Force the GATT client to encrypt before issuing any ATT requests, using
     * the stored LTK. BTstack defaults gatt_client_required_security_level to
     * LEVEL_0, in which case service discovery starts cleartext and only falls
     * back to reencryption after the peripheral rejects a read with
     * ATT_ERROR_INSUFFICIENT_AUTHENTICATION. Some HID peripherals tear the link
     * down before that recovery path can complete on reconnect.
     */
    gatt_client_set_required_security_level(LEVEL_2);
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
    /*
     * Classic SSP IO capability is DisplayYesNo and MITM Protection is
     * declared as required so peers that gate pairing on MITM-protected
     * bonding (e.g. Apple Magic Keyboard) negotiate the Numeric Comparison
     * association model instead of Just Works.
     *
     * Per Core Spec Vol 3 Part C 5.2.2.6, if either device declares MITM
     * Protection Not Required the SSP flow always degrades to Just Works
     * regardless of IO capability. Magic Keyboard rejects the resulting
     * unauthenticated link key with authentication-failure, which our
     * pairing attempt then surfaces as an AUTH_FAILED two-blink error.
     *
     * The Numeric Comparison 6-digit code is silently auto-accepted from
     * HCI_EVENT_USER_CONFIRMATION_REQUEST below, so no UI is needed -- the
     * resulting bond is authenticated even though we never display the
     * value to a human.
     */
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
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
    hids_client_init(
        g_btstack_le_hid_descriptor_storage,
        sizeof(g_btstack_le_hid_descriptor_storage)
    );
    hid_host_init(
        g_btstack_classic_hid_descriptor_storage,
        sizeof(g_btstack_classic_hid_descriptor_storage)
    );
    hid_host_register_packet_handler(&pico_w_btstack_packet_handler);
    g_btstack_hci_event_callback_registration.callback = &pico_w_btstack_packet_handler;
    hci_add_event_handler(&g_btstack_hci_event_callback_registration);
    g_btstack_sm_event_callback_registration.callback = &pico_w_btstack_packet_handler;
    sm_add_event_handler(&g_btstack_sm_event_callback_registration);
    /*
     * Match BTstack hid_host_demo defaults: also allow sniff mode (peripherals
     * such as Magic Keyboard expect to be able to enter sniff for power
     * management) in addition to role-switch.
     */
    gap_set_default_link_policy_settings(
        LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH
    );
    /*
     * Identify the host with a stable local name and a Computer/Desktop Class
     * of Device. Apple HID peripherals (and other peers) read these during
     * SSP/connection setup to decide whether to bond -- an unset CoD (0x000000)
     * and the BTstack default name appear to be enough to make some
     * peripherals reject pairing post-IO-capability-exchange.
     *
     * "00:00:00:00:00:00" in the local name is rewritten by BTstack with the
     * local BD_ADDR.
     */
    static const char hidrelay_local_name[] = "HID Relay 00:00:00:00:00:00";
    gap_set_local_name(hidrelay_local_name);
    gap_set_class_of_device(0x000104U);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    (void)hci_power_control(HCI_POWER_ON);
    /*
     * Mirror hid_host_demo: become discoverable. The HID peripheral can then
     * read our EIR (Extended Inquiry Response, which uses the local name above)
     * during the connection handshake -- some keyboards drop the link if the
     * host's EIR is missing.
     */
    gap_discoverable_control(1);
    /*
     * Enable page scan (connectable). BTstack defaults connectable=0, which
     * only lets peers FIND us via inquiry, not PAGE us to open a connection.
     * Per the HID profile the HID Device drives reconnection: a bonded Classic
     * keyboard (the Magic Keyboard included) pages the host on its own
     * initiative when it is powered back on. Without page scan that incoming
     * page has nowhere to land and the keyboard can never reconnect.
     */
    gap_connectable_control(1);
    g_btstack_available = true;
#else
    (void)radio_ready;
#endif

    return true;
}

void pico_w_stack_poll(uint32_t now_ms) {
    (void)now_ms;

#ifdef APP_PICO_HAS_BTSTACK
    uint8_t le_index = 0U;

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

    for (le_index = 0U; le_index < PICO_W_STACK_MAX_ACTIVE_CONNECTION; le_index++) {
        pico_w_stack_le_session_t * session = &g_btstack_le_session[le_index];

        if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
            && (session->con_handle != HCI_CON_HANDLE_INVALID)
            && session->security_pending
            && (session->security_pending_since_ms != 0U)
            && ((int32_t)(now_ms - session->security_pending_since_ms)
                >= (int32_t)PICO_W_STACK_RECONNECT_SECURITY_PENDING_TIMEOUT_MS)) {
            pico_w_stack_abandon_le_connect_attempt(session);
            pico_w_stack_handle_connect_failure(ERROR_CODE_CONNECTION_TIMEOUT);
            continue;
        }

        if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
            && (session->con_handle != HCI_CON_HANDLE_INVALID)
            && !session->connected
            && (session->hids_pending_since_ms != 0U)
            && ((int32_t)(now_ms - session->hids_pending_since_ms)
                >= (int32_t)PICO_W_STACK_RECONNECT_HIDS_PENDING_TIMEOUT_MS)) {
            pico_w_stack_abandon_le_connect_attempt(session);
            pico_w_stack_handle_connect_failure(ERROR_CODE_COMMAND_DISALLOWED);
        }
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
    bool descriptor_changed = false;

    if (!hid_transport_runtime_set_usb_plan(
            &g_transport_runtime,
            interface_count,
            descriptor_generation,
            interface_plan,
            &descriptor_changed
        )) {
        return;
    }

    if (descriptor_changed) {
        pico_w_tinyusb_runtime_request_reenumeration();
    }
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
        && !pico_w_stack_has_unconnected_le_session()) {
        (void)gap_connect_cancel();
    }

    if (!g_btstack_pairing_active) {
        g_btstack_pairing_attempt_consumed = false;
        g_btstack_pairing_auth_attempted = false;
        g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
        g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        pico_w_stack_clear_pairing_failure_phase();
        pico_w_stack_clear_reconnect_state();
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
        g_btstack_connect_pending_since_ms = 0U;
        pico_w_stack_clear_unconnected_le_pending();
        pico_w_stack_abandon_unconnected_le_sessions();
        pico_w_stack_stop_discovery();
        return;
    }

    if (pairing_mode_changed) {
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = PICO_W_STACK_CONNECT_MODE_NONE;
        g_btstack_connect_pending_since_ms = 0U;
        pico_w_stack_clear_unconnected_le_pending();
        pico_w_stack_abandon_unconnected_le_sessions();
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

    {
        uint16_t active_hid_cid = 0U;
        uint8_t active_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;

        if (pico_w_stack_find_hid_cid_for_device(device_id, &active_hid_cid, &active_link_type)) {
            pico_w_stack_emit_reconnect_result(
                device_id,
                HID_TRANSPORT_RECONNECT_RESULT_SUCCESS,
                ERROR_CODE_SUCCESS
            );
            return true;
        }
    }

    if (!pico_w_stack_connection_capacity_available()
        || ((bt_link_type_hint == HID_TRANSPORT_BT_LINK_TYPE_LE)
            && !pico_w_stack_le_session_capacity_available())) {
        pico_w_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            6U
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
        /*
         * Classic-paired devices (e.g. Magic Keyboard in its Classic HID
         * profile) reconnect over Classic only. LE attempts here would
         * route a Classic bond through the BLE path against intent, and
         * would always fail since no LE bond exists for these devices.
         */
        (void)
            pico_w_stack_reconnect_add_attempt(PICO_W_STACK_CONNECT_MODE_CLASSIC, BD_ADDR_TYPE_ACL);
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
    return hid_transport_runtime_usb_interface_count(&g_transport_runtime);
}

const uint8_t * pico_w_stack_usb_report_descriptor(
    uint8_t interface_number,
    uint16_t * out_len
) {
    hid_transport_usb_interface_plan_t interface_plan = {0};
    uint16_t fallback_len = 0U;

    if (out_len != NULL) {
        *out_len = 0U;
    }

    if (!hid_transport_runtime_usb_interface_plan_get(
            &g_transport_runtime,
            interface_number,
            &interface_plan
        )) {
        return NULL;
    }

    fallback_len = interface_plan.report_descriptor_len;

#ifdef APP_PICO_HAS_BTSTACK
    {
        const uint16_t hid_cid = interface_plan.hid_cid;
        const uint8_t bt_link_type = interface_plan.bt_link_type;

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
    return hid_transport_runtime_usb_protocol_mode(&g_transport_runtime, interface_number);
}

static const uint8_t * pico_w_stack_runtime_report_descriptor(
    uint8_t interface_number,
    uint16_t * out_len,
    void * context
) {
    (void)context;
    return pico_w_stack_usb_report_descriptor(interface_number, out_len);
}

bool pico_w_stack_take_event(hid_transport_event_t * out_event) {
    return hid_transport_runtime_take_event(&g_transport_runtime, out_event);
}

void pico_w_stack_ingest_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    (void)hid_transport_runtime_ingest_usb_report(
        &g_transport_runtime,
        interface_number,
        report,
        report_len,
        pico_w_stack_runtime_report_descriptor,
        NULL
    );
}

bool pico_w_stack_send_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    uint8_t remapped_report[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t remapped_report_len = 0U;

    if (!hid_transport_runtime_remap_bt_to_usb(
            &g_transport_runtime,
            interface_number,
            report,
            report_len,
            pico_w_stack_runtime_report_descriptor,
            NULL,
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
    hid_transport_runtime_queue_state_t queue_state = {0};

    if (out_state == NULL) {
        return false;
    }

    if (!hid_transport_runtime_queue_state_get(&g_transport_runtime, &queue_state)) {
        return false;
    }

    out_state->event_queue_depth = queue_state.event_queue_depth;
    out_state->event_queue_high_watermark = queue_state.event_queue_high_watermark;
    out_state->event_queue_dropped = queue_state.event_queue_dropped;
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
