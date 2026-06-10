#include "pair_store.h"

#include <stddef.h>
#include <string.h>

#include "platform_api.h"

enum {
    PAIR_STORE_MAGIC = 0x48494452U,
    PAIR_STORE_VERSION_V3 = 3U,
    PAIR_STORE_VERSION_V4 = 4U,
    PAIR_STORE_VERSION_V5 = 5U,
    PAIR_STORE_SLOT_COUNT = 2U,
};

typedef struct {
    pair_device_id_t device_id;
    uint32_t paired_at_ms;
    uint32_t last_seen_ms;
    uint16_t last_vendor_id;
    uint16_t last_product_id;
    uint16_t last_report_descriptor_len;
    uint8_t last_protocol_mode;
    uint8_t reconnect_allowed;
    uint8_t reconnect_fail_count;
    uint32_t reconnect_retry_after_ms;
} pair_db_entry_v4_t;

typedef struct {
    pair_db_entry_v4_t entries[PAIR_DB_MAX_DEVICE];
    uint8_t count;
} pair_db_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t payload_size;
    pair_db_t pair_db;
    uint32_t checksum;
} pair_store_blob_v5_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t payload_size;
    pair_db_v4_t pair_db;
    uint32_t checksum;
} pair_store_blob_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    pair_db_v4_t pair_db;
    uint32_t checksum;
} pair_store_blob_v3_t;

typedef struct {
    uint8_t slot_index;
    pair_store_blob_v5_t blob;
    uint32_t sequence;
} pair_store_latest_v5_t;

typedef struct {
    uint8_t slot_index;
    pair_store_blob_v4_t blob;
    uint32_t sequence;
} pair_store_latest_v4_t;

static uint32_t pair_store_checksum(
    const uint8_t * data,
    size_t len
) {
    uint32_t hash = 2166136261u;
    size_t index = 0U;

    if (data == NULL) {
        return 0U;
    }

    for (index = 0U; index < len; index++) {
        hash ^= data[index];
        hash *= 16777619u;
    }

    return hash;
}

static bool pair_store_sequence_newer(
    uint32_t candidate,
    uint32_t reference
) {
    return (int32_t)(candidate - reference) > 0;
}

static bool pair_store_blob_valid_v5(const pair_store_blob_v5_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PAIR_STORE_MAGIC) || (blob->version != PAIR_STORE_VERSION_V5)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum =
        pair_store_checksum((const uint8_t *)blob, offsetof(pair_store_blob_v5_t, checksum));
    return expected_checksum == blob->checksum;
}

static bool pair_store_blob_valid_v4(const pair_store_blob_v4_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PAIR_STORE_MAGIC) || (blob->version != PAIR_STORE_VERSION_V4)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum =
        pair_store_checksum((const uint8_t *)blob, offsetof(pair_store_blob_v4_t, checksum));
    return expected_checksum == blob->checksum;
}

static bool pair_store_blob_valid_v3(const pair_store_blob_v3_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PAIR_STORE_MAGIC) || (blob->version != PAIR_STORE_VERSION_V3)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum =
        pair_store_checksum((const uint8_t *)blob, offsetof(pair_store_blob_v3_t, checksum));
    return expected_checksum == blob->checksum;
}

static bool pair_store_find_latest_v5(pair_store_latest_v5_t * out_latest) {
    static pair_store_blob_v5_t candidate = {0};
    bool found = false;
    uint8_t slot_index = 0U;

    if (out_latest == NULL) {
        return false;
    }

    for (slot_index = 0U; slot_index < PAIR_STORE_SLOT_COUNT; slot_index++) {
        if (!platform_storage_read(slot_index, &candidate, sizeof(candidate))) {
            continue;
        }

        if (!pair_store_blob_valid_v5(&candidate)) {
            continue;
        }

        if (!found || pair_store_sequence_newer(candidate.sequence, out_latest->sequence)) {
            out_latest->slot_index = slot_index;
            out_latest->blob = candidate;
            out_latest->sequence = candidate.sequence;
            found = true;
        }
    }

    return found;
}

