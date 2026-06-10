#ifndef HIDRELAY_PLATFORM_API_H
#define HIDRELAY_PLATFORM_API_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Hardware primitives each platform under platform/ provides. Everything
 * above this line of abstraction -- transport stack, USB runtime, pair store
 * blob format -- is common code.
 */

bool platform_init(void);
bool platform_button_pressed(void);
uint32_t platform_uptime_ms(void);
void platform_set_led(bool led_on);
void platform_sleep_us(uint32_t sleep_us);
bool platform_factory_reset_erase_persistent_data(void);
void platform_reboot(void);

/*
 * Raw persistent-storage slots backing the pair store (see pair_store.h).
 * Platforms provide two equally-sized erase blocks addressed as slots 0 and 1;
 * a write replaces the whole slot. Reads of never-written slots may return
 * garbage -- the pair store validates content with its own checksum.
 *
 * platform_storage_read_legacy reads the pre-slot storage location used by
 * older firmware so its pair DB can be migrated; platforms without such
 * history return false.
 */
bool platform_storage_read(
    uint8_t slot_index,
    void * buffer,
    uint32_t length
);
bool platform_storage_write(
    uint8_t slot_index,
    const void * buffer,
    uint32_t length
);
bool platform_storage_read_legacy(
    void * buffer,
    uint32_t length
);

#endif
