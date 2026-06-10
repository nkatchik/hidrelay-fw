#include "transport_stack.h"

#include <stddef.h>
#include <string.h>

#ifdef APP_HAS_BTSTACK
#include "ad_parser.h"
#include "ble/gatt-service/hids_client.h"
#include "ble/gatt_client.h"
#include "ble/le_device_db.h"
#include "ble/le_device_db_tlv.h"
#include "ble/sm.h"
#include "bluetooth.h"
#include "bluetooth_data_types.h"
#include "bluetooth_gatt.h"
#include "bluetooth_sdp.h"
#include "btstack_event.h"
#include "btstack_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/hid_host.h"
#include "classic/sdp_client.h"
#include "classic/sdp_util.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "platform_bt_port.h"
#endif

#include "apple_keyboard.h"
#include "apple_trackpad.h"
#include "hid_transport_runtime.h"
#include "usb_runtime.h"

enum {
    TRANSPORT_STACK_MAX_USB_INTERFACE = HID_TRANSPORT_MAX_INTERFACE,
    TRANSPORT_STACK_MAX_ACTIVE_CONNECTION = 2U,
    TRANSPORT_STACK_INQUIRY_DURATION_UNITS = 0x08U,
    TRANSPORT_STACK_MAJOR_CLASS_PERIPHERAL = 0x05U,
    TRANSPORT_STACK_COD_MINOR_KEYBOARD_BIT = 0x40U,
    TRANSPORT_STACK_MT_ENABLE_RETRY_MS = 500U,
    TRANSPORT_STACK_MT_ENABLE_MAX_ATTEMPTS = 10U,
};

static hid_transport_runtime_t g_transport_runtime = {0};
static uint8_t g_stack_last_connect_status = 0U;

static bool transport_stack_push_event(const hid_transport_event_t * event) {
    return hid_transport_runtime_push_event(&g_transport_runtime, event);
}

static bool transport_stack_find_hid_cid_for_device(
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

#ifdef APP_HAS_BTSTACK
enum {
    TRANSPORT_STACK_LE_SCAN_INTERVAL = 48U,
    TRANSPORT_STACK_LE_SCAN_WINDOW = 48U,
    TRANSPORT_STACK_LE_CONN_SCAN_INTERVAL = 48U,
    TRANSPORT_STACK_LE_CONN_SCAN_WINDOW = 48U,
    /*
     * 7.5ms (6 x 1.25ms) is the spec minimum connection interval. The interval
     * bounds every keystroke's radio latency (average = interval/2), so bias
     * the link as low as the peripheral will schedule; a peripheral that wants
     * a slower cadence for battery life can request relaxed parameters and the
     * central honors it.
     */
    TRANSPORT_STACK_LE_CONN_INTERVAL_MIN = 6U,
    TRANSPORT_STACK_LE_CONN_INTERVAL_MAX = 9U,
    TRANSPORT_STACK_LE_CONN_LATENCY = 0U,
    TRANSPORT_STACK_LE_CONN_SUPERVISION_TIMEOUT = 40U,
    TRANSPORT_STACK_LE_CONN_CE_LENGTH_MIN = 0U,
    TRANSPORT_STACK_LE_CONN_CE_LENGTH_MAX = 0U,
    TRANSPORT_STACK_LE_HIDS_SERVICE_INDEX = 0U,
    TRANSPORT_STACK_RECONNECT_MAX_ATTEMPT = 4U,
    TRANSPORT_STACK_RECONNECT_CMD_DISALLOWED_TIMEOUT_MS = 1500U,
    TRANSPORT_STACK_RECONNECT_CONNECT_PENDING_TIMEOUT_MS = 7000U,
    TRANSPORT_STACK_RECONNECT_LE_DIRECT_PENDING_TIMEOUT_MS = 3500U,
    TRANSPORT_STACK_RECONNECT_HIDS_PENDING_TIMEOUT_MS = 3000U,
    TRANSPORT_STACK_RECONNECT_SECURITY_PENDING_TIMEOUT_MS = 30000U,
    TRANSPORT_STACK_LE_SCAN_TYPE_ACTIVE = 1U,
    TRANSPORT_STACK_LE_ADV_EVENT_CONNECTABLE_UNDIRECTED = 0x00U,
    TRANSPORT_STACK_LE_ADV_EVENT_CONNECTABLE_DIRECTED = 0x01U,
    TRANSPORT_STACK_LE_ADV_EVENT_SCAN_RESPONSE = 0x04U,
    TRANSPORT_STACK_LE_HID_APPEARANCE_MASK = 0xFFC0U,
    TRANSPORT_STACK_LE_HID_APPEARANCE_BASE = 0x03C0U,
};

typedef enum {
    TRANSPORT_STACK_CONNECT_MODE_NONE = 0,
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
    TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING,
    TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
    TRANSPORT_STACK_CONNECT_MODE_LE,
    TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST
} transport_stack_connect_mode_t;

typedef enum {
    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_NONE = 0,
    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT,
    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT,
    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_HIDS
} transport_stack_pairing_failure_phase_t;

static uint8_t transport_stack_diag_connect_mode(transport_stack_connect_mode_t mode) {
    switch (mode) {
        case TRANSPORT_STACK_CONNECT_MODE_CLASSIC:
            return 1U;
        case TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST:
        case TRANSPORT_STACK_CONNECT_MODE_LE:
            return 2U;
        case TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING:
            return 3U;
        case TRANSPORT_STACK_CONNECT_MODE_NONE:
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
} transport_stack_le_session_t;

typedef struct {
    transport_stack_connect_mode_t mode;
    bd_addr_type_t addr_type;
} transport_stack_reconnect_attempt_t;

typedef struct {
    bool used;
    hci_con_handle_t con_handle;
    uint16_t hid_cid;
    /*
     * USB Device ID (VID/PID) of this device, resolved from its SDP Device ID
     * (PnP Information) record after the HID connection opens. Drives
     * recognition of supported Apple devices (descriptor augmentation,
     * relay-side report rewriting). Per-connection so a recognized keyboard
     * and a recognized trackpad can be connected at the same time.
     */
    bool usb_id_valid;
    uint16_t usb_vendor_id;
    uint16_t usb_product_id;
    /*
     * Recognized Apple trackpads must be switched into their vendor
     * multitouch mode with a SET_REPORT (apple_trackpad_mt_enable_report)
     * after every connection -- the device falls back to plain-mouse mode on
     * link loss. Pending until the device acks the write; retried from
     * transport_stack_poll while the control channel is busy. If it never
     * sticks, the trackpad keeps working as the plain mouse its native
     * descriptor declares.
     */
    bool mt_enable_pending;
    uint8_t mt_enable_attempts;
    uint32_t mt_enable_next_attempt_ms;
} transport_stack_classic_session_t;

static uint8_t g_btstack_classic_hid_descriptor_storage[1024] = {0};
static uint8_t g_btstack_le_hid_descriptor_storage[1024] = {0};
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
 * Whether the in-flight Classic pairing candidate gets MITM-protected
 * bonding. Decided from the inquiry Class of Device keyboard bit: keyboards
 * can complete an authenticated association and Apple Magic Keyboard even
 * requires one, while NoInputNoOutput peers (trackpads, mice) can never
 * provide MITM -- requesting it makes BTstack abort the pairing locally with
 * INSUFFICIENT_SECURITY before the peer is even consulted.
 */
static bool g_btstack_candidate_mitm_required = true;
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
enum {
    /*
     * Apple reports its Device ID over Bluetooth with VendorIDSource =
     * Bluetooth-SIG and VendorID = its Bluetooth company identifier (0x004C),
     * not its USB vendor ID. macOS keys full Apple-keyboard handling (the
     * top-row function/media mapping, keyboard-model/layout recognition) off the
     * USB vendor ID 0x05AC, so the Bluetooth company ID is mapped to it.
     */
    TRANSPORT_STACK_APPLE_BLUETOOTH_COMPANY_ID = 0x004CU,
    TRANSPORT_STACK_APPLE_USB_VENDOR_ID = 0x05ACU,
};

/*
 * product_id of the recognized Apple device whose augmented descriptor is
 * currently presented to USB on each interface (0 = none/un-augmented).
 * Re-enumeration to swap in the augmented descriptor is requested only when
 * this would change, so reconnecting the same device does not re-enumerate.
 * Reset whenever the interface topology changes (transport_stack_set_usb_plan)
 * so the augmentation is re-applied against the new presentation.
 */
static uint16_t g_btstack_presented_product_id[HID_TRANSPORT_MAX_INTERFACE] = {0};
static bd_addr_t g_btstack_device_id_query_addr = {0};
/* Connection the in-flight Device ID query belongs to, so its result lands on
 * the right session when more than one device is connected. */
static uint16_t g_btstack_device_id_query_hid_cid = 0U;
static uint16_t g_btstack_device_id_vendor_source = 0U;
static uint16_t g_btstack_device_id_vendor_id = 0U;
static uint16_t g_btstack_device_id_product_id = 0U;
static uint8_t g_btstack_device_id_attribute[16] = {0};
static btstack_context_callback_registration_t g_btstack_device_id_sdp_request;
/*
 * The Device ID query is deferred from CONNECTION_OPENED to DESCRIPTOR_AVAILABLE
 * for the same connection. BTstack fetches the HID report descriptor over its
 * single SDP client after an incoming connection opens; issuing our Device ID
 * query at open contends with that fetch, and when ours wins the descriptor
 * never lands -- the keyboard connects but the USB host is shown an empty
 * descriptor (mute). Once DESCRIPTOR_AVAILABLE fires the SDP client is free by
 * definition, so the deferred query cannot starve the descriptor.
 */
static bool g_btstack_device_id_query_deferred = false;
static uint16_t g_btstack_device_id_deferred_hid_cid = 0U;
static bd_addr_t g_btstack_device_id_deferred_addr = {0};
/*
 * Last descriptor actually served to the USB host, per exported interface,
 * stamped with the device it belongs to. Served again whenever the live
 * BTstack descriptor storage cannot provide one for the same device: during a
 * warm interface window (link down, interface still exported), after a raced
 * SDP descriptor fetch on a fast reconnect, or when the host re-reads
 * descriptors (e.g. resume from sleep) while the link is re-establishing.
 * Keeping the served bytes stable is what keeps the host's view of the
 * interface usable across those gaps. Cleared on init and forget.
 */
typedef struct {
    bool valid;
    pair_device_id_t device_id;
    uint16_t len;
    uint8_t bytes[1024];
} transport_stack_descriptor_cache_t;
static transport_stack_descriptor_cache_t g_btstack_descriptor_cache[HID_TRANSPORT_MAX_INTERFACE] =
    {0};
static transport_stack_connect_mode_t g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
static transport_stack_pairing_failure_phase_t g_btstack_pairing_failure_phase =
    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_NONE;
static transport_stack_reconnect_attempt_t
    g_btstack_reconnect_attempt[TRANSPORT_STACK_RECONNECT_MAX_ATTEMPT] = {0};
static uint8_t g_btstack_reconnect_attempt_count = 0U;
static uint8_t g_btstack_reconnect_attempt_index = 0U;
static transport_stack_le_session_t g_btstack_le_session[TRANSPORT_STACK_MAX_ACTIVE_CONNECTION] = {
    0
};
static transport_stack_classic_session_t
    g_btstack_classic_session[TRANSPORT_STACK_MAX_USB_INTERFACE] = {0};

static void transport_stack_packet_handler(
    uint8_t packet_type,
    uint16_t channel,
    uint8_t * packet,
    uint16_t size
);

static bool transport_stack_device_id_valid(const pair_device_id_t * device_id) {
    static const pair_device_id_t zero_id = {.bytes = {0U, 0U, 0U, 0U, 0U, 0U}};

    if (device_id == NULL) {
        return false;
    }

    return memcmp(device_id->bytes, zero_id.bytes, sizeof(device_id->bytes)) != 0;
}

static void transport_stack_copy_device_id_from_addr(
    pair_device_id_t * device_id,
    const bd_addr_t addr
) {
    if ((device_id == NULL) || (addr == NULL)) {
        return;
    }

    (void)memcpy(device_id->bytes, addr, sizeof(device_id->bytes));
}

static void transport_stack_copy_addr_from_device_id(
    bd_addr_t addr,
    const pair_device_id_t * device_id
) {
    if ((addr == NULL) || (device_id == NULL)) {
        return;
    }

    (void)memcpy(addr, device_id->bytes, sizeof(device_id->bytes));
}

static void transport_stack_reset_le_session(transport_stack_le_session_t * session) {
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

static void transport_stack_reset_le_sessions(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        transport_stack_reset_le_session(&g_btstack_le_session[index]);
    }
}

static transport_stack_le_session_t * transport_stack_find_le_session_by_con_handle(
    hci_con_handle_t con_handle
) {
    uint8_t index = 0U;

    if (con_handle == HCI_CON_HANDLE_INVALID) {
        return NULL;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == con_handle) {
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static transport_stack_le_session_t * transport_stack_find_le_session_by_hids_cid(
    uint16_t hids_cid
) {
    uint8_t index = 0U;

    if (hids_cid == 0U) {
        return NULL;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].hids_cid == hids_cid) {
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static transport_stack_le_session_t * transport_stack_allocate_le_session(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == HCI_CON_HANDLE_INVALID) {
            transport_stack_reset_le_session(&g_btstack_le_session[index]);
            return &g_btstack_le_session[index];
        }
    }

    return NULL;
}

static bool transport_stack_le_session_capacity_available(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle == HCI_CON_HANDLE_INVALID) {
            return true;
        }
    }

    return false;
}

static bool transport_stack_has_unconnected_le_session(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if ((g_btstack_le_session[index].con_handle != HCI_CON_HANDLE_INVALID)
            && !g_btstack_le_session[index].connected) {
            return true;
        }
    }

    return false;
}

