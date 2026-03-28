#include "bt_manager.h"

#include <stddef.h>

enum {
    BT_MANAGER_STUB_PAIR_SUCCESS_MS = 2000U,
    BT_MANAGER_PAIRING_TIMEOUT_MS = 60000U
};

static pair_device_id_t bt_manager_make_stub_device_id(uint8_t seed) {
    pair_device_id_t id = { .bytes = { 0x02U, 0x00U, 0x00U, 0x00U, 0x00U, seed } };
    return id;
}

void bt_manager_init(bt_manager_t *manager, pair_db_t *pair_db) {
    if (manager == NULL) {
        return;
    }

    manager->pair_db = pair_db;
    manager->state = BT_MANAGER_STATE_IDLE;
    manager->pairing_started_ms = 0U;
}

bool bt_manager_start_pair_any(bt_manager_t *manager, uint32_t now_ms) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    manager->state = BT_MANAGER_STATE_PAIRING;
    manager->pairing_started_ms = now_ms;
    return true;
}

bool bt_manager_remove_last(bt_manager_t *manager) {
    if ((manager == NULL) || (manager->pair_db == NULL)) {
        return false;
    }

    if (!pair_db_remove_last(manager->pair_db)) {
        return false;
    }

    manager->state = (pair_db_count(manager->pair_db) == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
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
    manager->state = BT_MANAGER_STATE_IDLE;
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
        manager->state = (pair_db_count(manager->pair_db) == 0U) ? BT_MANAGER_STATE_IDLE : BT_MANAGER_STATE_ACTIVE;
        return;
    }

    if ((now_ms - manager->pairing_started_ms) < BT_MANAGER_STUB_PAIR_SUCCESS_MS) {
        return;
    }

    {
        const uint8_t next_slot = pair_db_count(manager->pair_db);
        const pair_device_id_t fake_device = bt_manager_make_stub_device_id(next_slot);

        if (!pair_db_add(manager->pair_db, &fake_device, now_ms)) {
            manager->state = BT_MANAGER_STATE_ERROR;
            return;
        }
    }

    manager->state = BT_MANAGER_STATE_ACTIVE;
}

bt_manager_state_t bt_manager_state(const bt_manager_t *manager) {
    if (manager == NULL) {
        return BT_MANAGER_STATE_ERROR;
    }

    return manager->state;
}
