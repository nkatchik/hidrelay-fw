#ifndef HIDRELAY_BT_MANAGER_H
#define HIDRELAY_BT_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "pair_db.h"

typedef enum {
    BT_MANAGER_STATE_IDLE = 0,
    BT_MANAGER_STATE_PAIRING,
    BT_MANAGER_STATE_ACTIVE,
    BT_MANAGER_STATE_ERROR
} bt_manager_state_t;

#define BT_MANAGER_MAX_ACTIVE_DEVICE PAIR_DB_MAX_DEVICE

typedef struct {
    pair_device_id_t device_id;
    uint16_t hid_cid;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t report_descriptor_len;
} bt_hid_device_t;

typedef struct {
    pair_db_t *pair_db;
    bt_manager_state_t state;
    uint32_t pairing_started_ms;
    bt_hid_device_t active_device[BT_MANAGER_MAX_ACTIVE_DEVICE];
    uint8_t active_count;
} bt_manager_t;

void bt_manager_init(bt_manager_t *manager, pair_db_t *pair_db);
bool bt_manager_start_pair_any(bt_manager_t *manager, uint32_t now_ms);
bool bt_manager_remove_last(bt_manager_t *manager);
bool bt_manager_remove_last_if_recent(bt_manager_t *manager, uint32_t now_ms, uint32_t max_age_ms);
bool bt_manager_remove_all(bt_manager_t *manager);
bool bt_manager_ingest_hid_open(bt_manager_t *manager,
                                const pair_device_id_t *device_id,
                                uint16_t hid_cid,
                                uint16_t vendor_id,
                                uint16_t product_id,
                                uint16_t report_descriptor_len,
                                uint32_t now_ms);
bool bt_manager_ingest_hid_close(bt_manager_t *manager, uint16_t hid_cid);
void bt_manager_tick(bt_manager_t *manager, uint32_t now_ms);
bt_manager_state_t bt_manager_state(const bt_manager_t *manager);
uint8_t bt_manager_active_count(const bt_manager_t *manager);
bool bt_manager_active_get(const bt_manager_t *manager, uint8_t index, bt_hid_device_t *out_device);

#endif