static void transport_stack_clear_unconnected_le_pending(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        transport_stack_le_session_t * session = &g_btstack_le_session[index];

        if (session->connected) {
            continue;
        }

        session->security_pending = false;
        session->hids_connect_pending = false;
        session->security_pending_since_ms = 0U;
        session->hids_pending_since_ms = 0U;
    }
}

static void transport_stack_abandon_le_connect_attempt(transport_stack_le_session_t * session) {
    if ((session == NULL) || session->connected) {
        return;
    }

    if (session->hids_cid != 0U) {
        (void)hids_client_disconnect(session->hids_cid);
    }

    if (session->con_handle != HCI_CON_HANDLE_INVALID) {
        (void)gap_disconnect(session->con_handle);
    }

    transport_stack_reset_le_session(session);
}

static void transport_stack_abandon_unconnected_le_sessions(void) {
    uint8_t index = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        transport_stack_le_session_t * session = &g_btstack_le_session[index];

        if (session->connected) {
            continue;
        }

        transport_stack_abandon_le_connect_attempt(session);
    }
}

static void transport_stack_update_le_identity(
    hci_con_handle_t con_handle,
    bd_addr_type_t identity_addr_type,
    const bd_addr_t identity_addr
) {
    transport_stack_le_session_t * session = NULL;

    if ((con_handle == HCI_CON_HANDLE_INVALID) || (identity_addr == NULL)) {
        return;
    }

    session = transport_stack_find_le_session_by_con_handle(con_handle);
    if (session == NULL) {
        return;
    }

    (void)memcpy(session->addr, identity_addr, sizeof(session->addr));
    session->addr_type = identity_addr_type;
}

static void transport_stack_reset_classic_session(void) {
    (void)memset(g_btstack_classic_session, 0, sizeof(g_btstack_classic_session));
}

static void transport_stack_remember_classic_session(
    uint16_t hid_cid,
    hci_con_handle_t con_handle
) {
    uint8_t index = 0U;

    if ((hid_cid == 0U) || (con_handle == HCI_CON_HANDLE_INVALID)) {
        return;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used) {
            continue;
        }

        if ((g_btstack_classic_session[index].hid_cid == hid_cid)
            || (g_btstack_classic_session[index].con_handle == con_handle)) {
            /* A partial match means a new session reusing the slot, not the
             * same one re-remembered; its resolved USB ID no longer applies. */
            if ((g_btstack_classic_session[index].hid_cid != hid_cid)
                || (g_btstack_classic_session[index].con_handle != con_handle)) {
                g_btstack_classic_session[index].usb_id_valid = false;
            }
            g_btstack_classic_session[index].hid_cid = hid_cid;
            g_btstack_classic_session[index].con_handle = con_handle;
            return;
        }
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (g_btstack_classic_session[index].used) {
            continue;
        }

        g_btstack_classic_session[index].used = true;
        g_btstack_classic_session[index].hid_cid = hid_cid;
        g_btstack_classic_session[index].con_handle = con_handle;
        g_btstack_classic_session[index].usb_id_valid = false;
        return;
    }

    g_btstack_classic_session[0].used = true;
    g_btstack_classic_session[0].hid_cid = hid_cid;
    g_btstack_classic_session[0].con_handle = con_handle;
    g_btstack_classic_session[0].usb_id_valid = false;
}

static void transport_stack_forget_classic_session_by_hid_cid(uint16_t hid_cid) {
    uint8_t index = 0U;

    if (hid_cid == 0U) {
        return;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used
            || (g_btstack_classic_session[index].hid_cid != hid_cid)) {
            continue;
        }

        (void)
            memset(&g_btstack_classic_session[index], 0, sizeof(g_btstack_classic_session[index]));
        return;
    }
}

static bool transport_stack_find_classic_hid_cid_by_con_handle(
    hci_con_handle_t con_handle,
    uint16_t * out_hid_cid
) {
    uint8_t index = 0U;

    if ((con_handle == HCI_CON_HANDLE_INVALID) || (out_hid_cid == NULL)) {
        return false;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (!g_btstack_classic_session[index].used
            || (g_btstack_classic_session[index].con_handle != con_handle)) {
            continue;
        }

        *out_hid_cid = g_btstack_classic_session[index].hid_cid;
        return true;
    }

    return false;
}

static transport_stack_classic_session_t * transport_stack_classic_session_by_hid_cid(
    uint16_t hid_cid
) {
    uint8_t index = 0U;

    if (hid_cid == 0U) {
        return NULL;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (g_btstack_classic_session[index].used
            && (g_btstack_classic_session[index].hid_cid == hid_cid)) {
            return &g_btstack_classic_session[index];
        }
    }

    return NULL;
}

/* Resolved USB VID/PID for the Classic connection, once its Device ID query
 * has completed. False until then (or for an unknown vendor source). */
static bool transport_stack_classic_session_usb_id(
    uint16_t hid_cid,
    uint16_t * out_vendor_id,
    uint16_t * out_product_id
) {
    const transport_stack_classic_session_t * session =
        transport_stack_classic_session_by_hid_cid(hid_cid);

    if ((session == NULL) || !session->usb_id_valid) {
        return false;
    }

    if (out_vendor_id != NULL) {
        *out_vendor_id = session->usb_vendor_id;
    }
    if (out_product_id != NULL) {
        *out_product_id = session->usb_product_id;
    }
    return true;
}

/*
 * Send the pending multitouch-enable SET_REPORT for a recognized trackpad.
 * Failures (control channel busy, device still settling) retry on the next
 * poll tick until the device acks (HID_SUBEVENT_SET_REPORT_RESPONSE) or the
 * attempt budget runs out.
 */
static void transport_stack_try_send_mt_enable(transport_stack_classic_session_t * session) {
    uint8_t report_id = 0U;
    uint8_t payload[4] = {0};
    uint8_t payload_len = 0U;

    if ((session == NULL)
        || !session->used
        || !session->mt_enable_pending
        || (session->hid_cid == 0U)
        || ((int32_t)(g_btstack_now_ms - session->mt_enable_next_attempt_ms) < 0)) {
        return;
    }

    if (!session->usb_id_valid
        || !apple_trackpad_mt_enable_report(
            session->usb_product_id,
            &report_id,
            payload,
            &payload_len
        )
        || (session->mt_enable_attempts >= TRANSPORT_STACK_MT_ENABLE_MAX_ATTEMPTS)) {
        session->mt_enable_pending = false;
        return;
    }

    session->mt_enable_attempts = (uint8_t)(session->mt_enable_attempts + 1U);
    session->mt_enable_next_attempt_ms = g_btstack_now_ms + TRANSPORT_STACK_MT_ENABLE_RETRY_MS;
    (void)hid_host_send_set_report(
        session->hid_cid,
        HID_REPORT_TYPE_FEATURE,
        report_id,
        payload,
        payload_len
    );
}

static uint8_t transport_stack_active_connection_count(void) {
    uint8_t index = 0U;
    uint8_t count = 0U;

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        if (g_btstack_le_session[index].con_handle != HCI_CON_HANDLE_INVALID) {
            count = (uint8_t)(count + 1U);
        }
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_USB_INTERFACE; index++) {
        if (g_btstack_classic_session[index].used) {
            count = (uint8_t)(count + 1U);
        }
    }

    return count;
}

static bool transport_stack_connection_capacity_available(void) {
    return transport_stack_active_connection_count() < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION;
}

static bool transport_stack_security_accept(void) {
    return g_btstack_pairing_active
        || g_btstack_reconnect_pending
        || (g_btstack_connect_mode != TRANSPORT_STACK_CONNECT_MODE_NONE);
}

/*
 * Classic SSP/PIN requests carry the peer address, so gate them per peer:
 * accept while user-initiated pairing is active, for the in-flight
 * connect/reconnect candidate, or for an already-bonded peer re-pairing
 * (e.g. after it dropped its own key). The blanket security_accept() window
 * would otherwise let an unrelated device bond whenever a reconnect to an
 * offline paired keyboard happens to be pending.
 */
static bool transport_stack_security_accept_for_addr(const bd_addr_t addr) {
    bd_addr_t addr_copy = {0};
    link_key_t link_key = {0};
    link_key_type_t link_key_type = INVALID_LINK_KEY;

    if (addr == NULL) {
        return false;
    }

    if (g_btstack_pairing_active) {
        return true;
    }

    if ((g_btstack_reconnect_pending
            || (g_btstack_connect_mode != TRANSPORT_STACK_CONNECT_MODE_NONE))
        && (memcmp(addr, g_btstack_candidate_addr, sizeof(bd_addr_t)) == 0)) {
        return true;
    }

    (void)memcpy(addr_copy, addr, sizeof(addr_copy));
    return gap_get_link_key_for_bd_addr(addr_copy, link_key, &link_key_type) != 0;
}

static bool transport_stack_valid_pairing_link_type(uint8_t bt_link_type) {
    return (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC);
}

static bool transport_stack_status_is_auth_related(uint8_t status_code) {
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

static uint8_t transport_stack_classify_reconnect_failure(
    uint8_t status_code,
    bool auth_attempted
) {
    if ((status_code == ERROR_CODE_COMMAND_DISALLOWED)
        || (status_code == BTSTACK_MEMORY_ALLOC_FAILED)) {
        return HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED;
    }

    if (auth_attempted || transport_stack_status_is_auth_related(status_code)) {
        return HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED;
    }

    return HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
}

static uint8_t transport_stack_classify_pairing_failure(
    uint8_t status_code,
    bool auth_attempted
) {
    const uint8_t base_result =
        transport_stack_classify_reconnect_failure(status_code, auth_attempted);

    if (base_result != HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED) {
        return base_result;
    }

    switch (g_btstack_pairing_failure_phase) {
        case TRANSPORT_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_CLASSIC_CONNECT_FAILED;
        case TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_CONNECT_FAILED;
        case TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_HIDS:
            return HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_HIDS_FAILED;
        case TRANSPORT_STACK_PAIRING_FAILURE_PHASE_NONE:
        default:
            return base_result;
    }
}

static void transport_stack_emit_reconnect_result(
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
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_reconnect_result_for_candidate(
    uint8_t reconnect_result,
    uint8_t status_code
) {
    pair_device_id_t device_id = {0};

    transport_stack_copy_device_id_from_addr(&device_id, g_btstack_candidate_addr);
    transport_stack_emit_reconnect_result(&device_id, reconnect_result, status_code);
}

static void transport_stack_clear_reconnect_state(void) {
    g_btstack_reconnect_pending = false;
    g_btstack_reconnect_auth_attempted = false;
    g_btstack_reconnect_attempt_count = 0U;
    g_btstack_reconnect_attempt_index = 0U;
    (void)memset(g_btstack_reconnect_attempt, 0, sizeof(g_btstack_reconnect_attempt));
}

static void transport_stack_mark_pairing_auth_attempted(void) {
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        g_btstack_pairing_auth_attempted = true;
    }
}

static void transport_stack_clear_pairing_failure_phase(void) {
    g_btstack_pairing_failure_phase = TRANSPORT_STACK_PAIRING_FAILURE_PHASE_NONE;
}

static void transport_stack_set_pairing_failure_phase(
    transport_stack_pairing_failure_phase_t phase
) {
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        g_btstack_pairing_failure_phase = phase;
    }
}

