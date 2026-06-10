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
 * Hang watchdog with breadcrumb reporting. The main loop feeds the
 * watchdog; if any context wedges, the platform reboots the firmware and
 * preserves the two most recent checkpoint markers (one written from the
 * Bluetooth stack's execution context, one from the application thread)
 * across the reset. platform_take_hang_report returns true exactly once
 * after such a reboot, with the frozen marker values -- the last
 * checkpoints passed before the wedge. Deliberate reboots
 * (platform_reboot, flash-reset requests) do not produce a report.
 */
void platform_watchdog_enable(void);
void platform_watchdog_feed(void);
void platform_hang_checkpoint_bt(uint8_t marker);
void platform_hang_checkpoint_main(uint8_t marker);
/*
 * out_panic_class: 0 = no panic recorded (the wedge was a loop or deadlock,
 * not a panic); platform-specific nonzero classes otherwise (on pico_w:
 * 1 = radio shared-bus RX overflow panic, 2 = its register-corruption
 * panic, 3 = any other panic).
 */
bool platform_take_hang_report(
    uint8_t * out_bt_marker,
    uint8_t * out_main_marker,
    uint8_t * out_panic_class
);

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
