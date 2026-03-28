#include "pair_db.h"

#include <string.h>

static bool pair_db_age_within_window(uint32_t now_ms, uint32_t then_ms, uint32_t window_ms) {
    return (now_ms - then_ms) <= window_ms;
}

void pair_db_init(pair_db_t *db) {
    if (db == NULL) {
        return;
    }

    (void)memset(db, 0, sizeof(*db));
}

bool pair_db_add(pair_db_t *db, const pair_device_id_t *device_id, uint32_t paired_at_ms) {
    if ((db == NULL) || (device_id == NULL)) {
        return false;
    }

    if (db->count >= PAIR_DB_MAX_DEVICE) {
        return false;
    }

    db->entries[db->count].device_id = *device_id;
    db->entries[db->count].paired_at_ms = paired_at_ms;
    db->count = (uint8_t)(db->count + 1U);
    return true;
}

bool pair_db_remove_last(pair_db_t *db) {
    if (db == NULL) {
        return false;
    }

    if (db->count == 0U) {
        return false;
    }

    db->count = (uint8_t)(db->count - 1U);
    (void)memset(&db->entries[db->count], 0, sizeof(db->entries[db->count]));
    return true;
}

void pair_db_remove_all(pair_db_t *db) {
    if (db == NULL) {
        return;
    }

    (void)memset(db, 0, sizeof(*db));
}

uint8_t pair_db_count(const pair_db_t *db) {
    if (db == NULL) {
        return 0U;
    }

    return db->count;
}

bool pair_db_get(const pair_db_t *db, uint8_t index, pair_device_id_t *out_device_id) {
    if ((db == NULL) || (out_device_id == NULL)) {
        return false;
    }

    if (index >= db->count) {
        return false;
    }

    *out_device_id = db->entries[index].device_id;
    return true;
}

bool pair_db_last_within_window(const pair_db_t *db, uint32_t now_ms, uint32_t window_ms) {
    uint8_t last_index = 0U;

    if (db == NULL) {
        return false;
    }

    if (db->count == 0U) {
        return false;
    }

    last_index = (uint8_t)(db->count - 1U);
    return pair_db_age_within_window(now_ms, db->entries[last_index].paired_at_ms, window_ms);
}
