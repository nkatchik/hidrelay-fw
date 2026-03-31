#include "platform_pico_w_pair_store.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico.h"

#define PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE (FLASH_SECTOR_SIZE * 2U)

#if (PICO_FLASH_SIZE_BYTES < (PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE + FLASH_SECTOR_SIZE))
#error "Insufficient flash for Pair DB + BTstack flash bank storage"
#endif

enum {
    PICO_W_PAIR_STORE_MAGIC = 0x48494452U,
    PICO_W_PAIR_STORE_VERSION_V3 = 3U,
    PICO_W_PAIR_STORE_VERSION_V4 = 4U,
    PICO_W_PAIR_STORE_VERSION_V5 = 5U,
    PICO_W_PAIR_STORE_SLOT_COUNT = 2U,
    PICO_W_PAIR_STORE_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES
        - PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE
        - (FLASH_SECTOR_SIZE * PICO_W_PAIR_STORE_SLOT_COUNT),
    PICO_W_PAIR_STORE_LEGACY_FLASH_OFFSET =
        PICO_FLASH_SIZE_BYTES - PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE - FLASH_SECTOR_SIZE,
    PICO_W_FACTORY_RESET_ERASE_LEN =
        (FLASH_SECTOR_SIZE * PICO_W_PAIR_STORE_SLOT_COUNT) + PICO_W_BTSTACK_FLASH_BANK_TOTAL_SIZE,
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
} pico_w_pair_store_blob_v5_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t payload_size;
    pair_db_v4_t pair_db;
    uint32_t checksum;
} pico_w_pair_store_blob_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    pair_db_v4_t pair_db;
    uint32_t checksum;
} pico_w_pair_store_blob_v3_t;

typedef struct {
    uint8_t slot_index;
    uint32_t flash_offset;
    const pico_w_pair_store_blob_v5_t * blob;
    uint32_t sequence;
} pico_w_pair_store_latest_v5_t;

typedef struct {
    uint8_t slot_index;
    uint32_t flash_offset;
    const pico_w_pair_store_blob_v4_t * blob;
    uint32_t sequence;
} pico_w_pair_store_latest_v4_t;

static uint32_t pico_w_pair_store_checksum(
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

static void pico_w_pair_store_erase_range(
    uint32_t flash_offset,
    uint32_t flash_len
) {
    uint32_t irq_state = 0U;

    irq_state = save_and_disable_interrupts();
    flash_range_erase(flash_offset, flash_len);
    restore_interrupts(irq_state);
}

static uint32_t pico_w_pair_store_slot_offset(uint8_t slot_index) {
    if (slot_index >= PICO_W_PAIR_STORE_SLOT_COUNT) {
        return PICO_W_PAIR_STORE_FLASH_OFFSET;
    }

    return PICO_W_PAIR_STORE_FLASH_OFFSET + (slot_index * FLASH_SECTOR_SIZE);
}

static bool pico_w_pair_store_sequence_newer(
    uint32_t candidate,
    uint32_t reference
) {
    return (int32_t)(candidate - reference) > 0;
}

static bool pico_w_pair_store_blob_valid_v5(const pico_w_pair_store_blob_v5_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PICO_W_PAIR_STORE_MAGIC)
        || (blob->version != PICO_W_PAIR_STORE_VERSION_V5)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum = pico_w_pair_store_checksum(
        (const uint8_t *)blob,
        offsetof(pico_w_pair_store_blob_v5_t, checksum)
    );
    return expected_checksum == blob->checksum;
}

static bool pico_w_pair_store_blob_valid_v4(const pico_w_pair_store_blob_v4_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PICO_W_PAIR_STORE_MAGIC)
        || (blob->version != PICO_W_PAIR_STORE_VERSION_V4)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum = pico_w_pair_store_checksum(
        (const uint8_t *)blob,
        offsetof(pico_w_pair_store_blob_v4_t, checksum)
    );
    return expected_checksum == blob->checksum;
}

static bool pico_w_pair_store_blob_valid_v3(const pico_w_pair_store_blob_v3_t * blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PICO_W_PAIR_STORE_MAGIC)
        || (blob->version != PICO_W_PAIR_STORE_VERSION_V3)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db))
        || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum = pico_w_pair_store_checksum(
        (const uint8_t *)blob,
        offsetof(pico_w_pair_store_blob_v3_t, checksum)
    );
    return expected_checksum == blob->checksum;
}

