#include "platform_pico_w_pair_store.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico.h"

enum {
    PICO_W_PAIR_STORE_MAGIC = 0x48494452U,
    PICO_W_PAIR_STORE_VERSION = 3U,
    PICO_W_PAIR_STORE_FLASH_OFFSET = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE,
};

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t payload_size;
    pair_db_t pair_db;
    uint32_t checksum;
} pico_w_pair_store_blob_t;

static uint32_t pico_w_pair_store_checksum(const uint8_t *data, size_t len) {
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

static bool pico_w_pair_store_blob_valid(const pico_w_pair_store_blob_t *blob) {
    uint32_t expected_checksum = 0U;

    if (blob == NULL) {
        return false;
    }

    if ((blob->magic != PICO_W_PAIR_STORE_MAGIC) || (blob->version != PICO_W_PAIR_STORE_VERSION)) {
        return false;
    }

    if ((blob->payload_size != sizeof(blob->pair_db)) || (blob->pair_db.count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    expected_checksum = pico_w_pair_store_checksum((const uint8_t *)blob, offsetof(pico_w_pair_store_blob_t, checksum));
    return expected_checksum == blob->checksum;
}

bool pico_w_pair_store_load(pair_db_t *db) {
    const pico_w_pair_store_blob_t *blob = NULL;

    if (db == NULL) {
        return false;
    }

    (void)memset(db, 0, sizeof(*db));
    blob = (const pico_w_pair_store_blob_t *)(XIP_BASE + PICO_W_PAIR_STORE_FLASH_OFFSET);

    if (!pico_w_pair_store_blob_valid(blob)) {
        return false;
    }

    *db = blob->pair_db;
    return true;
}

bool pico_w_pair_store_save(const pair_db_t *db) {
    uint8_t flash_buffer[FLASH_SECTOR_SIZE] = {0};
    pico_w_pair_store_blob_t *blob = NULL;
    uint32_t irq_state = 0U;

    if ((db == NULL) || (db->count > PAIR_DB_MAX_DEVICE)) {
        return false;
    }

    (void)memset(flash_buffer, 0xFF, sizeof(flash_buffer));
    blob = (pico_w_pair_store_blob_t *)flash_buffer;
    blob->magic = PICO_W_PAIR_STORE_MAGIC;
    blob->version = PICO_W_PAIR_STORE_VERSION;
    blob->payload_size = sizeof(*db);
    blob->pair_db = *db;
    blob->checksum = pico_w_pair_store_checksum(flash_buffer, offsetof(pico_w_pair_store_blob_t, checksum));

    irq_state = save_and_disable_interrupts();
    flash_range_erase(PICO_W_PAIR_STORE_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(PICO_W_PAIR_STORE_FLASH_OFFSET, flash_buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
    return true;
}
