#ifndef HIDRELAY_PAIR_STORE_H
#define HIDRELAY_PAIR_STORE_H

#include <stdbool.h>

#include "pair_db.h"

/*
 * Persistent pair DB storage: versioned, checksummed blobs rotated across two
 * platform storage slots so a failed write never loses the previous copy.
 * Blob layout, versioning, and migration are common code; platforms only
 * provide the raw slot read/write primitives (see platform_api.h).
 */
bool pair_store_load(pair_db_t * db);
bool pair_store_save(const pair_db_t * db);

#endif
