#include "usb_bridge.h"

#include <stddef.h>
#include <string.h>

enum {
    USB_BRIDGE_ENDPOINT_BASE_OUT = 0x01U,
    USB_BRIDGE_ENDPOINT_BASE_IN = 0x81U
};

static void usb_bridge_clear(usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    (void)memset(bridge->interface_slot, 0, sizeof(bridge->interface_slot));
    bridge->exported_interface_count = 0U;
    (void)memset(bridge->usb_tx_queue, 0, sizeof(bridge->usb_tx_queue));
    (void)memset(bridge->bt_tx_queue, 0, sizeof(bridge->bt_tx_queue));
    bridge->usb_tx_queue_head = 0U;
    bridge->usb_tx_queue_tail = 0U;
    bridge->usb_tx_queue_count = 0U;
    bridge->bt_tx_queue_head = 0U;
    bridge->bt_tx_queue_tail = 0U;
    bridge->bt_tx_queue_count = 0U;
    (void)memset(&bridge->telemetry, 0, sizeof(bridge->telemetry));
}

static void usb_bridge_clear_topology(usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    (void)memset(bridge->interface_slot, 0, sizeof(bridge->interface_slot));
    bridge->exported_interface_count = 0U;
}

static void usb_bridge_clear_report_queues(usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    (void)memset(bridge->usb_tx_queue, 0, sizeof(bridge->usb_tx_queue));
    (void)memset(bridge->bt_tx_queue, 0, sizeof(bridge->bt_tx_queue));
    bridge->usb_tx_queue_head = 0U;
    bridge->usb_tx_queue_tail = 0U;
    bridge->usb_tx_queue_count = 0U;
    bridge->bt_tx_queue_head = 0U;
    bridge->bt_tx_queue_tail = 0U;
    bridge->bt_tx_queue_count = 0U;
    bridge->telemetry.usb_tx_depth = 0U;
    bridge->telemetry.bt_tx_depth = 0U;
}

static bool usb_bridge_topology_changed(const usb_bridge_t *bridge,
                                        const usb_bridge_interface_t *previous_slot,
                                        uint8_t previous_count) {
    if ((bridge == NULL) || (previous_slot == NULL)) {
        return false;
    }

    if (previous_count != bridge->exported_interface_count) {
        return true;
    }

    return memcmp(previous_slot, bridge->interface_slot, sizeof(bridge->interface_slot)) != 0;
}

static void usb_bridge_mark_descriptor_dirty_if_changed(usb_bridge_t *bridge,
                                                        const usb_bridge_interface_t *previous_slot,
                                                        uint8_t previous_count) {
    if ((bridge == NULL) || (previous_slot == NULL)) {
        return;
    }

    if (usb_bridge_topology_changed(bridge, previous_slot, previous_count)) {
        bridge->descriptor_generation = bridge->descriptor_generation + 1U;
        usb_bridge_clear_report_queues(bridge);
    }
}

static bool usb_bridge_find_interface_for_hid_cid(const usb_bridge_t *bridge, uint16_t hid_cid, uint8_t *out_interface) {
    uint8_t index = 0U;

    if ((bridge == NULL) || (out_interface == NULL) || (hid_cid == 0U)) {
        return false;
    }

    for (index = 0U; index < bridge->exported_interface_count; index++) {
        if (bridge->interface_slot[index].used && (bridge->interface_slot[index].hid_cid == hid_cid)) {
            *out_interface = bridge->interface_slot[index].interface_number;
            return true;
        }
    }

    return false;
}

static bool usb_bridge_find_hid_cid_for_interface(const usb_bridge_t *bridge,
                                                  uint8_t interface_number,
                                                  uint16_t *out_hid_cid,
                                                  uint8_t *out_protocol_mode) {
    if ((bridge == NULL) || (out_hid_cid == NULL) || (out_protocol_mode == NULL)) {
        return false;
    }

    if (interface_number >= bridge->exported_interface_count) {
        return false;
    }

    if (!bridge->interface_slot[interface_number].used || (bridge->interface_slot[interface_number].hid_cid == 0U)) {
        return false;
    }

    *out_hid_cid = bridge->interface_slot[interface_number].hid_cid;
    *out_protocol_mode = bridge->interface_slot[interface_number].protocol_mode;
    return true;
}

