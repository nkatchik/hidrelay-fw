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
    uint32_t last_seen_ms;
    uint16_t last_vendor_id;
    uint16_t last_product_id;
    uint16_t last_report_descriptor_len;
    uint8_t last_protocol_mode;
    uint8_t reconnect_allowed;
    uint8_t reconnect_fail_count;
    uint32_t reconnect_retry_after_ms;
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
bool pair_db_get_entry(const pair_db_t *db, uint8_t index, pair_db_entry_t *out_entry);
bool pair_db_find(const pair_db_t *db, const pair_device_id_t *device_id, uint8_t *out_index);
bool pair_db_touch_session(pair_db_t *db,
                           const pair_device_id_t *device_id,
                           uint32_t seen_at_ms,
                           uint16_t vendor_id,
                           uint16_t product_id,
                           uint16_t report_descriptor_len,
                           uint8_t protocol_mode);
bool pair_db_set_reconnect_allowed(pair_db_t *db, const pair_device_id_t *device_id, bool reconnect_allowed);
bool pair_db_mark_reconnect_success(pair_db_t *db, const pair_device_id_t *device_id, uint32_t now_ms);
bool pair_db_mark_reconnect_failure(pair_db_t *db,
                                    const pair_device_id_t *device_id,
                                    uint8_t fail_count,
                                    uint32_t retry_after_ms);
bool pair_db_get_reconnect_candidate(const pair_db_t *db, uint32_t now_ms, pair_db_entry_t *out_entry);
bool pair_db_last_within_window(const pair_db_t *db, uint32_t now_ms, uint32_t window_ms);

#endif