static bool pico_w_pair_store_find_latest_v5(pico_w_pair_store_latest_v5_t * out_latest) {
    pico_w_pair_store_latest_v5_t latest = {0};
    bool found = false;
    uint8_t slot_index = 0U;

    if (out_latest == NULL) {
        return false;
    }

    for (slot_index = 0U; slot_index < PICO_W_PAIR_STORE_SLOT_COUNT; slot_index++) {
        const uint32_t flash_offset = pico_w_pair_store_slot_offset(slot_index);
        const pico_w_pair_store_blob_v5_t * blob =
            (const pico_w_pair_store_blob_v5_t *)(XIP_BASE + flash_offset);

        if (!pico_w_pair_store_blob_valid_v5(blob)) {
            continue;
        }

        if (!found || pico_w_pair_store_sequence_newer(blob->sequence, latest.sequence)) {
            latest.slot_index = slot_index;
            latest.flash_offset = flash_offset;
            latest.blob = blob;
            latest.sequence = blob->sequence;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    *out_latest = latest;
    return true;
}

static bool pico_w_pair_store_find_latest_v4(pico_w_pair_store_latest_v4_t * out_latest) {
    pico_w_pair_store_latest_v4_t latest = {0};
    bool found = false;
    uint8_t slot_index = 0U;

    if (out_latest == NULL) {
        return false;
    }

    for (slot_index = 0U; slot_index < PICO_W_PAIR_STORE_SLOT_COUNT; slot_index++) {
        const uint32_t flash_offset = pico_w_pair_store_slot_offset(slot_index);
        const pico_w_pair_store_blob_v4_t * blob =
            (const pico_w_pair_store_blob_v4_t *)(XIP_BASE + flash_offset);

        if (!pico_w_pair_store_blob_valid_v4(blob)) {
            continue;
        }

        if (!found || pico_w_pair_store_sequence_newer(blob->sequence, latest.sequence)) {
            latest.slot_index = slot_index;
            latest.flash_offset = flash_offset;
            latest.blob = blob;
            latest.sequence = blob->sequence;
            found = true;
        }
    }

    if (!found) {
        return false;
    }

    *out_latest = latest;
    return true;
}

static void pico_w_pair_store_convert_v4_db(
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

static bool pico_w_pair_store_load_legacy_v3(pair_db_t * db) {
    const pico_w_pair_store_blob_v3_t * legacy_blob = NULL;

    if (db == NULL) {
        return false;
    }

    legacy_blob =
        (const pico_w_pair_store_blob_v3_t *)(XIP_BASE + PICO_W_PAIR_STORE_LEGACY_FLASH_OFFSET);

    if (!pico_w_pair_store_blob_valid_v3(legacy_blob)) {
        return false;
    }

    pico_w_pair_store_convert_v4_db(&legacy_blob->pair_db, db);
    return true;
}

bool pico_w_pair_store_load(pair_db_t * db) {
    pico_w_pair_store_latest_v5_t latest_v5 = {0};
    pico_w_pair_store_latest_v4_t latest_v4 = {0};

    if (db == NULL) {
        return false;
    }

    (void)memset(db, 0, sizeof(*db));

    if (pico_w_pair_store_find_latest_v5(&latest_v5)) {
        *db = latest_v5.blob->pair_db;
        return true;
    }

    if (pico_w_pair_store_find_latest_v4(&latest_v4)) {
        pico_w_pair_store_convert_v4_db(&latest_v4.blob->pair_db, db);
        return true;
    }

    return pico_w_pair_store_load_legacy_v3(db);
}

bool pico_w_pair_store_save(const pair_db_t * db) {
    uint8_t flash_buffer[FLASH_SECTOR_SIZE] = {0};
    pico_w_pair_store_blob_v5_t * blob = NULL;
    pico_w_pair_store_latest_v5_t latest_v5 = {0};
    bool has_latest_v5 = false;
    uint8_t target_slot_index = 0U;
    uint32_t target_flash_offset = 0U;
    uint32_t next_sequence = 1U;
    uint32_t irq_state = 0U;

    if ((db == NULL) || (db->count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    has_latest_v5 = pico_w_pair_store_find_latest_v5(&latest_v5);

    if (has_latest_v5 && (memcmp(&latest_v5.blob->pair_db, db, sizeof(*db)) == 0)) {
        return true;
    }

    if (has_latest_v5) {
        target_slot_index = (uint8_t)((latest_v5.slot_index + 1U) % PICO_W_PAIR_STORE_SLOT_COUNT);
        next_sequence = latest_v5.sequence + 1U;
    }

    target_flash_offset = pico_w_pair_store_slot_offset(target_slot_index);

    (void)memset(flash_buffer, 0xFF, sizeof(flash_buffer));
    blob = (pico_w_pair_store_blob_v5_t *)flash_buffer;
    blob->magic = PICO_W_PAIR_STORE_MAGIC;
    blob->version = PICO_W_PAIR_STORE_VERSION_V5;
    blob->sequence = next_sequence;
    blob->payload_size = sizeof(*db);
    blob->pair_db = *db;
    blob->checksum =
        pico_w_pair_store_checksum(flash_buffer, offsetof(pico_w_pair_store_blob_v5_t, checksum));

    irq_state = save_and_disable_interrupts();
    flash_range_erase(target_flash_offset, FLASH_SECTOR_SIZE);
    flash_range_program(target_flash_offset, flash_buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
    return true;
}

bool pico_w_pair_store_factory_reset_all(void) {
    pico_w_pair_store_erase_range(PICO_W_PAIR_STORE_FLASH_OFFSET, PICO_W_FACTORY_RESET_ERASE_LEN);
    return true;
}
