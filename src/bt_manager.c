#include "bt_manager.h"

#include <stddef.h>
#include <string.h>

enum {
    BT_MANAGER_PAIRING_TIMEOUT_MS = 60000U,
    BT_MANAGER_PROTOCOL_UNKNOWN = 0U,
};

static bool bt_manager_device_id_equal(
    const pair_device_id_t * lhs,
    const pair_device_id_t * rhs
) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static void bt_manager_refresh_state(bt_manager_t * manager) {
    if (manager == NULL) {
        return;
    }

    if (manager->state == BT_MANAGER_STATE_PAIRING) {
        return;
    }

    manager->state =
        (manager->active_count == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
}

static bool bt_manager_valid_pairing_link_type(uint8_t bt_link_type) {
    return (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_LE)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC);
}

static bool bt_manager_find_active_index_by_hid_cid(
    const bt_manager_t * manager,
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t * out_index
) {
    uint8_t index = 0U;

    if ((manager == NULL) || (out_index == NULL)) {
        return false;
    }

    for (index = 0U; index < manager->active_count; index++) {
        if ((manager->active_device[index].hid_cid == hid_cid)
            && (manager->active_device[index].bt_link_type == bt_link_type)) {
            *out_index = index;
            return true;
        }
    }

    return false;
}

static bool bt_manager_find_active_index_by_device_id(
    const bt_manager_t * manager,
    const pair_device_id_t * device_id,
    uint8_t * out_index
) {
    uint8_t index = 0U;

    if ((manager == NULL) || (device_id == NULL) || (out_index == NULL)) {
        return false;
    }

    for (index = 0U; index < manager->active_count; index++) {
        if (bt_manager_device_id_equal(&manager->active_device[index].device_id, device_id)) {
            *out_index = index;
            return true;
        }
    }

    return false;
}

static bool bt_manager_find_active_index_by_device_id_and_link(
    const bt_manager_t * manager,
    const pair_device_id_t * device_id,
    uint8_t bt_link_type,
    uint8_t * out_index
) {
    uint8_t index = 0U;

    if ((manager == NULL)
        || (device_id == NULL)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)
        || (out_index == NULL)) {
        return false;
    }

    for (index = 0U; index < manager->active_count; index++) {
        if (!bt_manager_device_id_equal(&manager->active_device[index].device_id, device_id)
            || (manager->active_device[index].bt_link_type != bt_link_type)) {
            continue;
        }

        *out_index = index;
        return true;
    }

    return false;
}

static bool bt_manager_pair_db_contains(
    const pair_db_t * pair_db,
    const pair_device_id_t * device_id
) {
    uint8_t index = 0U;

    if ((pair_db == NULL) || (device_id == NULL)) {
        return false;
    }

    return pair_db_find(pair_db, device_id, &index);
}

static bool bt_manager_pair_db_entry_for_device(
    const pair_db_t * pair_db,
    const pair_device_id_t * device_id,
    pair_db_entry_t * out_entry
) {
    uint8_t index = 0U;

    if ((pair_db == NULL) || (device_id == NULL) || (out_entry == NULL)) {
        return false;
    }

    if (!pair_db_find(pair_db, device_id, &index)) {
        return false;
    }

    return pair_db_get_entry(pair_db, index, out_entry);
}

static void bt_manager_fill_open_metadata_from_pair_db(
    const pair_db_entry_t * entry,
    uint16_t * vendor_id,
    uint16_t * product_id,
    uint16_t * report_descriptor_len,
    uint8_t * protocol_mode,
    uint8_t * bt_addr_type
) {
    if ((entry == NULL)
        || (vendor_id == NULL)
        || (product_id == NULL)
        || (report_descriptor_len == NULL)
        || (protocol_mode == NULL)
        || (bt_addr_type == NULL)) {
        return;
    }

    if ((*vendor_id == 0U) && (entry->last_vendor_id != 0U)) {
        *vendor_id = entry->last_vendor_id;
    }

    if ((*product_id == 0U) && (entry->last_product_id != 0U)) {
        *product_id = entry->last_product_id;
    }

    if ((*report_descriptor_len == 0U) && (entry->last_report_descriptor_len != 0U)) {
        *report_descriptor_len = entry->last_report_descriptor_len;
    }

    if ((*protocol_mode == HID_TRANSPORT_PROTOCOL_UNKNOWN)
        && (entry->last_protocol_mode != HID_TRANSPORT_PROTOCOL_UNKNOWN)) {
        *protocol_mode = entry->last_protocol_mode;
    }

    if ((*bt_addr_type == HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN)
        && (entry->last_bt_addr_type != HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN)) {
        *bt_addr_type = entry->last_bt_addr_type;
    }
}

