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

typedef struct {
    pair_db_t *pair_db;
    bt_manager_state_t state;
    uint32_t pairing_started_ms;
} bt_manager_t;

void bt_manager_init(bt_manager_t *manager, pair_db_t *pair_db);
bool bt_manager_start_pair_any(bt_manager_t *manager, uint32_t now_ms);
bool bt_manager_remove_last(bt_manager_t *manager);
bool bt_manager_remove_last_if_recent(bt_manager_t *manager, uint32_t now_ms, uint32_t max_age_ms);
bool bt_manager_remove_all(bt_manager_t *manager);
void bt_manager_tick(bt_manager_t *manager, uint32_t now_ms);
bt_manager_state_t bt_manager_state(const bt_manager_t *manager);

#endif
