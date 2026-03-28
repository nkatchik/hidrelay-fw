#include "platform_pico_w_stack.h"

#include <stddef.h>

#ifdef APP_PICO_HAS_BTSTACK
#include "ble/sm.h"
#include "btstack_event.h"
#include "classic/hid_host.h"
#include "gap.h"
#include "hci.h"
#include "l2cap.h"
#include "pico/btstack_cyw43.h"
#include "pico/cyw43_arch.h"
#endif

#include "platform_pico_w_tinyusb_runtime.h"

enum {
    PICO_W_STACK_MAX_USB_INTERFACE = 8U
};

static uint8_t g_usb_interface_count = 0U;
static uint32_t g_usb_descriptor_generation = 0U;

#ifdef APP_PICO_HAS_BTSTACK
static uint8_t g_btstack_hid_descriptor_storage[1024] = {0};
static uint16_t g_btstack_hid_open_count = 0U;
static uint16_t g_btstack_hid_close_count = 0U;

static void pico_w_btstack_hid_packet_handler(uint8_t packet_type,
                                              uint16_t channel,
                                              uint8_t *packet,
                                              uint16_t size) {
    (void)channel;

    if ((packet == NULL) || (size < 3U) || (packet_type != HCI_EVENT_PACKET)) {
        return;
    }

    if (hci_event_packet_get_type(packet) != HCI_EVENT_HID_META) {
        return;
    }

    switch (hci_event_hid_meta_get_subevent_code(packet)) {
    case HID_SUBEVENT_CONNECTION_OPENED:
        if (hid_subevent_connection_opened_get_status(packet) == ERROR_CODE_SUCCESS) {
            g_btstack_hid_open_count = (uint16_t)(g_btstack_hid_open_count + 1U);
        }
        break;
    case HID_SUBEVENT_CONNECTION_CLOSED:
        g_btstack_hid_close_count = (uint16_t)(g_btstack_hid_close_count + 1U);
        break;
    default:
        break;
    }
}
#endif

bool pico_w_stack_init(void) {
#ifdef APP_PICO_HAS_BTSTACK
    async_context_t *context = cyw43_arch_async_context();

    if ((context == NULL) || !btstack_cyw43_init(context)) {
        return false;
    }

    l2cap_init();
    sm_init();
    hid_host_init(g_btstack_hid_descriptor_storage, sizeof(g_btstack_hid_descriptor_storage));
    hid_host_register_packet_handler(&pico_w_btstack_hid_packet_handler);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_SNIFF_MODE | LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);
    (void)hci_power_control(HCI_POWER_ON);
#endif

    if (!pico_w_tinyusb_runtime_init()) {
        return false;
    }

    return true;
}

void pico_w_stack_poll(uint32_t now_ms) {
    (void)now_ms;
    pico_w_tinyusb_runtime_poll();
}

void pico_w_stack_set_usb_plan(uint8_t interface_count, uint32_t descriptor_generation) {
    if (descriptor_generation == g_usb_descriptor_generation) {
        return;
    }

    if (interface_count > PICO_W_STACK_MAX_USB_INTERFACE) {
        interface_count = PICO_W_STACK_MAX_USB_INTERFACE;
    }

    g_usb_interface_count = interface_count;
    g_usb_descriptor_generation = descriptor_generation;
}

uint8_t pico_w_stack_usb_interface_count(void) {
    return g_usb_interface_count;
}
