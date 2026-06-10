#ifndef HIDRELAY_PLATFORM_USB_PORT_H
#define HIDRELAY_PLATFORM_USB_PORT_H

#include <stdint.h>

enum {
    /*
     * Headroom reserved in the configuration descriptor buffer for
     * platform-specific extra interfaces. A platform whose extra descriptor
     * exceeds this is ignored.
     */
    PLATFORM_USB_PORT_EXTRA_DESCRIPTOR_MAX_LEN = 64U,
};

/*
 * Optional platform-specific USB interfaces appended after the HID interfaces
 * in the configuration descriptor (e.g. a vendor reset interface). Platforms
 * without extras return 0 from both count/len and leave the buffer untouched.
 * Only referenced from builds with USB device support enabled.
 */
uint8_t platform_usb_port_extra_interface_count(void);
uint16_t platform_usb_port_extra_descriptor_len(void);
uint16_t platform_usb_port_append_extra_descriptor(
    uint8_t * buffer,
    uint16_t offset,
    uint8_t first_interface_number
);

#endif