static void bt_manager_remove_active_index(
    bt_manager_t * manager,
    uint8_t index
) {
    if ((manager == NULL) || (index >= manager->active_count)) {
        return;
    }

    if (index < (manager->active_count - 1U)) {
        (void)memmove(
            &manager->active_device[index],
            &manager->active_device[index + 1U],
            (size_t)(manager->active_count - index - 1U) * sizeof(manager->active_device[0])
        );
    }

    manager->active_count = (uint8_t)(manager->active_count - 1U);
    (void)memset(
        &manager->active_device[manager->active_count],
        0,
        sizeof(manager->active_device[0])
    );
}

void bt_manager_init(
    bt_manager_t * manager,
    pair_db_t * pair_db
) {
    if (manager == NULL) {
        return;
    }

    manager->pair_db = pair_db;
    manager->state = BT_MANAGER_STATE_IDLE;
    manager->pairing_started_ms = 0U;
    manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    manager->active_count = 0U;
    (void)memset(manager->active_device, 0, sizeof(manager->active_device));
}

bool bt_manager_start_pairing(
    bt_manager_t * manager,
    uint32_t now_ms,
    uint8_t bt_link_type
) {
    if ((manager == NULL)
        || (manager->pair_db == NULL)
        || !bt_manager_valid_pairing_link_type(bt_link_type)) {
        return false;
    }

    if ((manager->state == BT_MANAGER_STATE_PAIRING)
        && (manager->pairing_bt_link_type == bt_link_type)) {
        return false;
    }

    manager->state = BT_MANAGER_STATE_PAIRING;
    manager->pairing_started_ms = now_ms;
    manager->pairing_bt_link_type = bt_link_type;
    return true;
}

bool bt_manager_cancel_pair_any(bt_manager_t * manager) {
    if (manager == NULL) {
        return false;
    }

    if (manager->state != BT_MANAGER_STATE_PAIRING) {
        return false;
    }

    manager->pairing_started_ms = 0U;
    manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    manager->state =
        (manager->active_count == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
    return true;
}

bool bt_manager_remove_last(bt_manager_t * manager) {
    pair_device_id_t removed_device = {0};
    uint8_t active_index = 0U;

    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (pair_db_count(manager->pair_db) == 0U) {
        return false;
    }

    if (!pair_db_get(
            manager->pair_db,
            (uint8_t)(pair_db_count(manager->pair_db) - 1U),
            &removed_device
        )) {
        return false;
    }

    if (!pair_db_remove_last(manager->pair_db)) {
        return false;
    }

    if (bt_manager_find_active_index_by_device_id(manager, &removed_device, &active_index)) {
        bt_manager_remove_active_index(manager, active_index);
    }

    bt_manager_refresh_state(manager);
    return true;
}

bool bt_manager_remove_last_if_recent(
    bt_manager_t * manager,
    uint32_t now_ms,
    uint32_t max_age_ms
) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (!pair_db_last_within_window(manager->pair_db, now_ms, max_age_ms)) {
        return false;
    }

    return bt_manager_remove_last(manager);
}

bool bt_manager_remove_all(bt_manager_t * manager) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    pair_db_remove_all(manager->pair_db);
    (void)memset(manager->active_device, 0, sizeof(manager->active_device));
    manager->active_count = 0U;
    manager->pairing_started_ms = 0U;
    manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    manager->state = BT_MANAGER_STATE_IDLE;
    return true;
}

