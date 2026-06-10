#ifndef HIDRELAY_USB_RUNTIME_H
#define HIDRELAY_USB_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

enum {
    /*
     * The USB device stack services its endpoints from the main loop; sleeping
     * longer than this between polls risks missing control transfers during
     * enumeration. Callers clamp their idle sleep to this when USB is enabled.
     */
    USB_RUNTIME_MAX_POLL_SLEEP_US = 500U,
};

bool usb_runtime_init(void);
bool usb_runtime_is_initialized(void);
void usb_runtime_poll(void);
void usb_runtime_mark_descriptor_activity(void);
uint32_t usb_runtime_descriptor_activity_count(void);
bool usb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
);
void usb_runtime_request_reenumeration(void);
bool usb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
);

#endif
