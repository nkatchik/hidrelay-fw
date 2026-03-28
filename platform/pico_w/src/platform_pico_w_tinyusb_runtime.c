#include "platform_pico_w_tinyusb_runtime.h"

#ifdef APP_PICO_HAS_TINYUSB
#include "tusb.h"
#endif

bool pico_w_tinyusb_runtime_init(void) {
#ifdef APP_PICO_HAS_TINYUSB
    return tusb_init();
#else
    return true;
#endif
}

void pico_w_tinyusb_runtime_poll(void) {
#ifdef APP_PICO_HAS_TINYUSB
    tud_task();
#endif
}
