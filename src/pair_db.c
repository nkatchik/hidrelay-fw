#include "pair_db.h"

#include <string.h>

static bool pair_db_age_within_window(uint32_t now_ms, uint32_t then_ms, uint32_t window_ms) {
    return (now_ms - then_ms) <= window_ms;
}

static bool pair_db_device_id_equal(const pair_device_id_t *lhs, const pair_device_id_t *rhs) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
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
    db->entries[db->count].last_seen_ms = paired_at_ms;
    db->entries[db->count].last_vendor_id = 0U;
    db->entries[db->count].last_product_id = 0U;
    db->entries[db->count].last_report_descriptor_len = 0U;
    db->entries[db->count].last_protocol_mode = 0U;
    db->entries[db->count].reconnect_allowed = 1U;
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

bool pair_db_get_entry(const pair_db_t *db, uint8_t index, pair_db_entry_t *out_entry) {
    if ((db == NULL) || (out_entry == NULL)) {
        return false;
    }

    if (index >= db->count) {
        return false;
    }

    *out_entry = db->entries[index];
    return true;
}

bool pair_db_find(const pair_db_t *db, const pair_device_id_t *device_id, uint8_t *out_index) {
    uint8_t index = 0U;

    if ((db == NULL) || (device_id == NULL) || (out_index == NULL)) {
        return false;
    }

    for (index = 0U; index < db->count; index++) {
        if (pair_db_device_id_equal(&db->entries[index].device_id, device_id)) {
            *out_index = index;
            return true;
        }
    }

    return false;
}

bool pair_db_touch_session(pair_db_t *db,
                           const pair_device_id_t *device_id,
                           uint32_t seen_at_ms,
                           uint16_t vendor_id,
                           uint16_t product_id,
                           uint16_t report_descriptor_len,
                           uint8_t protocol_mode) {
    uint8_t index = 0U;

    if ((db == NULL) || (device_id == NULL)) {
        return false;
    }

    if (!pair_db_find(db, device_id, &index)) {
        if (!pair_db_add(db, device_id, seen_at_ms)) {
            return false;
        }

        index = (uint8_t)(db->count - 1U);
    }

    db->entries[index].last_seen_ms = seen_at_ms;
    db->entries[index].last_vendor_id = vendor_id;
    db->entries[index].last_product_id = product_id;
    db->entries[index].last_report_descriptor_len = report_descriptor_len;
    db->entries[index].last_protocol_mode = protocol_mode;
    return true;
}

bool pair_db_set_reconnect_allowed(pair_db_t *db, const pair_device_id_t *device_id, bool reconnect_allowed) {
    uint8_t index = 0U;

    if ((db == NULL) || (device_id == NULL)) {
        return false;
    }

    if (!pair_db_find(db, device_id, &index)) {
        return false;
    }

    db->entries[index].reconnect_allowed = reconnect_allowed ? 1U : 0U;
    return true;
}

bool pair_db_get_reconnect_candidate(const pair_db_t *db, pair_db_entry_t *out_entry) {
    uint8_t index = 0U;
    bool found = false;
    pair_db_entry_t best = {0};

    if ((db == NULL) || (out_entry == NULL)) {
        return false;
    }

    for (index = 0U; index < db->count; index++) {
        const pair_db_entry_t *entry = &db->entries[index];

        if (entry->reconnect_allowed == 0U) {
            continue;
        }

        if (!found || (entry->last_seen_ms > best.last_seen_ms)) {
            best = *entry;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    *out_entry = best;
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
