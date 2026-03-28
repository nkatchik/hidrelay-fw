#include "platform_pico_w_stack.h"

#ifdef APP_PICO_HAS_BTSTACK
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#endif

#ifdef APP_PICO_HAS_TINYUSB
#include "tusb.h"
#endif

bool pico_w_stack_init(void) {
#ifdef APP_PICO_HAS_BTSTACK
    async_context_t *context = cyw43_arch_async_context();

    if ((context == NULL) || !btstack_cyw43_init(context)) {
        return false;
    }
#endif

#ifdef APP_PICO_HAS_TINYUSB
    if (!tusb_init()) {
        return false;
    }
#endif

    return true;
}

void pico_w_stack_poll(uint32_t now_ms) {
    (void)now_ms;

#ifdef APP_PICO_HAS_TINYUSB
    tud_task();
#endif
}
