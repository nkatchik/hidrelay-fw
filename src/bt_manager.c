#include "bt_manager.h"

#include <stddef.h>
#include <string.h>

enum {
    BT_MANAGER_PAIRING_TIMEOUT_MS = 60000U,
};

static bool bt_manager_device_id_equal(const pair_device_id_t *lhs, const pair_device_id_t *rhs) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static void bt_manager_refresh_state(bt_manager_t *manager) {
    if (manager == NULL) {
        return;
    }

    if (manager->state == BT_MANAGER_STATE_PAIRING) {
        return;
    }

    manager->state = (manager->active_count == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
}

static bool bt_manager_find_active_index_by_hid_cid(const bt_manager_t *manager, uint16_t hid_cid, uint8_t *out_index) {
    uint8_t index = 0U;

    if ((manager == NULL) || (out_index == NULL)) {
        return false;
    }

    for (index = 0U; index < manager->active_count; index++) {
        if (manager->active_device[index].hid_cid == hid_cid) {
            *out_index = index;
            return true;
        }
    }

    return false;
}

static bool bt_manager_find_active_index_by_device_id(const bt_manager_t *manager,
                                                      const pair_device_id_t *device_id,
                                                      uint8_t *out_index) {
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

static bool bt_manager_pair_db_contains(const pair_db_t *pair_db, const pair_device_id_t *device_id) {
    uint8_t index = 0U;

    if ((pair_db == NULL) || (device_id == NULL)) {
        return false;
    }

    for (index = 0U; index < pair_db_count(pair_db); index++) {
        pair_device_id_t existing = {0};

        if (!pair_db_get(pair_db, index, &existing)) {
            continue;
        }

        if (bt_manager_device_id_equal(&existing, device_id)) {
            return true;
        }
    }

    return false;
}

static void bt_manager_remove_active_index(bt_manager_t *manager, uint8_t index) {
    if ((manager == NULL) || (index >= manager->active_count)) {
        return;
    }

    if (index < (manager->active_count - 1U)) {
        (void)memmove(&manager->active_device[index],
                      &manager->active_device[index + 1U],
                      (size_t)(manager->active_count - index - 1U) * sizeof(manager->active_device[0]));
    }

    manager->active_count = (uint8_t)(manager->active_count - 1U);
    (void)memset(&manager->active_device[manager->active_count], 0, sizeof(manager->active_device[0]));
}

void bt_manager_init(bt_manager_t *manager, pair_db_t *pair_db) {
    if (manager == NULL) {
        return;
    }

    manager->pair_db = pair_db;
    manager->state = BT_MANAGER_STATE_IDLE;
    manager->pairing_started_ms = 0U;
    manager->active_count = 0U;
    (void)memset(manager->active_device, 0, sizeof(manager->active_device));
}

bool bt_manager_start_pair_any(bt_manager_t *manager, uint32_t now_ms) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (manager->state == BT_MANAGER_STATE_PAIRING) {
        return false;
    }

    manager->state = BT_MANAGER_STATE_PAIRING;
    manager->pairing_started_ms = now_ms;
    return true;
}

bool bt_manager_remove_last(bt_manager_t *manager) {
    pair_device_id_t removed_device = {0};
    uint8_t active_index = 0U;

    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (pair_db_count(manager->pair_db) == 0U) {
        return false;
    }

    if (!pair_db_get(manager->pair_db, (uint8_t)(pair_db_count(manager->pair_db) - 1U), &removed_device)) {
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

bool bt_manager_remove_last_if_recent(bt_manager_t *manager, uint32_t now_ms, uint32_t max_age_ms) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (!pair_db_last_within_window(manager->pair_db, now_ms, max_age_ms)) {
        return false;
    }

    return bt_manager_remove_last(manager);
}

bool bt_manager_remove_all(bt_manager_t *manager) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    pair_db_remove_all(manager->pair_db);
    (void)memset(manager->active_device, 0, sizeof(manager->active_device));
    manager->active_count = 0U;
    manager->pairing_started_ms = 0U;
    manager->state = BT_MANAGER_STATE_IDLE;
    return true;
}

bool bt_manager_ingest_hid_open(bt_manager_t *manager,
                                const pair_device_id_t *device_id,
                                uint16_t hid_cid,
                                uint16_t vendor_id,
                                uint16_t product_id,
                                uint16_t report_descriptor_len,
                                uint32_t now_ms) {
    bt_hid_device_t *slot = NULL;
    uint8_t existing_index = 0U;

    if ((manager == NULL) || (manager->pair_db == NULL) || (device_id == NULL)) {
        return false;
    }

    if (hid_cid == 0U) {
        return false;
    }

    if (bt_manager_find_active_index_by_hid_cid(manager, hid_cid, &existing_index)) {
        slot = &manager->active_device[existing_index];
        slot->device_id = *device_id;
        slot->vendor_id = vendor_id;
        slot->product_id = product_id;
        slot->report_descriptor_len = report_descriptor_len;
        manager->pairing_started_ms = 0U;
        manager->state = BT_MANAGER_STATE_ACTIVE;
        return true;
    }

    if (bt_manager_find_active_index_by_device_id(manager, device_id, &existing_index)) {
        slot = &manager->active_device[existing_index];
        slot->hid_cid = hid_cid;
        slot->vendor_id = vendor_id;
        slot->product_id = product_id;
        slot->report_descriptor_len = report_descriptor_len;
        manager->pairing_started_ms = 0U;
        manager->state = BT_MANAGER_STATE_ACTIVE;
        return true;
    }

    if (manager->active_count >= BT_MANAGER_MAX_ACTIVE_DEVICE) {
        manager->state = BT_MANAGER_STATE_ERROR;
        return false;
    }

    if (!bt_manager_pair_db_contains(manager->pair_db, device_id) &&
        !pair_db_add(manager->pair_db, device_id, now_ms)) {
        manager->state = BT_MANAGER_STATE_ERROR;
        return false;
    }

    slot = &manager->active_device[manager->active_count];
    slot->device_id = *device_id;
    slot->hid_cid = hid_cid;
    slot->vendor_id = vendor_id;
    slot->product_id = product_id;
    slot->report_descriptor_len = report_descriptor_len;

    manager->active_count = (uint8_t)(manager->active_count + 1U);
    manager->pairing_started_ms = 0U;
    manager->state = BT_MANAGER_STATE_ACTIVE;
    return true;
}

bool bt_manager_ingest_hid_close(bt_manager_t *manager, uint16_t hid_cid) {
    uint8_t index = 0U;

    if ((manager == NULL) || (hid_cid == 0U)) {
        return false;
    }

    if (!bt_manager_find_active_index_by_hid_cid(manager, hid_cid, &index)) {
        return false;
    }

    bt_manager_remove_active_index(manager, index);
    bt_manager_refresh_state(manager);
    return true;
}

void bt_manager_tick(bt_manager_t *manager, uint32_t now_ms) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return;
    }

    if (manager->state != BT_MANAGER_STATE_PAIRING) {
        return;
    }

    if ((now_ms - manager->pairing_started_ms) >= BT_MANAGER_PAIRING_TIMEOUT_MS) {
        manager->pairing_started_ms = 0U;
        bt_manager_refresh_state(manager);
    }
}

bt_manager_state_t bt_manager_state(const bt_manager_t *manager) {
    if (manager == NULL) {
        return BT_MANAGER_STATE_ERROR;
    }

    return manager->state;
}

uint8_t bt_manager_active_count(const bt_manager_t *manager) {
    if (manager == NULL) {
        return 0U;
    }

    return manager->active_count;
}

bool bt_manager_active_get(const bt_manager_t *manager, uint8_t index, bt_hid_device_t *out_device) {
    if ((manager == NULL) || (out_device == NULL)) {
        return false;
    }

    if (index >= manager->active_count) {
        return false;
    }

    *out_device = manager->active_device[index];
    return true;
}