static uint8_t transport_stack_map_protocol_mode(uint8_t mode) {
    switch ((hid_protocol_mode_t)mode) {
        case HID_PROTOCOL_MODE_BOOT:
            return HID_TRANSPORT_PROTOCOL_BOOT;
        case HID_PROTOCOL_MODE_REPORT:
            return HID_TRANSPORT_PROTOCOL_REPORT;
        default:
            return HID_TRANSPORT_PROTOCOL_UNKNOWN;
    }
}

static uint8_t transport_stack_map_bt_addr_type_from_stack(bd_addr_type_t addr_type) {
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

static bd_addr_type_t transport_stack_map_bt_addr_type_to_stack(uint8_t addr_type) {
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

static bool transport_stack_is_valid_le_addr_type(bd_addr_type_t addr_type) {
    return (addr_type == BD_ADDR_TYPE_LE_PUBLIC)
        || (addr_type == BD_ADDR_TYPE_LE_RANDOM)
        || (addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY)
        || (addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY);
}

static void transport_stack_request_le_low_latency_params(transport_stack_le_session_t * session) {
    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        return;
    }

    /*
     * Central-initiated HCI LE Connection Update. The previous
     * gap_request_connection_parameter_update() here is the peripheral-role
     * API (it sends an L2CAP parameter update REQUEST to the central) -- from
     * our central role it never changed anything. Best-effort: the peripheral
     * can still request relaxed parameters afterwards and the central honors
     * that, so this only biases the link toward low latency.
     */
    (void)gap_update_connection_parameters(
        session->con_handle,
        TRANSPORT_STACK_LE_CONN_INTERVAL_MIN,
        TRANSPORT_STACK_LE_CONN_INTERVAL_MAX,
        TRANSPORT_STACK_LE_CONN_LATENCY,
        TRANSPORT_STACK_LE_CONN_SUPERVISION_TIMEOUT
    );
}

static uint8_t transport_stack_connect_with_reconnect_whitelist(void) {
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

        if (!transport_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
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

            if (!transport_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
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
    if (!transport_stack_is_valid_le_addr_type((bd_addr_type_t)entry_addr_type)) {
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }

    status = gap_whitelist_add((bd_addr_type_t)entry_addr_type, entry_addr);
    if ((status != ERROR_CODE_SUCCESS) && (status != ERROR_CODE_COMMAND_DISALLOWED)) {
        return status;
    }

    return gap_connect_with_whitelist();
}

static bool transport_stack_pairing_policy_allows_cod(uint32_t class_of_device) {
    const uint8_t major_class = (uint8_t)((class_of_device >> 8U) & 0x1FU);
    return major_class == TRANSPORT_STACK_MAJOR_CLASS_PERIPHERAL;
}

static bool transport_stack_pairing_policy_adv_has_hid_uuid(const uint8_t * packet) {
    const uint8_t * ad_data = NULL;
    uint8_t ad_len = 0U;

    if (packet == NULL) {
        return false;
    }

    ad_data = gap_event_advertising_report_get_data(packet);
    ad_len = gap_event_advertising_report_get_data_length(packet);
    return ad_data_contains_uuid16(ad_len, ad_data, ORG_BLUETOOTH_SERVICE_HUMAN_INTERFACE_DEVICE);
}

static char transport_stack_ascii_lower(char value) {
    if ((value >= 'A') && (value <= 'Z')) {
        return (char)(value + ('a' - 'A'));
    }

    return value;
}

static bool transport_stack_ascii_contains_fragment(
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
            if (transport_stack_ascii_lower((char)data[data_index + fragment_index])
                != transport_stack_ascii_lower(fragment[fragment_index])) {
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

static bool transport_stack_pairing_policy_adv_has_hid_name(const uint8_t * packet) {
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

        if (transport_stack_ascii_contains_fragment(data, data_len, "keyboard")
            || transport_stack_ascii_contains_fragment(data, data_len, "mouse")
            || transport_stack_ascii_contains_fragment(data, data_len, "trackpad")) {
            return true;
        }
    }

    return false;
}

static bool transport_stack_pairing_policy_adv_connectable(const uint8_t * packet) {
    const uint8_t advertising_event_type =
        gap_event_advertising_report_get_advertising_event_type(packet);
    return (advertising_event_type == TRANSPORT_STACK_LE_ADV_EVENT_CONNECTABLE_UNDIRECTED)
        || (advertising_event_type == TRANSPORT_STACK_LE_ADV_EVENT_CONNECTABLE_DIRECTED);
}

static bool transport_stack_pairing_policy_adv_scan_response(const uint8_t * packet) {
    const uint8_t advertising_event_type =
        gap_event_advertising_report_get_advertising_event_type(packet);
    return advertising_event_type == TRANSPORT_STACK_LE_ADV_EVENT_SCAN_RESPONSE;
}

static bool transport_stack_pairing_policy_adv_has_hid_appearance(const uint8_t * packet) {
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
        return (appearance & TRANSPORT_STACK_LE_HID_APPEARANCE_MASK)
            == TRANSPORT_STACK_LE_HID_APPEARANCE_BASE;
    }

    return false;
}

static void transport_stack_stop_inquiry(void) {
    if (!g_btstack_inquiry_active) {
        return;
    }

    (void)gap_inquiry_stop();
    g_btstack_inquiry_active = false;
}

static void transport_stack_stop_scan(void) {
    if (!g_btstack_scan_active) {
        return;
    }

    gap_stop_scan();
    g_btstack_scan_active = false;
}

static void transport_stack_stop_discovery(void) {
    transport_stack_stop_inquiry();
    transport_stack_stop_scan();
}

static void transport_stack_sync_hci_ready(void) {
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

static void transport_stack_try_start_inquiry(void) {
    if (!g_btstack_hci_ready
        || !g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_CLASSIC)
        || g_btstack_pairing_attempt_consumed
        || !transport_stack_connection_capacity_available()
        || g_btstack_connect_pending
        || g_btstack_inquiry_active) {
        return;
    }

    if (gap_inquiry_start(TRANSPORT_STACK_INQUIRY_DURATION_UNITS) == ERROR_CODE_SUCCESS) {
        g_btstack_inquiry_active = true;
    }
}

static void transport_stack_try_start_scan(void) {
    if (!g_btstack_hci_ready
        || !g_btstack_pairing_active
        || (g_btstack_pairing_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)
        || g_btstack_pairing_attempt_consumed
        || !transport_stack_connection_capacity_available()
        || !transport_stack_le_session_capacity_available()
        || g_btstack_connect_pending
        || g_btstack_scan_active) {
        return;
    }

    gap_start_scan();
    g_btstack_scan_active = true;
}

static void transport_stack_try_start_discovery(void) {
    transport_stack_try_start_scan();
    transport_stack_try_start_inquiry();
}

static void transport_stack_schedule_candidate(
    const bd_addr_t addr,
    bd_addr_type_t addr_type,
    transport_stack_connect_mode_t mode,
    bool reconnect_pending
) {
    if ((addr == NULL)
        || g_btstack_connect_pending
        || (mode == TRANSPORT_STACK_CONNECT_MODE_NONE)
        || !transport_stack_connection_capacity_available()
        || (((mode == TRANSPORT_STACK_CONNECT_MODE_LE)
                || (mode == TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST))
            && !transport_stack_le_session_capacity_available())
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
    transport_stack_clear_unconnected_le_pending();
    if (!reconnect_pending) {
        transport_stack_clear_reconnect_state();
        if (g_btstack_pairing_active) {
            g_btstack_pairing_attempt_consumed = true;
            g_btstack_pairing_auth_attempted = false;
            if ((mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC)
                || (mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING)) {
                transport_stack_set_pairing_failure_phase(
                    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_CLASSIC_CONNECT
                );
            } else if (mode == TRANSPORT_STACK_CONNECT_MODE_LE) {
                transport_stack_set_pairing_failure_phase(
                    TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_CONNECT
                );
            } else {
                transport_stack_clear_pairing_failure_phase();
            }
            transport_stack_emit_reconnect_result_for_candidate(
                HID_TRANSPORT_RECONNECT_RESULT_REQUESTED,
                ERROR_CODE_SUCCESS
            );
        }
    }
    transport_stack_stop_discovery();
}

static bool transport_stack_reconnect_add_attempt(
    transport_stack_connect_mode_t mode,
    bd_addr_type_t addr_type
) {
    uint8_t index = 0U;

    if ((mode == TRANSPORT_STACK_CONNECT_MODE_NONE)
        || (g_btstack_reconnect_attempt_count >= TRANSPORT_STACK_RECONNECT_MAX_ATTEMPT)) {
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

static bool transport_stack_reconnect_apply_attempt(uint8_t attempt_index) {
    if (attempt_index >= g_btstack_reconnect_attempt_count) {
        return false;
    }

    g_btstack_reconnect_attempt_index = attempt_index;
    g_btstack_connect_mode = g_btstack_reconnect_attempt[attempt_index].mode;
    g_btstack_candidate_addr_type = g_btstack_reconnect_attempt[attempt_index].addr_type;
    g_btstack_connect_pending = true;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_pending_since_ms = g_btstack_now_ms;
    transport_stack_clear_unconnected_le_pending();
    return true;
}

static bool transport_stack_try_reconnect_next_attempt(void) {
    uint8_t next_index = 0U;

    if (!g_btstack_reconnect_pending) {
        return false;
    }

    next_index = (uint8_t)(g_btstack_reconnect_attempt_index + 1U);
    if (next_index >= g_btstack_reconnect_attempt_count) {
        return false;
    }

    return transport_stack_reconnect_apply_attempt(next_index);
}

static uint32_t transport_stack_reconnect_connect_timeout_ms(void) {
    if (g_btstack_reconnect_pending
        && (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE)) {
        return TRANSPORT_STACK_RECONNECT_LE_DIRECT_PENDING_TIMEOUT_MS;
    }

    return TRANSPORT_STACK_RECONNECT_CONNECT_PENDING_TIMEOUT_MS;
}

static void transport_stack_handle_connect_failure(uint8_t status_code);

static void transport_stack_request_le_pairing_before_hids(transport_stack_le_session_t * session) {
    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        transport_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    transport_stack_mark_pairing_auth_attempted();
    session->security_pending = true;
    session->security_pending_since_ms = g_btstack_now_ms;
    session->hids_connect_pending = false;
    session->hids_pending_since_ms = 0U;
    sm_request_pairing(session->con_handle);
}

/*
 * Restore the boot-time SSP authentication requirement after a dedicated
 * bonding attempt may have relaxed it for a NoInputNoOutput candidate, so
 * inbound re-pairing from a bonded keyboard still negotiates an
 * authenticated association (Magic Keyboard rejects unauthenticated keys).
 */
static void transport_stack_restore_default_ssp_auth_requirement(void) {
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
}

static void transport_stack_handle_connect_failure(uint8_t status_code) {
    const bool auth_attempted =
        g_btstack_reconnect_auth_attempted || g_btstack_pairing_auth_attempted;

    if (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING) {
        transport_stack_restore_default_ssp_auth_requirement();
    }

    if ((g_btstack_connect_mode
            == TRANSPORT_STACK_CONNECT_MODE_LE
            || g_btstack_connect_mode
            == TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST)
        && !transport_stack_has_unconnected_le_session()) {
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
    g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    transport_stack_clear_unconnected_le_pending();
    g_btstack_pairing_auth_attempted = false;

    if (g_btstack_reconnect_pending) {
        if (transport_stack_try_reconnect_next_attempt()) {
            return;
        }

        transport_stack_emit_reconnect_result_for_candidate(
            transport_stack_classify_reconnect_failure(status_code, auth_attempted),
            status_code
        );
        transport_stack_clear_reconnect_state();
    } else if (g_btstack_pairing_active) {
        transport_stack_emit_reconnect_result_for_candidate(
            transport_stack_classify_pairing_failure(status_code, auth_attempted),
            status_code
        );
    }

    transport_stack_clear_pairing_failure_phase();
    transport_stack_try_start_discovery();
}

static bool transport_stack_status_requires_pairing_retry(uint8_t status_code) {
    return (status_code == ERROR_CODE_AUTHENTICATION_FAILURE)
        || (status_code == ERROR_CODE_PIN_OR_KEY_MISSING)
        || (status_code == ERROR_CODE_PAIRING_NOT_ALLOWED)
        || (status_code == ERROR_CODE_INSUFFICIENT_SECURITY)
        || (status_code == ATT_ERROR_INSUFFICIENT_AUTHORIZATION)
        || (status_code == ATT_ERROR_INSUFFICIENT_ENCRYPTION)
        || (status_code == ATT_ERROR_INSUFFICIENT_ENCRYPTION_KEY_SIZE);
}

static void transport_stack_handle_connect_success(void) {
    g_stack_last_connect_status = ERROR_CODE_SUCCESS;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
    g_btstack_connect_pending_since_ms = 0U;
    /*
     * The candidate ACL is now a live HID session; stop tracking it as a
     * teardown target so a later unrelated connect failure cannot disconnect
     * the working keyboard.
     */
    g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    transport_stack_clear_unconnected_le_pending();
    g_btstack_pairing_auth_attempted = false;
    transport_stack_clear_pairing_failure_phase();

    if (g_btstack_reconnect_pending) {
        transport_stack_emit_reconnect_result_for_candidate(
            HID_TRANSPORT_RECONNECT_RESULT_SUCCESS,
            ERROR_CODE_SUCCESS
        );
        transport_stack_clear_reconnect_state();
    }
}

static void transport_stack_emit_classic_open_event(uint8_t * packet) {
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
    transport_stack_remember_classic_session(
        event.hid_cid,
        hid_subevent_connection_opened_get_con_handle(packet)
    );
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_classic_descriptor_event(uint8_t * packet) {
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
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_bt_protocol_event(
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
    event.protocol_mode = transport_stack_map_protocol_mode(protocol_mode);
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_classic_close_by_hid_cid(uint16_t hid_cid) {
    hid_transport_event_t event = {0};

    if (hid_cid == 0U) {
        return;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.hid_cid = hid_cid;
    if (g_btstack_device_id_query_deferred && (g_btstack_device_id_deferred_hid_cid == hid_cid)) {
        g_btstack_device_id_query_deferred = false;
        g_btstack_device_id_deferred_hid_cid = 0U;
    }
    transport_stack_forget_classic_session_by_hid_cid(hid_cid);
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_classic_close_event(uint8_t * packet) {
    if (packet == NULL) {
        return;
    }

    transport_stack_emit_classic_close_by_hid_cid(
        hid_subevent_connection_closed_get_hid_cid(packet)
    );
}

/* Remap state for the connected Apple keyboard (single-device scope). */
static apple_keyboard_state_t g_apple_keyboard_state = {0};
/* Gesture-engine state for the connected Apple trackpad (single-device scope:
 * one trackpad at a time; a second one would steal the engine). */
static apple_trackpad_state_t g_apple_trackpad_state = {0};

static void transport_stack_push_classic_report(
    uint16_t hid_cid,
    const uint8_t * bytes,
    uint16_t len
) {
    hid_transport_event_t event = {0};

    if (len > HID_TRANSPORT_REPORT_MAX_LEN) {
        len = HID_TRANSPORT_REPORT_MAX_LEN;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.hid_cid = hid_cid;
    event.report_len = len;

    if ((bytes != NULL) && (len > 0U)) {
        (void)memcpy(event.report, bytes, len);
    }

    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_classic_report_event(uint8_t * packet) {
    const uint8_t * report = NULL;
    uint16_t report_len = 0U;
    uint16_t hid_cid = 0U;

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

    hid_cid = hid_subevent_report_get_hid_cid(packet);

    /*
     * Recognized Apple keyboards: rewrite the top row onto the augmented
     * Apple-vendor/consumer collection (see transport_stack_usb_report_descriptor).
     * The transform may also emit a second aux report on press/release, so both
     * are pushed as separate USB reports on the same interface.
     */
    uint16_t usb_vendor_id = 0U;
    uint16_t usb_product_id = 0U;
    const bool usb_id_valid =
        transport_stack_classic_session_usb_id(hid_cid, &usb_vendor_id, &usb_product_id);

    /*
     * Recognized Apple trackpads: consume vendor multitouch frames and emit
     * the synthesized standard reports declared by the augmented descriptor
     * (pointer, buttons, scroll). Reports that are not multitouch frames --
     * plain-mouse reports before the multitouch enable lands -- fall through
     * and are forwarded raw.
     */
    if (usb_id_valid && apple_trackpad_is_supported(usb_vendor_id, usb_product_id)) {
        apple_trackpad_out_t trackpad_out = {0};

        if (!g_apple_trackpad_state.initialized
            || (g_apple_trackpad_state.product_id != usb_product_id)) {
            apple_trackpad_state_init(&g_apple_trackpad_state, usb_product_id);
        }

        if (apple_trackpad_process_report(
                &g_apple_trackpad_state,
                report,
                report_len,
                g_btstack_now_ms,
                &trackpad_out
            )) {
            uint8_t out_index = 0U;

            for (out_index = 0U; out_index < trackpad_out.count; out_index++) {
                transport_stack_push_classic_report(
                    hid_cid,
                    trackpad_out.bytes[out_index],
                    trackpad_out.len[out_index]
                );
            }
            return;
        }
    }

    if (usb_id_valid && apple_keyboard_is_supported(usb_vendor_id, usb_product_id)) {
        uint8_t kbd[HID_TRANSPORT_REPORT_MAX_LEN];
        uint8_t aux[APPLE_KEYBOARD_AUX_REPORT_LEN];
        uint16_t kbd_len = 0U;
        uint16_t aux_len = 0U;

        if (!g_apple_keyboard_state.initialized
            || (g_apple_keyboard_state.product_id != usb_product_id)) {
            apple_keyboard_state_init(&g_apple_keyboard_state, usb_product_id);
        }

        if (apple_keyboard_process_report(
                &g_apple_keyboard_state,
                report,
                report_len,
                kbd,
                &kbd_len,
                aux,
                &aux_len
            )) {
            transport_stack_push_classic_report(hid_cid, kbd, kbd_len);
            if (aux_len > 0U) {
                transport_stack_push_classic_report(hid_cid, aux, aux_len);
            }
            return;
        }
    }

    transport_stack_push_classic_report(hid_cid, report, report_len);
}

static void transport_stack_emit_le_open_event(
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
    event.bt_addr_type = transport_stack_map_bt_addr_type_from_stack(addr_type);
    event.hid_cid = hids_cid;
    event.vendor_id = 0U;
    event.product_id = 0U;
    event.report_descriptor_len = 0U;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    (void)memcpy(event.device_id.bytes, addr, sizeof(event.device_id.bytes));
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_le_descriptor_event(
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
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_le_close_event(
    uint16_t hids_cid,
    const bd_addr_t addr,
    bd_addr_type_t addr_type
) {
    hid_transport_event_t event = {0};

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = transport_stack_map_bt_addr_type_from_stack(addr_type);
    event.hid_cid = hids_cid;
    if (addr != NULL) {
        (void)memcpy(event.device_id.bytes, addr, sizeof(event.device_id.bytes));
    }
    (void)transport_stack_push_event(&event);
}

static void transport_stack_emit_le_report_event(uint8_t * packet) {
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

    (void)transport_stack_push_event(&event);
}

static void transport_stack_try_connect_candidate(void) {
    uint8_t connect_status = ERROR_CODE_COMMAND_DISALLOWED;
    uint16_t hid_cid = 0U;

    if (!g_btstack_hci_ready || !g_btstack_connect_pending || g_btstack_connect_command_issued) {
        return;
    }

    if (!transport_stack_connection_capacity_available()
        || (((g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE)
                || (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST))
            && !transport_stack_le_session_capacity_available())) {
        transport_stack_handle_connect_failure(BTSTACK_MEMORY_ALLOC_FAILED);
        return;
    }

    if (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC) {
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
    } else if (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING) {
        /*
         * gap_dedicated_bonding pages the peer, drives SSP, generates and
         * stores the link key, and then tears down the ACL. Completion is
         * reported via GAP_EVENT_DEDICATED_BONDING_COMPLETED -- on success
         * we re-schedule the same candidate as plain CLASSIC so
         * hid_host_connect runs against a now-bonded peer.
         *
         * The SSP authentication requirement advertised in our IO Capability
         * Reply is aligned with the per-candidate MITM decision for the
         * duration of the attempt (restored when the attempt concludes), so
         * we never declare a MITM requirement the association model cannot
         * meet for a NoInputNoOutput peer.
         */
        gap_ssp_set_authentication_requirement(
            g_btstack_candidate_mitm_required
                ? SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING
                : SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING
        );
        connect_status = (uint8_t)gap_dedicated_bonding(
            g_btstack_candidate_addr,
            g_btstack_candidate_mitm_required ? 1 : 0
        );
    } else if (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE) {
        connect_status = gap_connect(g_btstack_candidate_addr, g_btstack_candidate_addr_type);
    } else if (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST) {
        connect_status = transport_stack_connect_with_reconnect_whitelist();
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
                >= (int32_t)TRANSPORT_STACK_RECONNECT_CMD_DISALLOWED_TIMEOUT_MS)) {
            transport_stack_handle_connect_failure(connect_status);
        }
        return;
    }

    transport_stack_handle_connect_failure(connect_status);
}

static void transport_stack_start_le_hids_client(transport_stack_le_session_t * session) {
    uint16_t hids_cid = 0U;
    uint8_t status = ERROR_CODE_COMMAND_DISALLOWED;

    if ((session == NULL) || (session->con_handle == HCI_CON_HANDLE_INVALID)) {
        if (session != NULL) {
            session->hids_connect_pending = false;
            session->hids_pending_since_ms = 0U;
        }
        transport_stack_handle_connect_failure(ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER);
        return;
    }

    transport_stack_set_pairing_failure_phase(TRANSPORT_STACK_PAIRING_FAILURE_PHASE_LE_HIDS);

    status = hids_client_connect(
        session->con_handle,
        &transport_stack_packet_handler,
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
    transport_stack_abandon_le_connect_attempt(session);
    transport_stack_handle_connect_failure(status);
}

static void transport_stack_try_start_le_hids_client(void) {
    uint8_t index = 0U;

    if (!g_btstack_hci_ready) {
        return;
    }

    for (index = 0U; index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; index++) {
        transport_stack_le_session_t * session = &g_btstack_le_session[index];

        if (!session->hids_connect_pending
            || session->security_pending
            || (session->con_handle == HCI_CON_HANDLE_INVALID)
            || session->connected) {
            continue;
        }

        transport_stack_start_le_hids_client(session);
    }
}

static void transport_stack_handle_hci_inquiry_result(
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
        || !transport_stack_pairing_policy_allows_cod(class_of_device)) {
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
    transport_stack_schedule_candidate(
        addr,
        BD_ADDR_TYPE_ACL,
        TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING,
        false
    );

    /*
     * schedule_candidate early-returns on a number of conditions; connect
     * pending was false on entry, so it being set now means this candidate
     * was accepted. Keyboards (CoD minor keyboard bit) bond with MITM so
     * Apple Magic Keyboard negotiates Numeric Comparison; pointing devices
     * bond Just Works -- the same trust model a host OS uses for them.
     */
    if (g_btstack_connect_pending
        && (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_CLASSIC_DEDICATED_BONDING)) {
        g_btstack_candidate_mitm_required =
            (class_of_device & TRANSPORT_STACK_COD_MINOR_KEYBOARD_BIT) != 0U;
    }
}

static void transport_stack_handle_gap_advertising_report(uint8_t * packet) {
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

    has_hid_uuid = transport_stack_pairing_policy_adv_has_hid_uuid(packet);
    connectable = transport_stack_pairing_policy_adv_connectable(packet);
    scan_response = transport_stack_pairing_policy_adv_scan_response(packet);
    has_hid_appearance = transport_stack_pairing_policy_adv_has_hid_appearance(packet);
    has_hid_name = transport_stack_pairing_policy_adv_has_hid_name(packet);
    if (!has_hid_uuid
        && !(connectable && (has_hid_appearance || has_hid_name))
        && !(scan_response && (has_hid_appearance || has_hid_name))) {
        return;
    }

    gap_event_advertising_report_get_address(packet, addr);
    addr_type = gap_event_advertising_report_get_address_type(packet);

    transport_stack_schedule_candidate(addr, addr_type, TRANSPORT_STACK_CONNECT_MODE_LE, false);
}

static void transport_stack_handle_sm_event(uint8_t * packet) {
    const bool security_accept = transport_stack_security_accept();
    bd_addr_t identity_addr = {0};
    transport_stack_le_session_t * session = NULL;

    if (packet == NULL) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SM_EVENT_PAIRING_STARTED:
            transport_stack_mark_pairing_auth_attempted();
            break;
        case SM_EVENT_JUST_WORKS_REQUEST:
            transport_stack_mark_pairing_auth_attempted();
            if (security_accept) {
                (void)sm_just_works_confirm(sm_event_just_works_request_get_handle(packet));
            }
            break;
        case SM_EVENT_PASSKEY_DISPLAY_NUMBER:
        case SM_EVENT_PASSKEY_INPUT_NUMBER:
        case SM_EVENT_PASSKEY_DISPLAY_CANCEL:
            transport_stack_mark_pairing_auth_attempted();
            break;
        case SM_EVENT_NUMERIC_COMPARISON_REQUEST:
            transport_stack_mark_pairing_auth_attempted();
            if (security_accept) {
                (void)sm_numeric_comparison_confirm(
                    sm_event_numeric_comparison_request_get_handle(packet)
                );
            }
            break;
        case SM_EVENT_PAIRING_COMPLETE:
            session = transport_stack_find_le_session_by_con_handle(
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
                    transport_stack_start_le_hids_client(session);
                }
            } else {
                const uint8_t status = sm_event_pairing_complete_get_status(packet);
                const uint8_t reason = sm_event_pairing_complete_get_reason(packet);
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                transport_stack_mark_pairing_auth_attempted();
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                session->hids_connect_pending = false;
                transport_stack_abandon_le_connect_attempt(session);
                transport_stack_handle_connect_failure((reason != 0U) ? reason : status);
            }
            break;
        case SM_EVENT_REENCRYPTION_COMPLETE:
            session = transport_stack_find_le_session_by_con_handle(
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
                    transport_stack_start_le_hids_client(session);
                }
            } else {
                if (g_btstack_reconnect_pending) {
                    g_btstack_reconnect_auth_attempted = true;
                }
                transport_stack_mark_pairing_auth_attempted();
                session->security_pending = false;
                session->security_pending_since_ms = 0U;
                session->hids_connect_pending = false;
                transport_stack_abandon_le_connect_attempt(session);
                transport_stack_handle_connect_failure(
                    sm_event_reencryption_complete_get_status(packet)
                );
            }
            break;
        case SM_EVENT_IDENTITY_CREATED:
            sm_event_identity_created_get_identity_address(packet, identity_addr);
            transport_stack_update_le_identity(
                sm_event_identity_created_get_handle(packet),
                (bd_addr_type_t)sm_event_identity_created_get_identity_addr_type(packet),
                identity_addr
            );
            break;
        case SM_EVENT_IDENTITY_RESOLVING_SUCCEEDED:
            sm_event_identity_resolving_succeeded_get_identity_address(packet, identity_addr);
            transport_stack_update_le_identity(
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

static void transport_stack_handle_gattservice_meta(uint8_t * packet) {
    uint16_t hids_cid = 0U;
    uint8_t status = 0U;
    transport_stack_le_session_t * session = NULL;

    if (packet == NULL) {
        return;
    }

    switch (hci_event_gattservice_meta_get_subevent_code(packet)) {
        case GATTSERVICE_SUBEVENT_HID_SERVICE_CONNECTED:
            hids_cid = gattservice_subevent_hid_service_connected_get_hids_cid(packet);
            status = gattservice_subevent_hid_service_connected_get_status(packet);
            session = transport_stack_find_le_session_by_hids_cid(hids_cid);
            if (session == NULL) {
                break;
            }

            if (status != ERROR_CODE_SUCCESS) {
                if (!session->security_pending
                    && (session->con_handle != HCI_CON_HANDLE_INVALID)
                    && transport_stack_status_requires_pairing_retry(status)) {
                    if (g_btstack_reconnect_pending) {
                        g_btstack_reconnect_auth_attempted = true;
                    }
                    transport_stack_mark_pairing_auth_attempted();
                    session->security_pending = true;
                    session->security_pending_since_ms = g_btstack_now_ms;
                    session->hids_connect_pending = true;
                    session->hids_pending_since_ms = g_btstack_now_ms;
                    sm_request_pairing(session->con_handle);
                    break;
                }

                transport_stack_abandon_le_connect_attempt(session);
                transport_stack_handle_connect_failure(status);
                break;
            }

            session->hids_connect_pending = false;
            session->connected = true;
            session->hids_cid = hids_cid;
            session->hids_pending_since_ms = 0U;
            transport_stack_handle_connect_success();
            transport_stack_request_le_low_latency_params(session);
            transport_stack_emit_le_open_event(hids_cid, session->addr, session->addr_type);
            transport_stack_emit_bt_protocol_event(
                hids_cid,
                HID_TRANSPORT_BT_LINK_TYPE_LE,
                gattservice_subevent_hid_service_connected_get_protocol_mode(packet)
            );
            transport_stack_emit_le_descriptor_event(
                hids_cid,
                TRANSPORT_STACK_LE_HIDS_SERVICE_INDEX
            );
            break;
        case GATTSERVICE_SUBEVENT_HID_PROTOCOL_MODE:
            transport_stack_emit_bt_protocol_event(
                gattservice_subevent_hid_protocol_mode_get_hids_cid(packet),
                HID_TRANSPORT_BT_LINK_TYPE_LE,
                gattservice_subevent_hid_protocol_mode_get_protocol_mode(packet)
            );
            break;
        case GATTSERVICE_SUBEVENT_HID_REPORT:
            transport_stack_emit_le_report_event(packet);
            break;
        default:
            break;
    }
}

static void transport_stack_handle_le_connection_complete(uint8_t * packet) {
    uint8_t status = ERROR_CODE_SUCCESS;
    static const bd_addr_t zero_addr = {0};
    transport_stack_le_session_t * session = NULL;

    if (packet == NULL) {
        return;
    }

    status = gap_subevent_le_connection_complete_get_status(packet);

    if ((g_btstack_connect_mode != TRANSPORT_STACK_CONNECT_MODE_LE)
        && (g_btstack_connect_mode != TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST)) {
        if (status == ERROR_CODE_SUCCESS) {
            (void)gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(packet));
        }
        return;
    }

    if (status != ERROR_CODE_SUCCESS) {
        transport_stack_handle_connect_failure(status);
        return;
    }

    session = transport_stack_allocate_le_session();
    if (session == NULL) {
        (void)gap_disconnect(gap_subevent_le_connection_complete_get_connection_handle(packet));
        transport_stack_handle_connect_failure(BTSTACK_MEMORY_ALLOC_FAILED);
        return;
    }

    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
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
    transport_stack_request_le_low_latency_params(session);
    session->security_pending = false;
    if (g_btstack_pairing_active && !g_btstack_reconnect_pending) {
        transport_stack_request_le_pairing_before_hids(session);
        return;
    }

    session->hids_connect_pending = true;
    session->hids_pending_since_ms = g_btstack_now_ms;
    transport_stack_start_le_hids_client(session);
}

static void transport_stack_handle_disconnect(uint8_t * packet) {
    hci_con_handle_t con_handle = HCI_CON_HANDLE_INVALID;
    uint16_t classic_hid_cid = 0U;
    uint16_t le_close_hid_cid = 0U;
    transport_stack_le_session_t * le_session = NULL;

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

    le_session = transport_stack_find_le_session_by_con_handle(con_handle);

    if (le_session != NULL) {
        const bool pairing_attempt_failed =
            g_btstack_pairing_active && !g_btstack_reconnect_pending && !le_session->connected;
        const bool reconnect_attempt_failed = g_btstack_reconnect_pending && !le_session->connected;

        le_close_hid_cid = le_session->hids_cid;

        if (le_close_hid_cid == 0U) {
            pair_device_id_t device_id = {0};
            uint8_t bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
            transport_stack_copy_device_id_from_addr(&device_id, le_session->addr);
            if (transport_stack_find_hid_cid_for_device(
                    &device_id,
                    &le_close_hid_cid,
                    &bt_link_type
                )
                && (bt_link_type != HID_TRANSPORT_BT_LINK_TYPE_LE)) {
                le_close_hid_cid = 0U;
            }
        }

        /*
         * Emit close before reconnect handling so app-level state and LED cues
         * stay consistent even if LE hids_cid tracking was stale.
         */
        transport_stack_emit_le_close_event(
            le_close_hid_cid,
            le_session->addr,
            le_session->addr_type
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

        if (reconnect_attempt_failed || pairing_attempt_failed) {
            transport_stack_handle_connect_failure(
                hci_event_disconnection_complete_get_reason(packet)
            );
        }

        transport_stack_reset_le_session(le_session);
        transport_stack_try_start_discovery();
        return;
    }

    if (transport_stack_find_classic_hid_cid_by_con_handle(con_handle, &classic_hid_cid)) {
        transport_stack_emit_classic_close_by_hid_cid(classic_hid_cid);
        return;
    }

    if (g_btstack_pairing_active && g_btstack_connect_pending) {
        transport_stack_handle_connect_failure(hci_event_disconnection_complete_get_reason(packet));
    }
}

/*
 * SDP "Device ID" (PnP Information, UUID 0x1200) query result handler. The three
 * attributes we care about (VendorIDSource/VendorID/ProductID) are each a single
 * UINT16 data element; accumulate the bytes of one attribute, then
 * decode with de_element_get_uint16. On query completion, resolve the device's
 * USB VID/PID (so a supported Apple keyboard can be recognized and its
 * descriptor augmented) and re-enumerate so the host re-reads it.
 */
static void transport_stack_device_id_sdp_handler(
    uint8_t packet_type,
    uint16_t channel,
    uint8_t * packet,
    uint16_t size
) {
    uint16_t attribute_id = 0U;
    uint16_t attribute_len = 0U;
    uint16_t attribute_offset = 0U;
    uint16_t value = 0U;

    (void)channel;
    (void)size;

    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case SDP_EVENT_QUERY_ATTRIBUTE_VALUE:
            attribute_id = sdp_event_query_attribute_byte_get_attribute_id(packet);
            attribute_len = sdp_event_query_attribute_byte_get_attribute_length(packet);
            attribute_offset = sdp_event_query_attribute_byte_get_data_offset(packet);

            /*
             * The data-element header byte arrives first while attribute_length
             * is still 0 (BTstack hasn't parsed the element size yet). Store it
             * too -- skipping it would leave the accumulated buffer without its
             * DE header and de_element_get_uint16 would reject the value. Store
             * every in-range byte, then decode once the full value is in.
             */
            if ((attribute_len > sizeof(g_btstack_device_id_attribute))
                || (attribute_offset >= sizeof(g_btstack_device_id_attribute))) {
                break;
            }
            g_btstack_device_id_attribute[attribute_offset] =
                sdp_event_query_attribute_byte_get_data(packet);

            if ((attribute_len == 0U) || ((uint16_t)(attribute_offset + 1U) != attribute_len)) {
                break;
            }
            if (!de_element_get_uint16(g_btstack_device_id_attribute, &value)) {
                break;
            }
            switch (attribute_id) {
                case BLUETOOTH_ATTRIBUTE_VENDOR_ID_SOURCE:
                    g_btstack_device_id_vendor_source = value;
                    break;
                case BLUETOOTH_ATTRIBUTE_VENDOR_ID:
                    g_btstack_device_id_vendor_id = value;
                    break;
                case BLUETOOTH_ATTRIBUTE_PRODUCT_ID:
                    g_btstack_device_id_product_id = value;
                    break;
                default:
                    break;
            }
            break;
        case SDP_EVENT_QUERY_COMPLETE: {
            uint16_t usb_vendor_id = 0U;

            if (g_btstack_device_id_vendor_id == 0U) {
                break;
            }
            if (g_btstack_device_id_vendor_source == DEVICE_ID_VENDOR_ID_SOURCE_USB) {
                /* Already a USB vendor ID; use it directly. */
                usb_vendor_id = g_btstack_device_id_vendor_id;
            } else if ((g_btstack_device_id_vendor_source == DEVICE_ID_VENDOR_ID_SOURCE_BLUETOOTH)
                && (g_btstack_device_id_vendor_id == TRANSPORT_STACK_APPLE_BLUETOOTH_COMPANY_ID)) {
                /* Apple over Bluetooth: map the company ID to the USB vendor ID. */
                usb_vendor_id = TRANSPORT_STACK_APPLE_USB_VENDOR_ID;
            } else {
                /*
                 * Bluetooth-SIG-sourced ID with no known USB mapping -- leave the
                 * device unrecognized rather than guess a wrong USB vendor.
                 */
                break;
            }
            {
                transport_stack_classic_session_t * session =
                    transport_stack_classic_session_by_hid_cid(g_btstack_device_id_query_hid_cid);
                const uint16_t resolved_product = g_btstack_device_id_product_id;
                uint8_t iface = 0U;

                if (session == NULL) {
                    /* Connection closed while the query was in flight. */
                    break;
                }
                session->usb_vendor_id = usb_vendor_id;
                session->usb_product_id = resolved_product;
                session->usb_id_valid = true;

                if (apple_trackpad_is_supported(usb_vendor_id, resolved_product)) {
                    session->mt_enable_pending = true;
                    session->mt_enable_attempts = 0U;
                    session->mt_enable_next_attempt_ms = g_btstack_now_ms;
                    transport_stack_try_send_mt_enable(session);
                }

                /*
                 * Re-enumerate to swap in the augmented descriptor only when the
                 * recognized device on this interface actually changes --
                 * reconnecting the same one presents the identical descriptor,
                 * so skipping the re-enumeration keeps the interface mounted and
                 * avoids the churn that wedged the HID endpoint on a fast off/on.
                 */
                for (iface = 0U; iface < HID_TRANSPORT_MAX_INTERFACE; iface++) {
                    hid_transport_usb_interface_plan_t plan = {0};
                    uint16_t desired_product = 0U;

                    if (!hid_transport_runtime_usb_interface_plan_get(
                            &g_transport_runtime,
                            iface,
                            &plan
                        )
                        || (plan.hid_cid != session->hid_cid)) {
                        continue;
                    }
                    if (apple_keyboard_is_supported(usb_vendor_id, resolved_product)
                        || apple_trackpad_is_supported(usb_vendor_id, resolved_product)) {
                        desired_product = resolved_product;
                    }
                    if (desired_product != g_btstack_presented_product_id[iface]) {
                        g_btstack_presented_product_id[iface] = desired_product;
                        usb_runtime_request_reenumeration();
                    }
                    break;
                }
            }
        } break;
        default:
            break;
    }
}

static void transport_stack_device_id_start_query(void * context) {
    (void)context;
    (void)sdp_client_query_uuid16(
        &transport_stack_device_id_sdp_handler,
        g_btstack_device_id_query_addr,
        BLUETOOTH_SERVICE_CLASS_PNP_INFORMATION
    );
}

/*
 * Queue an SDP Device ID query against the connected device. Run after the
 * HID connection opens (the SDP client is free again by then). Resets the
 * partial-result fields; registering the callback is a no-op if a query is
 * already queued. Single query slot: should a second device's query be
 * requested while one is in flight, the first result is dropped (its session
 * just stays unrecognized until it reconnects).
 */
static void transport_stack_request_device_id(
    const uint8_t * addr,
    uint16_t hid_cid
) {
    if (addr == NULL) {
        return;
    }
    (void)memcpy(g_btstack_device_id_query_addr, addr, sizeof(g_btstack_device_id_query_addr));
    g_btstack_device_id_query_hid_cid = hid_cid;
    g_btstack_device_id_vendor_source = 0U;
    g_btstack_device_id_vendor_id = 0U;
    g_btstack_device_id_product_id = 0U;
    g_btstack_device_id_sdp_request.callback = &transport_stack_device_id_start_query;
    (void)sdp_client_register_query_callback(&g_btstack_device_id_sdp_request);
}

static void transport_stack_packet_handler(
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

        transport_stack_handle_gattservice_meta(packet);
        return;
    }

    if (packet_type != HCI_EVENT_PACKET || (size < 3U)) {
        return;
    }

    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_PIN_CODE_REQUEST: {
            bd_addr_t address = {0};
            bool security_accept = false;

            hci_event_pin_code_request_get_bd_addr(packet, address);
            security_accept = transport_stack_security_accept_for_addr(address);
            if (security_accept) {
                transport_stack_mark_pairing_auth_attempted();
                (void)gap_pin_code_response(address, "0000");
            } else {
                (void)gap_pin_code_negative(address);
            }
        } break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST: {
            bd_addr_t address = {0};
            bool security_accept = false;

            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            security_accept = transport_stack_security_accept_for_addr(address);
            if (security_accept) {
                transport_stack_mark_pairing_auth_attempted();
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
            bool security_accept = false;

            hci_event_user_passkey_request_get_bd_addr(packet, address);
            security_accept = transport_stack_security_accept_for_addr(address);
            if (security_accept) {
                transport_stack_mark_pairing_auth_attempted();
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
            if (transport_stack_security_accept()) {
                transport_stack_mark_pairing_auth_attempted();
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
                transport_stack_try_start_discovery();
            }
            break;
        case HCI_EVENT_INQUIRY_RESULT:
            transport_stack_handle_hci_inquiry_result(packet, false);
            break;
        case HCI_EVENT_INQUIRY_RESULT_WITH_RSSI:
            transport_stack_handle_hci_inquiry_result(packet, true);
            break;
        case HCI_EVENT_INQUIRY_COMPLETE:
            g_btstack_inquiry_active = false;
            break;
        case GAP_EVENT_ADVERTISING_REPORT:
            transport_stack_handle_gap_advertising_report(packet);
            break;
        case HCI_EVENT_META_GAP:
            if (hci_event_gap_meta_get_subevent_code(packet)
                == GAP_SUBEVENT_LE_CONNECTION_COMPLETE) {
                transport_stack_handle_le_connection_complete(packet);
            }
            break;
        case GAP_EVENT_DEDICATED_BONDING_COMPLETED: {
            const uint8_t bonding_status = gap_event_dedicated_bonding_completed_get_status(packet);
            bd_addr_t bonded_addr = {0};

            gap_event_dedicated_bonding_completed_get_address(packet, bonded_addr);

            transport_stack_restore_default_ssp_auth_requirement();
            g_btstack_connect_pending = false;
            g_btstack_connect_command_issued = false;
            g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
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
                transport_stack_clear_pairing_failure_phase();
                g_btstack_pairing_attempt_consumed = false;
                transport_stack_schedule_candidate(
                    bonded_addr,
                    BD_ADDR_TYPE_ACL,
                    TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
                    false
                );
            } else {
                transport_stack_handle_connect_failure(bonding_status);
            }
        } break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            transport_stack_handle_disconnect(packet);
            break;
        case HCI_EVENT_GATTSERVICE_META:
            transport_stack_handle_gattservice_meta(packet);
            break;
        case HCI_EVENT_HID_META:
            switch (hci_event_hid_meta_get_subevent_code(packet)) {
                case HID_SUBEVENT_INCOMING_CONNECTION: {
                    bd_addr_t incoming_addr = {0};
                    link_key_t link_key = {0};
                    link_key_type_t link_key_type = INVALID_LINK_KEY;
                    bool accept = false;

                    hid_subevent_incoming_connection_get_address(packet, incoming_addr);

                    /*
                     * Accept only while user-initiated pairing is active or
                     * from an already-bonded peer (which covers reconnects
                     * regardless of app-level backoff). A pending stack-side
                     * reconnect window must NOT open the door: Classic devices
                     * reconnect themselves and are bonded, so anything
                     * unbonded arriving during that window is unrelated.
                     */
                    if (g_btstack_pairing_active
                        && !g_btstack_connect_pending
                        && transport_stack_connection_capacity_available()) {
                        accept = true;
                    } else if (!g_btstack_connect_pending
                        && transport_stack_connection_capacity_available()
                        && gap_get_link_key_for_bd_addr(incoming_addr, link_key, &link_key_type)) {
                        accept = true;
                    }

                    /*
                     * No arbitration with our own outgoing attempts is needed
                     * here: the relay never pages Classic devices to reconnect
                     * them (they reconnect themselves -- see the reconnect
                     * scheduler), so an incoming page cannot collide with an
                     * outgoing one for the same address.
                     */
                    if (accept) {
                        (void)hid_host_accept_connection(
                            hid_subevent_incoming_connection_get_hid_cid(packet),
                            HID_PROTOCOL_MODE_REPORT
                        );
                    } else {
                        (void)hid_host_decline_connection(
                            hid_subevent_incoming_connection_get_hid_cid(packet)
                        );
                    }
                } break;
                case HID_SUBEVENT_CONNECTION_OPENED: {
                    const uint8_t status = hid_subevent_connection_opened_get_status(packet);

                    if (status == ERROR_CODE_SUCCESS) {
                        bd_addr_t opened_addr = {0};

                        transport_stack_handle_connect_success();
                        transport_stack_emit_classic_open_event(packet);
                        (void)hid_host_send_get_protocol(
                            hid_subevent_connection_opened_get_hid_cid(packet)
                        );
                        /*
                         * The keyboard's USB VID/PID (for Apple-keyboard
                         * recognition) is read with a Device ID SDP query, but
                         * deferred until DESCRIPTOR_AVAILABLE so it cannot
                         * contend with BTstack's own report-descriptor fetch on
                         * the shared SDP client (see the deferred-query state).
                         */
                        hid_subevent_connection_opened_get_bd_addr(packet, opened_addr);
                        (void)memcpy(
                            g_btstack_device_id_deferred_addr,
                            opened_addr,
                            sizeof(g_btstack_device_id_deferred_addr)
                        );
                        g_btstack_device_id_deferred_hid_cid =
                            hid_subevent_connection_opened_get_hid_cid(packet);
                        g_btstack_device_id_query_deferred = true;
                    } else {
                        transport_stack_handle_connect_failure(status);
                    }
                } break;
                case HID_SUBEVENT_DESCRIPTOR_AVAILABLE: {
                    const uint16_t descriptor_hid_cid =
                        hid_subevent_descriptor_available_get_hid_cid(packet);

                    transport_stack_emit_classic_descriptor_event(packet);
                    /*
                     * BTstack's descriptor SDP transaction is finished for this
                     * connection (whatever its status), so the deferred Device ID
                     * query can run without starving the descriptor fetch.
                     */
                    if (g_btstack_device_id_query_deferred
                        && (descriptor_hid_cid == g_btstack_device_id_deferred_hid_cid)) {
                        g_btstack_device_id_query_deferred = false;
                        g_btstack_device_id_deferred_hid_cid = 0U;
                        transport_stack_request_device_id(
                            g_btstack_device_id_deferred_addr,
                            descriptor_hid_cid
                        );
                    }
                    /*
                     * If this device's descriptor bytes have never been served to
                     * the host (no cache entry), the host may have enumerated the
                     * interface while the descriptor was still in flight and read
                     * nothing. Re-enumerate once so it reads the real bytes; on a
                     * reconnect of a known device the cache is already valid and
                     * no re-enumeration happens.
                     */
                    {
                        uint8_t iface = 0U;

                        for (iface = 0U; iface < HID_TRANSPORT_MAX_INTERFACE; iface++) {
                            hid_transport_usb_interface_plan_t plan = {0};

                            if (!hid_transport_runtime_usb_interface_plan_get(
                                    &g_transport_runtime,
                                    iface,
                                    &plan
                                )
                                || (plan.hid_cid != descriptor_hid_cid)) {
                                continue;
                            }
                            if (!g_btstack_descriptor_cache[iface].valid
                                || (memcmp(
                                        g_btstack_descriptor_cache[iface].device_id.bytes,
                                        plan.device_id.bytes,
                                        sizeof(plan.device_id.bytes)
                                    )
                                    != 0)) {
                                usb_runtime_request_reenumeration();
                            }
                            break;
                        }
                    }
                } break;
                case HID_SUBEVENT_GET_PROTOCOL_RESPONSE:
                    if (hid_subevent_get_protocol_response_get_handshake_status(packet)
                        == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        transport_stack_emit_bt_protocol_event(
                            hid_subevent_get_protocol_response_get_hid_cid(packet),
                            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                            hid_subevent_get_protocol_response_get_protocol_mode(packet)
                        );
                    }
                    break;
                case HID_SUBEVENT_SET_PROTOCOL_RESPONSE:
                    if (hid_subevent_set_protocol_response_get_handshake_status(packet)
                        == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL) {
                        transport_stack_emit_bt_protocol_event(
                            hid_subevent_set_protocol_response_get_hid_cid(packet),
                            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                            hid_subevent_set_protocol_response_get_protocol_mode(packet)
                        );
                    }
                    break;
                case HID_SUBEVENT_SET_REPORT_RESPONSE: {
                    transport_stack_classic_session_t * session =
                        transport_stack_classic_session_by_hid_cid(
                            hid_subevent_set_report_response_get_hid_cid(packet)
                        );

                    if ((session != NULL)
                        && session->mt_enable_pending
                        && (hid_subevent_set_report_response_get_handshake_status(packet)
                            == HID_HANDSHAKE_PARAM_TYPE_SUCCESSFUL)) {
                        session->mt_enable_pending = false;
                    }
                } break;
                case HID_SUBEVENT_CONNECTION_CLOSED:
                    transport_stack_emit_classic_close_event(packet);
                    transport_stack_try_start_discovery();
                    break;
                case HID_SUBEVENT_REPORT:
                    transport_stack_emit_classic_report_event(packet);
                    break;
                default:
                    break;
            }
            break;
        default:
            transport_stack_handle_sm_event(packet);
            break;
    }
}

#endif

bool transport_stack_init(void) {
    hid_transport_runtime_init(&g_transport_runtime);
    g_stack_last_connect_status = 0U;

    (void)usb_runtime_init();

#ifdef APP_HAS_BTSTACK
    const btstack_tlv_t * tlv_impl = NULL;
    void * tlv_context = NULL;

    g_btstack_available = false;
    g_btstack_hci_ready = false;
    g_btstack_resolving_list_requested = false;
    g_btstack_pairing_active = false;
    g_btstack_pairing_attempt_consumed = false;
    g_btstack_pairing_auth_attempted = false;
    g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
    (void)memset(g_btstack_presented_product_id, 0, sizeof(g_btstack_presented_product_id));
    g_btstack_device_id_query_deferred = false;
    g_btstack_device_id_deferred_hid_cid = 0U;
    g_btstack_device_id_query_hid_cid = 0U;
    (void)memset(g_btstack_descriptor_cache, 0, sizeof(g_btstack_descriptor_cache));
    g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    g_btstack_inquiry_active = false;
    g_btstack_scan_active = false;
    g_btstack_connect_pending = false;
    g_btstack_connect_command_issued = false;
    g_btstack_reconnect_pending = false;
    g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
    g_btstack_now_ms = 0U;
    g_btstack_connect_pending_since_ms = 0U;
    transport_stack_clear_reconnect_state();
    (void)memset(g_btstack_candidate_addr, 0, sizeof(g_btstack_candidate_addr));
    g_btstack_candidate_addr_type = BD_ADDR_TYPE_UNKNOWN;
    g_btstack_candidate_mitm_required = true;
    transport_stack_clear_pairing_failure_phase();
    transport_stack_reset_le_sessions();
    transport_stack_reset_classic_session();

    if (!platform_bt_port_init(&tlv_impl, &tlv_context) || (tlv_impl == NULL)) {
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
    btstack_tlv_set_instance(tlv_impl, tlv_context);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, tlv_context));
    le_device_db_tlv_configure(tlv_impl, tlv_context);
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
     *
     * MITM-required is only the boot-time default, kept for inbound
     * re-pairing from bonded keyboards. Outgoing dedicated bonding overrides
     * it per candidate (transport_stack_try_connect_candidate): a
     * NoInputNoOutput peer such as a trackpad or mouse can never produce an
     * authenticated key, and requesting MITM makes BTstack abort the pairing
     * with INSUFFICIENT_SECURITY -- the trackpad-side twin of the Magic
     * Keyboard failure above. Those candidates bond Just Works.
     */
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);
    gap_ssp_set_authentication_requirement(SSP_IO_AUTHREQ_MITM_PROTECTION_REQUIRED_GENERAL_BONDING);
    gap_ssp_set_auto_accept(0);
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    /*
     * Advertise LE Secure Connections support so SC-capable peripherals pair
     * with ECDH instead of legacy Just Works (whose TK=0 is passively
     * sniffable). With NoInputNoOutput IO the association model stays Just
     * Works either way; non-SC peers fall back to legacy pairing.
     */
    sm_set_authentication_requirements(SM_AUTHREQ_SECURE_CONNECTION | SM_AUTHREQ_BONDING);
    gap_set_scan_parameters(
        TRANSPORT_STACK_LE_SCAN_TYPE_ACTIVE,
        TRANSPORT_STACK_LE_SCAN_INTERVAL,
        TRANSPORT_STACK_LE_SCAN_WINDOW
    );
    gap_set_connection_parameters(
        TRANSPORT_STACK_LE_CONN_SCAN_INTERVAL,
        TRANSPORT_STACK_LE_CONN_SCAN_WINDOW,
        TRANSPORT_STACK_LE_CONN_INTERVAL_MIN,
        TRANSPORT_STACK_LE_CONN_INTERVAL_MAX,
        TRANSPORT_STACK_LE_CONN_LATENCY,
        TRANSPORT_STACK_LE_CONN_SUPERVISION_TIMEOUT,
        TRANSPORT_STACK_LE_CONN_CE_LENGTH_MIN,
        TRANSPORT_STACK_LE_CONN_CE_LENGTH_MAX
    );
    hids_client_init(
        g_btstack_le_hid_descriptor_storage,
        sizeof(g_btstack_le_hid_descriptor_storage)
    );
    hid_host_init(
        g_btstack_classic_hid_descriptor_storage,
        sizeof(g_btstack_classic_hid_descriptor_storage)
    );
    hid_host_register_packet_handler(&transport_stack_packet_handler);
    g_btstack_hci_event_callback_registration.callback = &transport_stack_packet_handler;
    hci_add_event_handler(&g_btstack_hci_event_callback_registration);
    g_btstack_sm_event_callback_registration.callback = &transport_stack_packet_handler;
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
#endif

    return true;
}

void transport_stack_poll(uint32_t now_ms) {
    (void)now_ms;

#ifdef APP_HAS_BTSTACK
    uint8_t le_index = 0U;

    g_btstack_now_ms = now_ms;
    transport_stack_sync_hci_ready();

    /*
     * Reconnect attempts can legitimately start at uptime 0. Treat 0ms start
     * timestamps as valid so timeout recovery still triggers.
     */
    if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
        && g_btstack_connect_pending
        && ((int32_t)(now_ms - g_btstack_connect_pending_since_ms)
            >= (int32_t)transport_stack_reconnect_connect_timeout_ms())) {
        const uint8_t timeout_status = (g_stack_last_connect_status != ERROR_CODE_SUCCESS)
            ? g_stack_last_connect_status
            : ERROR_CODE_CONNECTION_TIMEOUT;
        transport_stack_handle_connect_failure(timeout_status);
    }

    for (le_index = 0U; le_index < TRANSPORT_STACK_MAX_ACTIVE_CONNECTION; le_index++) {
        transport_stack_le_session_t * session = &g_btstack_le_session[le_index];

        if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
            && (session->con_handle != HCI_CON_HANDLE_INVALID)
            && session->security_pending
            && (session->security_pending_since_ms != 0U)
            && ((int32_t)(now_ms - session->security_pending_since_ms)
                >= (int32_t)TRANSPORT_STACK_RECONNECT_SECURITY_PENDING_TIMEOUT_MS)) {
            transport_stack_abandon_le_connect_attempt(session);
            transport_stack_handle_connect_failure(ERROR_CODE_CONNECTION_TIMEOUT);
            continue;
        }

        if ((g_btstack_reconnect_pending || g_btstack_pairing_active)
            && (session->con_handle != HCI_CON_HANDLE_INVALID)
            && !session->connected
            && (session->hids_pending_since_ms != 0U)
            && ((int32_t)(now_ms - session->hids_pending_since_ms)
                >= (int32_t)TRANSPORT_STACK_RECONNECT_HIDS_PENDING_TIMEOUT_MS)) {
            transport_stack_abandon_le_connect_attempt(session);
            transport_stack_handle_connect_failure(ERROR_CODE_COMMAND_DISALLOWED);
        }
    }

    if (g_btstack_available) {
        uint8_t session_index = 0U;

        transport_stack_try_start_discovery();
        transport_stack_try_connect_candidate();
        transport_stack_try_start_le_hids_client();
        for (session_index = 0U; session_index < TRANSPORT_STACK_MAX_USB_INTERFACE;
            session_index++) {
            transport_stack_try_send_mt_enable(&g_btstack_classic_session[session_index]);
        }
    }
#endif

    usb_runtime_poll();
}

void transport_stack_set_usb_plan(
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
        /*
         * The exported interface topology changed (a device connected, was
         * dropped after its warm grace expired, or its base descriptor changed),
         * so the host re-reads descriptors. Clear the presented-augmentation
         * signatures so recognized devices re-apply their augmented descriptors
         * against the new presentation.
         */
        (void)memset(g_btstack_presented_product_id, 0, sizeof(g_btstack_presented_product_id));
        usb_runtime_request_reenumeration();
    }
}

void transport_stack_set_pairing(
    bool pairing_active,
    uint8_t bt_link_type
) {
#ifdef APP_HAS_BTSTACK
    bool pairing_mode_changed = false;

    if (!g_btstack_available) {
        return;
    }

    if (!pairing_active) {
        bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    } else if (!transport_stack_valid_pairing_link_type(bt_link_type)) {
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
        && ((g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE)
            || (g_btstack_connect_mode == TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST))
        && !transport_stack_has_unconnected_le_session()) {
        (void)gap_connect_cancel();
    }

    if (!g_btstack_pairing_active) {
        g_btstack_pairing_attempt_consumed = false;
        g_btstack_pairing_auth_attempted = false;
        g_btstack_classic_candidate_con_handle = HCI_CON_HANDLE_INVALID;
        g_btstack_pairing_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        transport_stack_clear_pairing_failure_phase();
        transport_stack_clear_reconnect_state();
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
        g_btstack_connect_pending_since_ms = 0U;
        transport_stack_clear_unconnected_le_pending();
        transport_stack_abandon_unconnected_le_sessions();
        transport_stack_stop_discovery();
        return;
    }

    if (pairing_mode_changed) {
        g_btstack_connect_pending = false;
        g_btstack_connect_command_issued = false;
        g_btstack_connect_mode = TRANSPORT_STACK_CONNECT_MODE_NONE;
        g_btstack_connect_pending_since_ms = 0U;
        transport_stack_clear_unconnected_le_pending();
        transport_stack_abandon_unconnected_le_sessions();
        transport_stack_stop_discovery();
    }

    g_btstack_pairing_attempt_consumed = false;
    g_btstack_pairing_auth_attempted = false;
    transport_stack_clear_pairing_failure_phase();
    transport_stack_try_start_discovery();
#else
    (void)pairing_active;
    (void)bt_link_type;
#endif
}

bool transport_stack_request_reconnect(
    const pair_device_id_t * device_id,
    uint8_t bt_link_type_hint,
    uint8_t bt_addr_type_hint
) {
#ifdef APP_HAS_BTSTACK
    bd_addr_type_t preferred_addr_type = BD_ADDR_TYPE_UNKNOWN;

    if (!transport_stack_device_id_valid(device_id)) {
        if (device_id != NULL) {
            transport_stack_emit_reconnect_result(
                device_id,
                HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
                1U
            );
        }

        return false;
    }

    if (!g_btstack_available || !g_btstack_hci_ready) {
        transport_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            2U
        );
        return false;
    }

    if (g_btstack_pairing_active) {
        transport_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            3U
        );
        return false;
    }

    {
        uint16_t active_hid_cid = 0U;
        uint8_t active_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;

        if (transport_stack_find_hid_cid_for_device(
                device_id,
                &active_hid_cid,
                &active_link_type
            )) {
            transport_stack_emit_reconnect_result(
                device_id,
                HID_TRANSPORT_RECONNECT_RESULT_SUCCESS,
                ERROR_CODE_SUCCESS
            );
            return true;
        }
    }

    if (!transport_stack_connection_capacity_available()
        || ((bt_link_type_hint == HID_TRANSPORT_BT_LINK_TYPE_LE)
            && !transport_stack_le_session_capacity_available())) {
        transport_stack_emit_reconnect_result(
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

        transport_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            4U
        );
        return false;
    }

    preferred_addr_type = transport_stack_map_bt_addr_type_to_stack(bt_addr_type_hint);

    g_stack_last_connect_status = ERROR_CODE_SUCCESS;
    transport_stack_clear_reconnect_state();
    (void)memcpy(g_btstack_candidate_addr, device_id->bytes, sizeof(g_btstack_candidate_addr));

    if (bt_link_type_hint == HID_TRANSPORT_BT_LINK_TYPE_LE) {
        /*
         * Prefer last-known LE path first. If that address is stale after a
         * long offline window, try LE whitelist immediately before spending
         * time on lower-probability fallbacks.
         */
        if (preferred_addr_type == BD_ADDR_TYPE_LE_RANDOM_IDENTITY) {
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
        } else if (preferred_addr_type == BD_ADDR_TYPE_LE_RANDOM) {
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_RANDOM
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC
            );
        } else if (preferred_addr_type == BD_ADDR_TYPE_LE_PUBLIC_IDENTITY) {
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_PUBLIC_IDENTITY
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_RANDOM_IDENTITY
            );
        } else {
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
                BD_ADDR_TYPE_LE_PUBLIC
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE_WHITELIST,
                BD_ADDR_TYPE_LE_PUBLIC
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
                BD_ADDR_TYPE_ACL
            );
            (void)transport_stack_reconnect_add_attempt(
                TRANSPORT_STACK_CONNECT_MODE_LE,
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
        (void)transport_stack_reconnect_add_attempt(
            TRANSPORT_STACK_CONNECT_MODE_CLASSIC,
            BD_ADDR_TYPE_ACL
        );
    }

    g_btstack_reconnect_pending = true;
    if (!transport_stack_reconnect_apply_attempt(0U)) {
        transport_stack_clear_reconnect_state();
        transport_stack_emit_reconnect_result(
            device_id,
            HID_TRANSPORT_RECONNECT_RESULT_STACK_REJECTED,
            5U
        );
        return false;
    }

    transport_stack_try_connect_candidate();
    return true;
#else
    (void)device_id;
    (void)bt_link_type_hint;
    (void)bt_addr_type_hint;
    return false;
#endif
}

bool transport_stack_forget_device(const pair_device_id_t * device_id) {
#ifdef APP_HAS_BTSTACK
    bd_addr_t device_addr = {0};
    uint16_t hid_cid = 0U;
    uint8_t bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    int entry_index = 0;

    if (!g_btstack_available
        || !transport_stack_device_id_valid(device_id)
        || !g_btstack_hci_ready) {
        return false;
    }

    transport_stack_copy_addr_from_device_id(device_addr, device_id);

    if (transport_stack_find_hid_cid_for_device(device_id, &hid_cid, &bt_link_type)) {
        if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) {
            hid_host_disconnect(hid_cid);
        } else if (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE) {
            (void)hids_client_disconnect(hid_cid);
        }
    }

    {
        uint8_t cache_index = 0U;

        for (cache_index = 0U; cache_index < HID_TRANSPORT_MAX_INTERFACE; cache_index++) {
            if (g_btstack_descriptor_cache[cache_index].valid
                && (memcmp(
                        g_btstack_descriptor_cache[cache_index].device_id.bytes,
                        device_id->bytes,
                        sizeof(device_id->bytes)
                    )
                    == 0)) {
                (void)memset(
                    &g_btstack_descriptor_cache[cache_index],
                    0,
                    sizeof(g_btstack_descriptor_cache[cache_index])
                );
            }
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

uint8_t transport_stack_usb_interface_count(void) {
    return hid_transport_runtime_usb_interface_count(&g_transport_runtime);
}

static void transport_stack_descriptor_cache_store(
    uint8_t interface_number,
    const pair_device_id_t * device_id,
    const uint8_t * bytes,
    uint16_t len
) {
    transport_stack_descriptor_cache_t * slot = NULL;

    if ((interface_number >= HID_TRANSPORT_MAX_INTERFACE)
        || !transport_stack_device_id_valid(device_id)
        || (bytes == NULL)
        || (len == 0U)
        || (len > (uint16_t)sizeof(g_btstack_descriptor_cache[0].bytes))) {
        return;
    }

    slot = &g_btstack_descriptor_cache[interface_number];
    if (slot->valid
        && (slot->len == len)
        && (memcmp(slot->device_id.bytes, device_id->bytes, sizeof(slot->device_id.bytes)) == 0)
        && (memcmp(slot->bytes, bytes, len) == 0)) {
        return;
    }

    slot->device_id = *device_id;
    (void)memcpy(slot->bytes, bytes, len);
    slot->len = len;
    slot->valid = true;
}

const uint8_t * transport_stack_usb_report_descriptor(
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

#ifdef APP_HAS_BTSTACK
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
                    TRANSPORT_STACK_LE_HIDS_SERVICE_INDEX
                );
                descriptor_len = hids_client_descriptor_storage_get_descriptor_len(
                    hid_cid,
                    TRANSPORT_STACK_LE_HIDS_SERVICE_INDEX
                );
            }

            if ((descriptor != NULL) && (descriptor_len > 0U)) {
                /*
                 * For recognized Apple devices, present the device's own
                 * descriptor augmented with relay-authored collections: an
                 * Apple-vendor/consumer collection for the keyboard top-row
                 * remap, or the synthesized mouse collection the trackpad
                 * gesture engine emits into (the native BT descriptors
                 * declare none of those usages).
                 */
                uint16_t usb_vendor_id = 0U;
                uint16_t usb_product_id = 0U;
                const bool usb_id_valid = transport_stack_classic_session_usb_id(
                    hid_cid,
                    &usb_vendor_id,
                    &usb_product_id
                );

                if ((bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC) && usb_id_valid) {
                    static uint8_t aug_descriptor[1024];
                    uint16_t aug_len = 0U;

                    if (apple_keyboard_is_supported(usb_vendor_id, usb_product_id)) {
                        aug_len = apple_keyboard_augment_descriptor(
                            usb_product_id,
                            descriptor,
                            descriptor_len,
                            aug_descriptor,
                            (uint16_t)sizeof(aug_descriptor)
                        );
                    } else if (apple_trackpad_is_supported(usb_vendor_id, usb_product_id)) {
                        aug_len = apple_trackpad_augment_descriptor(
                            usb_product_id,
                            descriptor,
                            descriptor_len,
                            aug_descriptor,
                            (uint16_t)sizeof(aug_descriptor)
                        );
                    }

                    if (aug_len > 0U) {
                        transport_stack_descriptor_cache_store(
                            interface_number,
                            &interface_plan.device_id,
                            aug_descriptor,
                            aug_len
                        );
                        if (out_len != NULL) {
                            *out_len = aug_len;
                        }
                        return aug_descriptor;
                    }
                }

                transport_stack_descriptor_cache_store(
                    interface_number,
                    &interface_plan.device_id,
                    descriptor,
                    descriptor_len
                );
                if (out_len != NULL) {
                    *out_len = descriptor_len;
                }

                return descriptor;
            }
        }
    }

    /*
     * No live descriptor (link down during a warm interface window, or the
     * descriptor SDP fetch has not landed for this connection): serve the bytes
     * last served for the same device on this interface, so the host's view of
     * the interface stays consistent instead of collapsing to an empty
     * descriptor.
     */
    if (interface_number < HID_TRANSPORT_MAX_INTERFACE) {
        const transport_stack_descriptor_cache_t * cache =
            &g_btstack_descriptor_cache[interface_number];

        if (cache->valid
            && (cache->len > 0U)
            && (memcmp(
                    cache->device_id.bytes,
                    interface_plan.device_id.bytes,
                    sizeof(cache->device_id.bytes)
                )
                == 0)) {
            if (out_len != NULL) {
                *out_len = cache->len;
            }
            return cache->bytes;
        }
    }
#endif

    if (out_len != NULL) {
        *out_len = fallback_len;
    }

    return NULL;
}

uint16_t transport_stack_usb_report_descriptor_len(uint8_t interface_number) {
    uint16_t descriptor_len = 0U;

    (void)transport_stack_usb_report_descriptor(interface_number, &descriptor_len);
    return descriptor_len;
}

uint8_t transport_stack_usb_protocol_mode(uint8_t interface_number) {
    return hid_transport_runtime_usb_protocol_mode(&g_transport_runtime, interface_number);
}

static const uint8_t * transport_stack_runtime_report_descriptor(
    uint8_t interface_number,
    uint16_t * out_len,
    void * context
) {
    (void)context;
    return transport_stack_usb_report_descriptor(interface_number, out_len);
}

bool transport_stack_take_event(hid_transport_event_t * out_event) {
    return hid_transport_runtime_take_event(&g_transport_runtime, out_event);
}

void transport_stack_ingest_usb_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
    (void)hid_transport_runtime_ingest_usb_report(
        &g_transport_runtime,
        interface_number,
        report,
        report_len,
        transport_stack_runtime_report_descriptor,
        NULL
    );
}

bool transport_stack_send_usb_report(
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
            transport_stack_runtime_report_descriptor,
            NULL,
            remapped_report,
            &remapped_report_len
        )) {
        return false;
    }

    return usb_runtime_send_in_report(interface_number, remapped_report, remapped_report_len);
}

bool transport_stack_send_bt_report(
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    const uint8_t * report,
    uint16_t report_len
) {
#ifdef APP_HAS_BTSTACK
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

bool transport_stack_state_get(transport_stack_state_t * out_state) {
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
#ifdef APP_HAS_BTSTACK
    out_state->connect_pending = g_btstack_connect_pending ? 1U : 0U;
    out_state->reconnect_pending = g_btstack_reconnect_pending ? 1U : 0U;
    out_state->connect_mode = transport_stack_diag_connect_mode(g_btstack_connect_mode);
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
