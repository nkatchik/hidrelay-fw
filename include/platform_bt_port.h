#ifndef HIDRELAY_PLATFORM_BT_PORT_H
#define HIDRELAY_PLATFORM_BT_PORT_H

#include <stdbool.h>

#include "btstack_tlv.h"

/*
 * Platform glue for bringing up the Bluetooth stack: initialize the HCI
 * transport/chipset for the local radio and provide the persistent TLV store
 * BTstack uses for Classic link keys and the LE device DB.
 *
 * Returns false when the radio or persistent storage is unavailable; the
 * transport stack then runs without Bluetooth. Only referenced from builds
 * with Bluetooth support enabled, so platforms without it need no stub.
 */
bool platform_bt_port_init(
    const btstack_tlv_t ** out_tlv_impl,
    void ** out_tlv_context
);

/*
 * Serialize access to the Bluetooth stack. On platforms where the stack
 * executes on its own context (e.g. a background/IRQ-driven async context),
 * every call into stack APIs or stack-owned state from the application
 * thread must happen between lock and unlock -- unsynchronized calls race
 * the stack's own execution and corrupt its internal state. The lock is
 * recursive (nested entry points are fine) and may also be taken from
 * within the stack's own callbacks. No-op before the radio is initialized.
 */
void platform_bt_port_lock(void);
void platform_bt_port_unlock(void);

#endif
