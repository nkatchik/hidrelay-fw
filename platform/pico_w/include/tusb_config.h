#ifndef HIDRELAY_PICO_W_TUSB_CONFIG_H
#define HIDRELAY_PICO_W_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0
#define CFG_TUD_DFU_RUNTIME 0
#define CFG_TUD_DFU 0
#define CFG_TUD_ECM_RNDIS 0
#define CFG_TUD_NCM 0
#define CFG_TUD_USBTMC 0

#define CFG_TUD_HID 8
#define CFG_TUD_HID_EP_BUFSIZE 16

#ifdef __cplusplus
}
#endif

#endif
