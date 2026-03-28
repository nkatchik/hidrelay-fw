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
}

static void usb_bridge_mark_descriptor_dirty_if_changed(usb_bridge_t *bridge,
                                                        const usb_bridge_interface_t *previous_slot,
                                                        uint8_t previous_count) {
    if ((bridge == NULL) || (previous_slot == NULL)) {
        return;
    }

    if ((previous_count != bridge->exported_interface_count) ||
        (memcmp(previous_slot, bridge->interface_slot, sizeof(bridge->interface_slot)) != 0)) {
        bridge->descriptor_generation = bridge->descriptor_generation + 1U;
    }
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

    usb_bridge_clear(bridge);

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

    usb_bridge_clear(bridge);

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
        slot->device_id = active_device.device_id;
        bridge->exported_interface_count = (uint8_t)(bridge->exported_interface_count + 1U);
    }

    usb_bridge_mark_descriptor_dirty_if_changed(bridge, previous_slot, previous_count);
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
