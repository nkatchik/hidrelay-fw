#ifdef APP_HAS_BTSTACK

#include <stddef.h>

#include "btstack_tlv_flash_bank.h"
#include "pico/btstack_cyw43.h"
#include "pico/btstack_flash_bank.h"
#include "pico/cyw43_arch.h"
#include "platform_bt_port.h"

static btstack_tlv_flash_bank_t g_pico_w_tlv_flash_bank_context = {0};

bool platform_bt_port_init(
    const btstack_tlv_t ** out_tlv_impl,
    void ** out_tlv_context
) {
    async_context_t * context = NULL;
    const hal_flash_bank_t * flash_bank = NULL;

    if ((out_tlv_impl == NULL) || (out_tlv_context == NULL)) {
        return false;
    }

    context = cyw43_arch_async_context();
    flash_bank = pico_flash_bank_instance();

    if ((context == NULL) || (flash_bank == NULL) || !btstack_cyw43_init(context)) {
        return false;
    }

    *out_tlv_impl =
        btstack_tlv_flash_bank_init_instance(&g_pico_w_tlv_flash_bank_context, flash_bank, NULL);
    *out_tlv_context = &g_pico_w_tlv_flash_bank_context;
    return *out_tlv_impl != NULL;
}

/*
 * BTstack runs on the cyw43 async context, which on this platform executes
 * in a background (IRQ-driven) context, not on the application thread. The
 * async context lock is recursive and safe to take from its own callbacks.
 */
void platform_bt_port_lock(void) {
    async_context_t * context = cyw43_arch_async_context();

    if (context != NULL) {
        async_context_acquire_lock_blocking(context);
    }
}

void platform_bt_port_unlock(void) {
    async_context_t * context = cyw43_arch_async_context();

    if (context != NULL) {
        async_context_release_lock(context);
    }
}

#endif