static bool pair_store_find_latest_v4(pair_store_latest_v4_t * out_latest) {
    static pair_store_blob_v4_t candidate = {0};
    bool found = false;
    uint8_t slot_index = 0U;

    if (out_latest == NULL) {
        return false;
    }

    for (slot_index = 0U; slot_index < PAIR_STORE_SLOT_COUNT; slot_index++) {
        if (!platform_storage_read(slot_index, &candidate, sizeof(candidate))) {
            continue;
        }

        if (!pair_store_blob_valid_v4(&candidate)) {
            continue;
        }

        if (!found || pair_store_sequence_newer(candidate.sequence, out_latest->sequence)) {
            out_latest->slot_index = slot_index;
            out_latest->blob = candidate;
            out_latest->sequence = candidate.sequence;
            found = true;
        }
    }

    return found;
}

static void pair_store_convert_v4_db(
    const pair_db_v4_t * legacy_db,
    pair_db_t * db
) {
    uint8_t index = 0U;

    if ((legacy_db == NULL) || (db == NULL)) {
        return;
    }

    (void)memset(db, 0, sizeof(*db));
    db->count = legacy_db->count;
    if (db->count > PAIR_DB_MAX_DEVICE) {
        db->count = PAIR_DB_MAX_DEVICE;
    }

    for (index = 0U; index < db->count; index++) {
        db->entries[index].device_id = legacy_db->entries[index].device_id;
        db->entries[index].paired_at_ms = legacy_db->entries[index].paired_at_ms;
        db->entries[index].last_seen_ms = legacy_db->entries[index].last_seen_ms;
        db->entries[index].last_vendor_id = legacy_db->entries[index].last_vendor_id;
        db->entries[index].last_product_id = legacy_db->entries[index].last_product_id;
        db->entries[index].last_report_descriptor_len =
            legacy_db->entries[index].last_report_descriptor_len;
        db->entries[index].last_protocol_mode = legacy_db->entries[index].last_protocol_mode;
        db->entries[index].last_bt_link_type = 0U;
        db->entries[index].last_bt_addr_type = 0U;
        db->entries[index].reconnect_allowed = legacy_db->entries[index].reconnect_allowed;
        db->entries[index].reconnect_fail_count = legacy_db->entries[index].reconnect_fail_count;
        db->entries[index].reconnect_retry_after_ms =
            legacy_db->entries[index].reconnect_retry_after_ms;
    }
}

static bool pair_store_load_legacy_v3(pair_db_t * db) {
    static pair_store_blob_v3_t legacy_blob = {0};

    if (db == NULL) {
        return false;
    }

    if (!platform_storage_read_legacy(&legacy_blob, sizeof(legacy_blob))) {
        return false;
    }

    if (!pair_store_blob_valid_v3(&legacy_blob)) {
        return false;
    }

    pair_store_convert_v4_db(&legacy_blob.pair_db, db);
    return true;
}

bool pair_store_load(pair_db_t * db) {
    static pair_store_latest_v5_t latest_v5 = {0};
    static pair_store_latest_v4_t latest_v4 = {0};

    if (db == NULL) {
        return false;
    }

    (void)memset(db, 0, sizeof(*db));

    if (pair_store_find_latest_v5(&latest_v5)) {
        *db = latest_v5.blob.pair_db;
        return true;
    }

    if (pair_store_find_latest_v4(&latest_v4)) {
        pair_store_convert_v4_db(&latest_v4.blob.pair_db, db);
        return true;
    }

    return pair_store_load_legacy_v3(db);
}

bool pair_store_save(const pair_db_t * db) {
    static pair_store_blob_v5_t blob = {0};
    static pair_store_latest_v5_t latest_v5 = {0};
    bool has_latest_v5 = false;
    uint8_t target_slot_index = 0U;
    uint32_t next_sequence = 1U;

    if ((db == NULL) || (db->count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    has_latest_v5 = pair_store_find_latest_v5(&latest_v5);

    if (has_latest_v5 && (memcmp(&latest_v5.blob.pair_db, db, sizeof(*db)) == 0)) {
        return true;
    }

    if (has_latest_v5) {
        target_slot_index = (uint8_t)((latest_v5.slot_index + 1U) % PAIR_STORE_SLOT_COUNT);
        next_sequence = latest_v5.sequence + 1U;
    }

    (void)memset(&blob, 0, sizeof(blob));
    blob.magic = PAIR_STORE_MAGIC;
    blob.version = PAIR_STORE_VERSION_V5;
    blob.sequence = next_sequence;
    blob.payload_size = sizeof(*db);
    blob.pair_db = *db;
    blob.checksum =
        pair_store_checksum((const uint8_t *)&blob, offsetof(pair_store_blob_v5_t, checksum));

    return platform_storage_write(target_slot_index, &blob, sizeof(blob));
}