bool bt_manager_ingest_hid_open(
    bt_manager_t * manager,
    const pair_device_id_t * device_id,
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t bt_addr_type,
    uint16_t vendor_id,
    uint16_t product_id,
    uint16_t report_descriptor_len,
    uint32_t now_ms
) {
    bt_hid_device_t * slot = NULL;
    uint8_t existing_index = 0U;
    bool known_device = false;
    pair_db_entry_t known_entry = {0};
    uint8_t protocol_mode = BT_MANAGER_PROTOCOL_UNKNOWN;

    if ((manager == NULL) || (manager->pair_db == NULL) || (device_id == NULL)) {
        return false;
    }

    if ((hid_cid == 0U) || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)) {
        return false;
    }

    if ((bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_CLASSIC)
        && (bt_addr_type != HID_TRANSPORT_BT_ADDR_TYPE_ACL)) {
        bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    }

    known_device = bt_manager_pair_db_contains(manager->pair_db, device_id);
    if (known_device) {
        (void)bt_manager_pair_db_entry_for_device(manager->pair_db, device_id, &known_entry);
        if ((known_entry.last_bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)
            || (known_entry.last_bt_link_type == bt_link_type)) {
            bt_manager_fill_open_metadata_from_pair_db(
                &known_entry,
                &vendor_id,
                &product_id,
                &report_descriptor_len,
                &protocol_mode,
                &bt_addr_type
            );
        }
    }

    if (!known_device && (manager->state != BT_MANAGER_STATE_PAIRING)) {
        return false;
    }

    if (bt_manager_find_active_index_by_hid_cid(manager, hid_cid, bt_link_type, &existing_index)) {
        slot = &manager->active_device[existing_index];
        slot->device_id = *device_id;
        slot->bt_link_type = bt_link_type;
        slot->bt_addr_type = bt_addr_type;
        slot->vendor_id = vendor_id;
        slot->product_id = product_id;
        slot->report_descriptor_len = report_descriptor_len;
        slot->protocol_mode = protocol_mode;
        (void)pair_db_touch_session(
            manager->pair_db,
            device_id,
            now_ms,
            vendor_id,
            product_id,
            report_descriptor_len,
            slot->protocol_mode,
            slot->bt_link_type,
            slot->bt_addr_type
        );
        manager->pairing_started_ms = 0U;
        manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        manager->state = BT_MANAGER_STATE_ACTIVE;
        return true;
    }

    if (bt_manager_find_active_index_by_device_id(manager, device_id, &existing_index)) {
        slot = &manager->active_device[existing_index];
        slot->hid_cid = hid_cid;
        slot->bt_link_type = bt_link_type;
        slot->bt_addr_type = bt_addr_type;
        slot->vendor_id = vendor_id;
        slot->product_id = product_id;
        slot->report_descriptor_len = report_descriptor_len;
        slot->protocol_mode = protocol_mode;
        (void)pair_db_touch_session(
            manager->pair_db,
            device_id,
            now_ms,
            vendor_id,
            product_id,
            report_descriptor_len,
            slot->protocol_mode,
            slot->bt_link_type,
            slot->bt_addr_type
        );
        manager->pairing_started_ms = 0U;
        manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        manager->state = BT_MANAGER_STATE_ACTIVE;
        return true;
    }

    if (manager->active_count >= BT_MANAGER_MAX_ACTIVE_DEVICE) {
        manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        manager->state = BT_MANAGER_STATE_ERROR;
        return false;
    }

    if (!known_device && !pair_db_add(manager->pair_db, device_id, now_ms)) {
        manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        manager->state = BT_MANAGER_STATE_ERROR;
        return false;
    }

    slot = &manager->active_device[manager->active_count];
    slot->device_id = *device_id;
    slot->hid_cid = hid_cid;
    slot->bt_link_type = bt_link_type;
    slot->bt_addr_type = bt_addr_type;
    slot->vendor_id = vendor_id;
    slot->product_id = product_id;
    slot->report_descriptor_len = report_descriptor_len;
    slot->protocol_mode = protocol_mode;

    manager->active_count = (uint8_t)(manager->active_count + 1U);
    (void)pair_db_touch_session(
        manager->pair_db,
        device_id,
        now_ms,
        vendor_id,
        product_id,
        report_descriptor_len,
        slot->protocol_mode,
        slot->bt_link_type,
        slot->bt_addr_type
    );
    manager->pairing_started_ms = 0U;
    manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    manager->state = BT_MANAGER_STATE_ACTIVE;
    return true;
}

bool bt_manager_ingest_hid_close(
    bt_manager_t * manager,
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint32_t now_ms
) {
    uint8_t index = 0U;
    bt_hid_device_t closed_device = {0};

    if ((manager == NULL)
        || (manager->pair_db == NULL)
        || (hid_cid == 0U)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)) {
        return false;
    }

    if (!bt_manager_find_active_index_by_hid_cid(manager, hid_cid, bt_link_type, &index)) {
        return false;
    }

    closed_device = manager->active_device[index];
    (void)pair_db_touch_session(
        manager->pair_db,
        &closed_device.device_id,
        now_ms,
        closed_device.vendor_id,
        closed_device.product_id,
        closed_device.report_descriptor_len,
        closed_device.protocol_mode,
        closed_device.bt_link_type,
        closed_device.bt_addr_type
    );
    bt_manager_remove_active_index(manager, index);
    bt_manager_refresh_state(manager);
    return true;
}