static bool usb_bridge_push_usb_tx(usb_bridge_t *bridge, const hid_transport_usb_tx_t *tx) {
    if ((bridge == NULL) || (tx == NULL) || !tx->valid) {
        return false;
    }

    if (bridge->usb_tx_queue_count >= USB_BRIDGE_MAX_INTERFACE) {
        bridge->usb_tx_queue[bridge->usb_tx_queue_head].valid = false;
        bridge->usb_tx_queue_head = (uint8_t)((bridge->usb_tx_queue_head + 1U) % USB_BRIDGE_MAX_INTERFACE);
        bridge->usb_tx_queue_count = (uint8_t)(bridge->usb_tx_queue_count - 1U);
        bridge->telemetry.usb_tx_dropped = bridge->telemetry.usb_tx_dropped + 1U;
    }

    bridge->usb_tx_queue[bridge->usb_tx_queue_tail] = *tx;
    bridge->usb_tx_queue_tail = (uint8_t)((bridge->usb_tx_queue_tail + 1U) % USB_BRIDGE_MAX_INTERFACE);
    bridge->usb_tx_queue_count = (uint8_t)(bridge->usb_tx_queue_count + 1U);
    bridge->telemetry.usb_tx_depth = bridge->usb_tx_queue_count;
    bridge->telemetry.usb_tx_enqueued = bridge->telemetry.usb_tx_enqueued + 1U;

    if (bridge->telemetry.usb_tx_depth > bridge->telemetry.usb_tx_high_watermark) {
        bridge->telemetry.usb_tx_high_watermark = bridge->telemetry.usb_tx_depth;
    }

    return true;
}

static bool usb_bridge_push_bt_tx(usb_bridge_t *bridge, const hid_transport_bt_tx_t *tx) {
    if ((bridge == NULL) || (tx == NULL) || !tx->valid) {
        return false;
    }

    if (bridge->bt_tx_queue_count >= USB_BRIDGE_MAX_INTERFACE) {
        bridge->bt_tx_queue[bridge->bt_tx_queue_head].valid = false;
        bridge->bt_tx_queue_head = (uint8_t)((bridge->bt_tx_queue_head + 1U) % USB_BRIDGE_MAX_INTERFACE);
        bridge->bt_tx_queue_count = (uint8_t)(bridge->bt_tx_queue_count - 1U);
        bridge->telemetry.bt_tx_dropped = bridge->telemetry.bt_tx_dropped + 1U;
    }

    bridge->bt_tx_queue[bridge->bt_tx_queue_tail] = *tx;
    bridge->bt_tx_queue_tail = (uint8_t)((bridge->bt_tx_queue_tail + 1U) % USB_BRIDGE_MAX_INTERFACE);
    bridge->bt_tx_queue_count = (uint8_t)(bridge->bt_tx_queue_count + 1U);
    bridge->telemetry.bt_tx_depth = bridge->bt_tx_queue_count;
    bridge->telemetry.bt_tx_enqueued = bridge->telemetry.bt_tx_enqueued + 1U;

    if (bridge->telemetry.bt_tx_depth > bridge->telemetry.bt_tx_high_watermark) {
        bridge->telemetry.bt_tx_high_watermark = bridge->telemetry.bt_tx_depth;
    }

    return true;
}

static bool usb_bridge_pop_usb_tx(usb_bridge_t *bridge, hid_transport_usb_tx_t *out_tx) {
    if ((bridge == NULL) || (out_tx == NULL) || (bridge->usb_tx_queue_count == 0U)) {
        return false;
    }

    *out_tx = bridge->usb_tx_queue[bridge->usb_tx_queue_head];
    bridge->usb_tx_queue[bridge->usb_tx_queue_head].valid = false;
    bridge->usb_tx_queue_head = (uint8_t)((bridge->usb_tx_queue_head + 1U) % USB_BRIDGE_MAX_INTERFACE);
    bridge->usb_tx_queue_count = (uint8_t)(bridge->usb_tx_queue_count - 1U);
    bridge->telemetry.usb_tx_depth = bridge->usb_tx_queue_count;
    bridge->telemetry.usb_tx_dequeued = bridge->telemetry.usb_tx_dequeued + 1U;
    return true;
}

