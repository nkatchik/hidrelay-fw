#ifndef HIDRELAY_USB_BRIDGE_H
#define HIDRELAY_USB_BRIDGE_H

#include <stdint.h>

#include "pair_db.h"

typedef struct {
    uint8_t exported_interface_count;
} usb_bridge_t;

void usb_bridge_init(usb_bridge_t *bridge);
void usb_bridge_sync_from_pair_db(usb_bridge_t *bridge, const pair_db_t *pair_db);
void usb_bridge_tick(usb_bridge_t *bridge, uint32_t now_ms);
uint8_t usb_bridge_interface_count(const usb_bridge_t *bridge);

#endif
