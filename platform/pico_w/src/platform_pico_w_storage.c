#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico.h"
#include "platform_api.h"

#ifndef PICO_FLASH_BANK_TOTAL_SIZE
#define PICO_FLASH_BANK_TOTAL_SIZE (FLASH_SECTOR_SIZE * 2U)
#endif

#ifndef PICO_FLASH_BANK_STORAGE_OFFSET
/* Mirror the Pico SDK BTstack flash-bank default, including the RP2350 final
 * spare sector used for its flash erratum workaround. */
#if defined(PICO_RP2350) \
    && PICO_RP2350 \
    && defined(PICO_RP2350_A2_SUPPORTED) \
    && PICO_RP2350_A2_SUPPORTED
#define PICO_FLASH_BANK_STORAGE_OFFSET \
    (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - PICO_FLASH_BANK_TOTAL_SIZE)
#else
#define PICO_FLASH_BANK_STORAGE_OFFSET (PICO_FLASH_SIZE_BYTES - PICO_FLASH_BANK_TOTAL_SIZE)
#endif
#endif

#define PICO_W_STORAGE_SLOT_COUNT 2U
#define PICO_W_STORAGE_SLOT_TOTAL_SIZE (FLASH_SECTOR_SIZE * PICO_W_STORAGE_SLOT_COUNT)

#if (PICO_FLASH_BANK_STORAGE_OFFSET < PICO_W_STORAGE_SLOT_TOTAL_SIZE)
#error "Insufficient flash for Pair DB storage below BTstack flash bank storage"
#endif

enum {
    PICO_W_STORAGE_FLASH_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET - PICO_W_STORAGE_SLOT_TOTAL_SIZE,
    PICO_W_STORAGE_LEGACY_FLASH_OFFSET = PICO_FLASH_BANK_STORAGE_OFFSET - FLASH_SECTOR_SIZE,
    PICO_W_FACTORY_RESET_ERASE_LEN = PICO_W_STORAGE_SLOT_TOTAL_SIZE + PICO_FLASH_BANK_TOTAL_SIZE,
};

static uint32_t pico_w_storage_slot_offset(uint8_t slot_index) {
    return PICO_W_STORAGE_FLASH_OFFSET + ((uint32_t)slot_index * FLASH_SECTOR_SIZE);
}

bool platform_storage_read(
    uint8_t slot_index,
    void * buffer,
    uint32_t length
) {
    if ((buffer == NULL)
        || (slot_index >= PICO_W_STORAGE_SLOT_COUNT)
        || (length > FLASH_SECTOR_SIZE)) {
        return false;
    }

    (void)memcpy(buffer, (const void *)(XIP_BASE + pico_w_storage_slot_offset(slot_index)), length);
    return true;
}

bool platform_storage_write(
    uint8_t slot_index,
    const void * buffer,
    uint32_t length
) {
    uint8_t flash_buffer[FLASH_SECTOR_SIZE] = {0};
    uint32_t flash_offset = 0U;
    uint32_t irq_state = 0U;

    if ((buffer == NULL)
        || (slot_index >= PICO_W_STORAGE_SLOT_COUNT)
        || (length > FLASH_SECTOR_SIZE)) {
        return false;
    }

    (void)memset(flash_buffer, 0xFF, sizeof(flash_buffer));
    (void)memcpy(flash_buffer, buffer, length);
    flash_offset = pico_w_storage_slot_offset(slot_index);

    irq_state = save_and_disable_interrupts();
    flash_range_erase(flash_offset, FLASH_SECTOR_SIZE);
    flash_range_program(flash_offset, flash_buffer, FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
    return true;
}

bool platform_storage_read_legacy(
    void * buffer,
    uint32_t length
) {
    if ((buffer == NULL) || (length > FLASH_SECTOR_SIZE)) {
        return false;
    }

    (void)memcpy(buffer, (const void *)(XIP_BASE + PICO_W_STORAGE_LEGACY_FLASH_OFFSET), length);
    return true;
}

bool platform_factory_reset_erase_persistent_data(void) {
    uint32_t irq_state = 0U;

    irq_state = save_and_disable_interrupts();
    flash_range_erase(PICO_W_STORAGE_FLASH_OFFSET, PICO_W_FACTORY_RESET_ERASE_LEN);
    restore_interrupts(irq_state);
    return true;
}