static bool usb_bridge_pop_bt_tx(usb_bridge_t *bridge, hid_transport_bt_tx_t *out_tx) {
    if ((bridge == NULL) || (out_tx == NULL) || (bridge->bt_tx_queue_count == 0U)) {
        return false;
    }

    *out_tx = bridge->bt_tx_queue[bridge->bt_tx_queue_head];
    bridge->bt_tx_queue[bridge->bt_tx_queue_head].valid = false;
    bridge->bt_tx_queue_head = (uint8_t)((bridge->bt_tx_queue_head + 1U) % USB_BRIDGE_MAX_INTERFACE);
    bridge->bt_tx_queue_count = (uint8_t)(bridge->bt_tx_queue_count - 1U);
    bridge->telemetry.bt_tx_depth = bridge->bt_tx_queue_count;
    bridge->telemetry.bt_tx_dequeued = bridge->telemetry.bt_tx_dequeued + 1U;
    return true;
}

static bool usb_bridge_report_valid(const uint8_t *report, uint16_t report_len) {
    if (report_len > HID_TRANSPORT_REPORT_MAX_LEN) {
        return false;
    }

    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    return true;
}

void usb_bridge_init(usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    usb_bridge_clear(bridge);
    bridge->descriptor_generation = 1U;
}

void usb_bridge_sync_from_pair_db(usb_bridge_t *bridge, const pair_db_t *pair_db) {
    usb_bridge_interface_t previous_slot[USB_BRIDGE_MAX_INTERFACE] = {0};
    uint8_t previous_count = 0U;
    uint8_t interface_count = 0U;

    if ((bridge == NULL) || (pair_db == NULL)) {
        return;
    }

    (void)memcpy(previous_slot, bridge->interface_slot, sizeof(previous_slot));
    previous_count = bridge->exported_interface_count;

    usb_bridge_clear_topology(bridge);

    for (interface_count = 0U;
         (interface_count < pair_db_count(pair_db)) && (interface_count < USB_BRIDGE_MAX_INTERFACE);
         interface_count++) {
        usb_bridge_interface_t *slot = &bridge->interface_slot[interface_count];
        pair_device_id_t device_id = {0};

        if (!pair_db_get(pair_db, interface_count, &device_id)) {
            continue;
        }

        slot->used = true;
        slot->interface_number = interface_count;
        slot->endpoint_out = (uint8_t)(USB_BRIDGE_ENDPOINT_BASE_OUT + interface_count);
        slot->endpoint_in = (uint8_t)(USB_BRIDGE_ENDPOINT_BASE_IN + interface_count);
        slot->hid_cid = 0U;
        slot->report_descriptor_len = 0U;
        slot->protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
        slot->device_id = device_id;
        bridge->exported_interface_count = (uint8_t)(bridge->exported_interface_count + 1U);
    }

    usb_bridge_mark_descriptor_dirty_if_changed(bridge, previous_slot, previous_count);
}

