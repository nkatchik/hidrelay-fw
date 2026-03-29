#ifndef HIDRELAY_PLATFORM_PICO_W_PAIR_STORE_H
#define HIDRELAY_PLATFORM_PICO_W_PAIR_STORE_H

#include <stdbool.h>

#include "pair_db.h"

bool pico_w_pair_store_load(pair_db_t * db);
bool pico_w_pair_store_save(const pair_db_t * db);
bool pico_w_pair_store_factory_reset_all(void);

#endif
