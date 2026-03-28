#include "usb_bridge.h"

#include <stddef.h>

void usb_bridge_init(usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return;
    }

    bridge->exported_interface_count = 0U;
}

void usb_bridge_sync_from_pair_db(usb_bridge_t *bridge, const pair_db_t *pair_db) {
    if ((bridge == NULL) || (pair_db == NULL)) {
        return;
    }

    bridge->exported_interface_count = pair_db_count(pair_db);
}

void usb_bridge_tick(usb_bridge_t *bridge, uint32_t now_ms) {
    (void)now_ms;

    if (bridge == NULL) {
        return;
    }
}

uint8_t usb_bridge_interface_count(const usb_bridge_t *bridge) {
    if (bridge == NULL) {
        return 0U;
    }

    return bridge->exported_interface_count;
}
