#ifndef HIDRELAY_PAIR_DB_H
#define HIDRELAY_PAIR_DB_H

#include <stdbool.h>
#include <stdint.h>

#define PAIR_DB_MAX_DEVICE 8U

typedef struct {
    uint8_t bytes[6];
} pair_device_id_t;

typedef struct {
    pair_device_id_t device_id;
    uint32_t paired_at_ms;
} pair_db_entry_t;

typedef struct {
    pair_db_entry_t entries[PAIR_DB_MAX_DEVICE];
    uint8_t count;
} pair_db_t;

void pair_db_init(pair_db_t *db);
bool pair_db_add(pair_db_t *db, const pair_device_id_t *device_id, uint32_t paired_at_ms);
bool pair_db_remove_last(pair_db_t *db);
void pair_db_remove_all(pair_db_t *db);
uint8_t pair_db_count(const pair_db_t *db);
bool pair_db_get(const pair_db_t *db, uint8_t index, pair_device_id_t *out_device_id);
bool pair_db_last_within_window(const pair_db_t *db, uint32_t now_ms, uint32_t window_ms);

#endif