bool bt_manager_ingest_hid_close_device(
    bt_manager_t * manager,
    const pair_device_id_t * device_id,
    uint8_t bt_link_type,
    uint32_t now_ms
) {
    uint8_t index = 0U;
    bt_hid_device_t closed_device = {0};
    bool found = false;

    if ((manager == NULL) || (manager->pair_db == NULL) || (device_id == NULL)) {
        return false;
    }

    if ((bt_link_type != HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)
        && bt_manager_find_active_index_by_device_id_and_link(
            manager,
            device_id,
            bt_link_type,
            &index
        )) {
        found = true;
    } else if (bt_manager_find_active_index_by_device_id(manager, device_id, &index)) {
        /*
         * Metadata can be stale around LE reconnect/close races. Fall back to
         * device-id-only close to avoid leaving ghost active sessions that
         * block reconnect scheduling.
         */
        found = true;
    }

    if (!found) {
        return false;
    }

    closed_device = manager->active_device[index];
    (void)pair_db_touch_session(
        manager->pair_db,
        &closed_device.device_id,
        now_ms,
        closed_device.vendor_id,
        closed_device.product_id,
        closed_device.report_descriptor_len,
        closed_device.protocol_mode,
        closed_device.bt_link_type,
        closed_device.bt_addr_type
    );
    bt_manager_remove_active_index(manager, index);
    bt_manager_refresh_state(manager);
    return true;
}

bool bt_manager_ingest_hid_descriptor(
    bt_manager_t * manager,
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint16_t report_descriptor_len,
    uint32_t now_ms
) {
    uint8_t index = 0U;
    bt_hid_device_t * device = NULL;

    if ((manager == NULL)
        || (manager->pair_db == NULL)
        || (hid_cid == 0U)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)) {
        return false;
    }

    if (!bt_manager_find_active_index_by_hid_cid(manager, hid_cid, bt_link_type, &index)) {
        return false;
    }

    device = &manager->active_device[index];
    device->report_descriptor_len = report_descriptor_len;
    return pair_db_touch_session(
        manager->pair_db,
        &device->device_id,
        now_ms,
        device->vendor_id,
        device->product_id,
        device->report_descriptor_len,
        device->protocol_mode,
        device->bt_link_type,
        device->bt_addr_type
    );
}

bool bt_manager_ingest_hid_protocol(
    bt_manager_t * manager,
    uint16_t hid_cid,
    uint8_t bt_link_type,
    uint8_t protocol_mode,
    uint32_t now_ms
) {
    uint8_t index = 0U;
    bt_hid_device_t * device = NULL;

    if ((manager == NULL)
        || (manager->pair_db == NULL)
        || (hid_cid == 0U)
        || (bt_link_type == HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN)) {
        return false;
    }

    if (!bt_manager_find_active_index_by_hid_cid(manager, hid_cid, bt_link_type, &index)) {
        return false;
    }

    device = &manager->active_device[index];
    device->protocol_mode = protocol_mode;
    return pair_db_touch_session(
        manager->pair_db,
        &device->device_id,
        now_ms,
        device->vendor_id,
        device->product_id,
        device->report_descriptor_len,
        device->protocol_mode,
        device->bt_link_type,
        device->bt_addr_type
    );
}

void bt_manager_tick(
    bt_manager_t * manager,
    uint32_t now_ms
) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return;
    }

    if (manager->state != BT_MANAGER_STATE_PAIRING) {
        return;
    }

    if ((now_ms - manager->pairing_started_ms) >= BT_MANAGER_PAIRING_TIMEOUT_MS) {
        manager->pairing_started_ms = 0U;
        manager->pairing_bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
        manager->state =
            (manager->active_count == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
    }
}

bt_manager_state_t bt_manager_state(const bt_manager_t * manager) {
    if (manager == NULL) {
        return BT_MANAGER_STATE_ERROR;
    }

    return manager->state;
}

uint8_t bt_manager_pairing_link_type(const bt_manager_t * manager) {
    if ((manager == NULL) || (manager->state != BT_MANAGER_STATE_PAIRING)) {
        return HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN;
    }

    return manager->pairing_bt_link_type;
}

uint8_t bt_manager_active_count(const bt_manager_t * manager) {
    if (manager == NULL) {
        return 0U;
    }

    return manager->active_count;
}

bool bt_manager_active_get(
    const bt_manager_t * manager,
    uint8_t index,
    bt_hid_device_t * out_device
) {
    if ((manager == NULL) || (out_device == NULL)) {
        return false;
    }

    if (index >= manager->active_count) {
        return false;
    }

    *out_device = manager->active_device[index];
    return true;
}