void usb_bridge_sync_from_bt_manager(usb_bridge_t *bridge, const bt_manager_t *manager) {
    usb_bridge_interface_t previous_slot[USB_BRIDGE_MAX_INTERFACE] = {0};
    uint8_t previous_count = 0U;
    uint8_t manager_count = 0U;
    uint8_t index = 0U;

    if ((bridge == NULL) || (manager == NULL)) {
        return;
    }

    (void)memcpy(previous_slot, bridge->interface_slot, sizeof(previous_slot));
    previous_count = bridge->exported_interface_count;
    manager_count = bt_manager_active_count(manager);

    usb_bridge_clear_topology(bridge);

    for (index = 0U; (index < manager_count) && (index < USB_BRIDGE_MAX_INTERFACE); index++) {
        usb_bridge_interface_t *slot = &bridge->interface_slot[index];
        bt_hid_device_t active_device = {0};

        if (!bt_manager_active_get(manager, index, &active_device)) {
            continue;
        }

        slot->used = true;
        slot->interface_number = index;
        slot->endpoint_out = (uint8_t)(USB_BRIDGE_ENDPOINT_BASE_OUT + index);
        slot->endpoint_in = (uint8_t)(USB_BRIDGE_ENDPOINT_BASE_IN + index);
        slot->hid_cid = active_device.hid_cid;
        slot->report_descriptor_len = active_device.report_descriptor_len;
        slot->protocol_mode = active_device.protocol_mode;
        slot->device_id = active_device.device_id;
        bridge->exported_interface_count = (uint8_t)(bridge->exported_interface_count + 1U);
    }

    usb_bridge_mark_descriptor_dirty_if_changed(bridge, previous_slot, previous_count);
}

bool usb_bridge_ingest_bt_report(usb_bridge_t *bridge, uint16_t hid_cid, const uint8_t *report, uint16_t report_len) {
    hid_transport_usb_tx_t tx = {0};

    if ((bridge == NULL) || (hid_cid == 0U) || !usb_bridge_report_valid(report, report_len)) {
        return false;
    }

    if (!usb_bridge_find_interface_for_hid_cid(bridge, hid_cid, &tx.interface_number)) {
        return false;
    }

    tx.valid = true;
    tx.report_len = report_len;

    if (report_len > 0U) {
        (void)memcpy(tx.report, report, report_len);
    }

    return usb_bridge_push_usb_tx(bridge, &tx);
}

bool usb_bridge_ingest_usb_report(usb_bridge_t *bridge,
                                  uint8_t interface_number,
                                  const uint8_t *report,
                                  uint16_t report_len) {
    hid_transport_bt_tx_t tx = {0};

    if ((bridge == NULL) || !usb_bridge_report_valid(report, report_len)) {
        return false;
    }

    if (!usb_bridge_find_hid_cid_for_interface(bridge, interface_number, &tx.hid_cid, &tx.protocol_mode)) {
        return false;
    }

    tx.valid = true;
    tx.report_len = report_len;

    if (report_len > 0U) {
        (void)memcpy(tx.report, report, report_len);
    }

    return usb_bridge_push_bt_tx(bridge, &tx);
}

bool usb_bridge_take_usb_tx(usb_bridge_t *bridge, hid_transport_usb_tx_t *out_tx) {
    if (out_tx == NULL) {
        return false;
    }

    (void)memset(out_tx, 0, sizeof(*out_tx));
    return usb_bridge_pop_usb_tx(bridge, out_tx);
}

bool usb_bridge_take_bt_tx(usb_bridge_t *bridge, hid_transport_bt_tx_t *out_tx) {
    if (out_tx == NULL) {
        return false;
    }

    (void)memset(out_tx, 0, sizeof(*out_tx));
    return usb_bridge_pop_bt_tx(bridge, out_tx);
}

bool usb_bridge_telemetry_get(const usb_bridge_t *bridge, usb_bridge_telemetry_t *out_telemetry) {
    if ((bridge == NULL) || (out_telemetry == NULL)) {
        return false;
    }

    *out_telemetry = bridge->telemetry;
    return true;
}

void usb_bridge_tick(usb_bridge_t *bridge, uint32_t now_ms) {
    (void)now_ms;

    if (bridge == NULL) {
        return;
    }
}

uint8_t usb_bridge_interface_count(const usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return 0U;
    }

    return bridge->exported_interface_count;
}

uint32_t usb_bridge_descriptor_generation(const usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return 0U;
    }

    return bridge->descriptor_generation;
}

bool usb_bridge_interface_get(const usb_bridge_t *bridge, uint8_t index, usb_bridge_interface_t *out_interface) {
    if ((bridge == NULL) || (out_interface == NULL)) {
        return false;
    }

    if ((index >= bridge->exported_interface_count) || !bridge->interface_slot[index].used) {
        return false;
    }

    *out_interface = bridge->interface_slot[index];
    return true;
}
