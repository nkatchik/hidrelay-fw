#ifndef HIDRELAY_APPLE_KEYBOARD_H
#define HIDRELAY_APPLE_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Relay-side top-row remap for recognized external Apple keyboards.
 *
 * An Apple keyboard's Bluetooth HID profile delivers its top row (F1-F12) as
 * plain Keyboard keycodes, with no per-key media/system encoding -- macOS only
 * applies its native top-row mapping over the keyboard's *USB* profile, which a
 * transparent BT->USB relay does not reproduce. To make the top row behave like
 * a native connection we remap it ourselves: the presented USB report
 * descriptor is augmented with an Apple-vendor/consumer collection declaring the
 * usages macOS honors (Mission Control, Launchpad, Spotlight, brightness, media,
 * volume, ...), and incoming keyboard reports have their F-keys rewritten onto
 * that collection. Non-Apple devices are never touched.
 *
 * Scope: external Apple desktop Bluetooth keyboards from the model/layout table
 * (see apple_keyboard.c). The keyboard report layout is assumed to match the
 * Apple boot-compatible form (modifiers, reserved, six keycodes, then a status
 * byte whose bit 1 is the Fn key) shared by these keyboards.
 */

/* Report ID and length of the synthesized aux (vendor/consumer) report. */
#define APPLE_KEYBOARD_AUX_REPORT_ID 0xB0U
#define APPLE_KEYBOARD_AUX_REPORT_LEN 3U

/*
 * True when (vendor_id, product_id) is a recognized Apple keyboard we have a
 * top-row layout for. vendor_id is the USB-form Apple vendor ID (0x05AC) that
 * the relay clones; product_id is the keyboard's Bluetooth product ID.
 */
bool apple_keyboard_is_supported(
    uint16_t vendor_id,
    uint16_t product_id
);

/*
 * Build base_descriptor + the aux collection into out_buf. Returns the total
 * length, or 0 if product_id is unsupported or out_buf is too small. The aux
 * collection is identical across layouts (layout only selects which usage F4
 * maps to), so this does not depend on anything but support.
 */
uint16_t apple_keyboard_augment_descriptor(
    uint16_t product_id,
    const uint8_t * base_descriptor,
    uint16_t base_len,
    uint8_t * out_buf,
    uint16_t out_cap
);

/* Per-keyboard remap state. Treat as opaque; init with apple_keyboard_state_init. */
typedef struct {
    bool initialized;
    uint16_t product_id;
    uint8_t layout;
    bool media_default; /* true: top row = media, Fn = literal F-keys */
    bool toggle_chord_active; /* Fn+Esc rising-edge tracking */
    uint8_t prev_aux[2]; /* last emitted aux payload, for press/release diffing */
} apple_keyboard_state_t;

void apple_keyboard_state_init(
    apple_keyboard_state_t * state,
    uint16_t product_id
);

/*
 * Transform one incoming keyboard input report (the keyboard's Report ID 1,
 * already stripped of any Bluetooth transaction header).
 *
 * On true: *out_kbd (length *out_kbd_len) is the keyboard report to forward
 * (top-row keys removed when remapped, Fn bit preserved); if *out_aux_len > 0,
 * *out_aux (length *out_aux_len) is an aux report to forward as well (emitted
 * only when the aux state changed this cycle, covering both press and release).
 *
 * Returns false when the report is not the remappable keyboard report (wrong
 * report ID / too short / unsupported state) -- the caller forwards the original
 * report unchanged.
 *
 * out_kbd must hold at least report_len bytes; out_aux at least
 * APPLE_KEYBOARD_AUX_REPORT_LEN bytes.
 */
bool apple_keyboard_process_report(
    apple_keyboard_state_t * state,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_kbd,
    uint16_t * out_kbd_len,
    uint8_t * out_aux,
    uint16_t * out_aux_len
);

#endif
