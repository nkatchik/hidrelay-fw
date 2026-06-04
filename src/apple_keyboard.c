#include "apple_keyboard.h"

#include <stddef.h>
#include <string.h>

/* USB-form Apple vendor ID that the relay clones for Apple keyboards. */
#define APPLE_KEYBOARD_USB_VENDOR_ID 0x05ACU

/*
 * Keyboard report (Report ID 1) shape shared by these Apple keyboards, as
 * received after the Bluetooth transaction header is stripped:
 *   [0] report ID (1)
 *   [1] modifiers
 *   [2] reserved (Vendor 0xFF00 const)
 *   [3..8] six keycodes (Keyboard usage array)
 *   [9] status: bit0 Eject, bit1 Fn (Vendor 0xFF01 usage 3), bits2-7 const
 */
enum {
    APPLE_KEYBOARD_REPORT_ID = 0x01U,
    APPLE_KEYBOARD_REPORT_MIN_LEN = 10U,
    APPLE_KEYBOARD_KEYCODE_FIRST = 3U,
    APPLE_KEYBOARD_KEYCODE_LAST = 8U,
    APPLE_KEYBOARD_STATUS_BYTE = 9U,
    APPLE_KEYBOARD_FN_BIT_MASK = 0x02U
};

/* HID keyboard usage IDs for the top row and the mode-toggle key. */
enum {
    APPLE_KEYBOARD_KEY_F1 = 0x3AU,
    APPLE_KEYBOARD_KEY_F12 = 0x45U,
    APPLE_KEYBOARD_KEY_ESC = 0x29U
};

/*
 * Bit positions in the 2-byte aux payload. The order here must match the field
 * declaration order in the augmentation descriptor below.
 */
enum {
    AUX_BIT_SPOTLIGHT = 0,
    AUX_BIT_DASHBOARD = 1,
    AUX_BIT_LAUNCHPAD = 2,
    AUX_BIT_EXPOSE = 3,
    AUX_BIT_BRIGHT_UP = 4,
    AUX_BIT_BRIGHT_DOWN = 5,
    AUX_BIT_SCAN_PREV = 6,
    AUX_BIT_PLAY_PAUSE = 7,
    AUX_BIT_SCAN_NEXT = 8,
    AUX_BIT_MUTE = 9,
    AUX_BIT_VOL_UP = 10,
    AUX_BIT_VOL_DOWN = 11,
    AUX_BIT_VOICE = 12,
    AUX_BIT_DND = 13,
    AUX_BIT_NONE = 0xFFU
};

/*
 * Top-row layouts. Only F4 differs between them (Dashboard / Launchpad /
 * Spotlight); every other key maps identically, so the per-key table below is
 * shared and F4 is resolved separately.
 */
enum {
    APPLE_KEYBOARD_LAYOUT_NONE = 0U,
    APPLE_KEYBOARD_LAYOUT_A = 1U, /* Expose + Dashboard (A1255, early A1314) */
    APPLE_KEYBOARD_LAYOUT_B = 2U, /* Mission Control + Launchpad (A1314'11, A1644, A1843) */
    APPLE_KEYBOARD_LAYOUT_C = 3U /* Mission Control + Spotlight (2021+ Magic Keyboards) */
};

/* F1..F12 (index 0..11) -> aux bit. F4 (index 3) is layout-specific (see below). */
static const uint8_t k_apple_keyboard_fkey_aux[12] = {
    AUX_BIT_BRIGHT_DOWN, /* F1  */
    AUX_BIT_BRIGHT_UP, /* F2  */
    AUX_BIT_EXPOSE, /* F3  */
    AUX_BIT_NONE, /* F4  -> apple_keyboard_f4_aux() */
    AUX_BIT_VOICE, /* F5  Dictation */
    AUX_BIT_DND, /* F6  Do Not Disturb / Focus */
    AUX_BIT_SCAN_PREV, /* F7  */
    AUX_BIT_PLAY_PAUSE, /* F8  */
    AUX_BIT_SCAN_NEXT, /* F9  */
    AUX_BIT_MUTE, /* F10 */
    AUX_BIT_VOL_DOWN, /* F11 */
    AUX_BIT_VOL_UP /* F12 */
};

static uint8_t apple_keyboard_layout_for_pid(uint16_t product_id) {
    switch (product_id) {
        /* Layout A: Expose / Dashboard */
        case 0x022CU:
        case 0x022DU:
        case 0x022EU: /* A1255 */
        case 0x0239U:
        case 0x023AU:
        case 0x023BU: /* A1314 (2009) */
            return APPLE_KEYBOARD_LAYOUT_A;
        /* Layout B: Mission Control / Launchpad */
        case 0x0255U:
        case 0x0256U:
        case 0x0257U: /* A1314 (2011) */
        case 0x0267U: /* A1644 Magic Keyboard */
        case 0x026CU: /* A1843 Magic Keyboard + Numpad */
            return APPLE_KEYBOARD_LAYOUT_B;
        /* Layout C: Mission Control / Spotlight (2021+) */
        case 0x029AU:
        case 0x029CU:
        case 0x029FU: /* A2449 / A2450 / A2520 (Lightning) */
        case 0x0320U:
        case 0x0321U:
        case 0x0322U: /* A3203 / A3118 / A3119 (USB-C) */
            return APPLE_KEYBOARD_LAYOUT_C;
        default:
            return APPLE_KEYBOARD_LAYOUT_NONE;
    }
}

static uint8_t apple_keyboard_f4_aux(uint8_t layout) {
    switch (layout) {
        case APPLE_KEYBOARD_LAYOUT_A:
            return AUX_BIT_DASHBOARD;
        case APPLE_KEYBOARD_LAYOUT_C:
            return AUX_BIT_SPOTLIGHT;
        case APPLE_KEYBOARD_LAYOUT_B:
        default:
            return AUX_BIT_LAUNCHPAD;
    }
}

/*
 * Aux collection appended to the keyboard's report descriptor. Declares every
 * usage the remap may emit; macOS rejects input reports carrying usages not
 * present in the descriptor, so each must be listed even if a given layout
 * never sends it. Field order matches the AUX_BIT_* bit positions.
 */
static const uint8_t k_apple_keyboard_aux_descriptor[] = {
    0x05,
    0x01, /* Usage Page (Generic Desktop)               */
    0x09,
    0x06, /* Usage (Keyboard)                           */
    0xA1,
    0x01, /* Collection (Application)                   */
    0x85,
    APPLE_KEYBOARD_AUX_REPORT_ID, /*   Report ID (0xB0)        */
    0x15,
    0x00, /*   Logical Minimum (0)                      */
    0x25,
    0x01, /*   Logical Maximum (1)                      */
    0x75,
    0x01, /*   Report Size (1)                          */
    0x06,
    0x01,
    0xFF, /*   Usage Page (Apple Vendor Keyboard 0xFF01)*/
    0x09,
    0x01, /*   Usage (Spotlight)            bit 0       */
    0x09,
    0x02, /*   Usage (Dashboard)            bit 1       */
    0x09,
    0x04, /*   Usage (Launchpad)            bit 2       */
    0x09,
    0x10, /*   Usage (Expose All)           bit 3       */
    0x09,
    0x20, /*   Usage (Brightness Up)        bit 4       */
    0x09,
    0x21, /*   Usage (Brightness Down)      bit 5       */
    0x95,
    0x06, /*   Report Count (6)                         */
    0x81,
    0x02, /*   Input (Data,Var,Abs)                     */
    0x05,
    0x0C, /*   Usage Page (Consumer)                    */
    0x09,
    0xB6, /*   Usage (Scan Previous Track)  bit 6       */
    0x09,
    0xCD, /*   Usage (Play/Pause)           bit 7       */
    0x09,
    0xB5, /*   Usage (Scan Next Track)      bit 8       */
    0x09,
    0xE2, /*   Usage (Mute)                 bit 9       */
    0x09,
    0xE9, /*   Usage (Volume Increment)     bit 10      */
    0x09,
    0xEA, /*   Usage (Volume Decrement)     bit 11      */
    0x09,
    0xCF, /*   Usage (Voice Command)        bit 12      */
    0x95,
    0x07, /*   Report Count (7)                         */
    0x81,
    0x02, /*   Input (Data,Var,Abs)                     */
    0x05,
    0x01, /*   Usage Page (Generic Desktop)             */
    0x09,
    0x9B, /*   Usage (Do Not Disturb)       bit 13      */
    0x95,
    0x01, /*   Report Count (1)                         */
    0x81,
    0x02, /*   Input (Data,Var,Abs)                     */
    0x95,
    0x02, /*   Report Count (2)             pad bits    */
    0x81,
    0x03, /*   Input (Const,Var,Abs)                    */
    0xC0 /* End Collection                             */
};

bool apple_keyboard_is_supported(
    uint16_t vendor_id,
    uint16_t product_id
) {
    return (vendor_id == APPLE_KEYBOARD_USB_VENDOR_ID)
        && (apple_keyboard_layout_for_pid(product_id) != APPLE_KEYBOARD_LAYOUT_NONE);
}

uint16_t apple_keyboard_augment_descriptor(
    uint16_t product_id,
    const uint8_t * base_descriptor,
    uint16_t base_len,
    uint8_t * out_buf,
    uint16_t out_cap
) {
    const uint16_t aux_len = (uint16_t)sizeof(k_apple_keyboard_aux_descriptor);
    uint16_t total = 0U;

    if ((base_descriptor == NULL) || (out_buf == NULL)) {
        return 0U;
    }
    if (apple_keyboard_layout_for_pid(product_id) == APPLE_KEYBOARD_LAYOUT_NONE) {
        return 0U;
    }

    total = (uint16_t)(base_len + aux_len);
    if (total > out_cap) {
        return 0U;
    }

    if (base_len > 0U) {
        (void)memcpy(out_buf, base_descriptor, base_len);
    }
    (void)memcpy(&out_buf[base_len], k_apple_keyboard_aux_descriptor, aux_len);
    return total;
}

void apple_keyboard_state_init(
    apple_keyboard_state_t * state,
    uint16_t product_id
) {
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->product_id = product_id;
    state->layout = apple_keyboard_layout_for_pid(product_id);
    state->media_default = true; /* native default: top row = media, Fn = F-keys */
    state->toggle_chord_active = false;
}

static void apple_keyboard_set_aux_bit(
    uint8_t aux[2],
    uint8_t bit
) {
    if (bit >= 16U) {
        return;
    }
    aux[bit >> 3U] = (uint8_t)(aux[bit >> 3U] | (uint8_t)(1U << (bit & 0x07U)));
}

bool apple_keyboard_process_report(
    apple_keyboard_state_t * state,
    const uint8_t * report,
    uint16_t report_len,
    uint8_t * out_kbd,
    uint16_t * out_kbd_len,
    uint8_t * out_aux,
    uint16_t * out_aux_len
) {
    uint8_t aux[2] = {0U, 0U};
    bool fn_held = false;
    bool esc_held = false;
    bool media_action = false;
    uint8_t i = 0U;

    if ((state == NULL)
        || !state->initialized
        || (report == NULL)
        || (out_kbd == NULL)
        || (out_kbd_len == NULL)
        || (out_aux == NULL)
        || (out_aux_len == NULL)) {
        return false;
    }
    if ((report_len < APPLE_KEYBOARD_REPORT_MIN_LEN) || (report[0] != APPLE_KEYBOARD_REPORT_ID)) {
        return false;
    }

    (void)memcpy(out_kbd, report, report_len);
    *out_kbd_len = report_len;
    *out_aux_len = 0U;

    fn_held = (report[APPLE_KEYBOARD_STATUS_BYTE] & APPLE_KEYBOARD_FN_BIT_MASK) != 0U;
    /* Media action engages when default mode is active without Fn, or when Fn
     * inverts a non-default mode -- i.e. exactly when (media_default XOR Fn). */
    media_action = (state->media_default != fn_held);

    for (i = APPLE_KEYBOARD_KEYCODE_FIRST; i <= APPLE_KEYBOARD_KEYCODE_LAST; i++) {
        const uint8_t key = report[i];

        if (key == APPLE_KEYBOARD_KEY_ESC) {
            esc_held = true;
            /* Fn+Esc is our private mode toggle; never forward that Esc. */
            if (fn_held) {
                out_kbd[i] = 0U;
            }
            continue;
        }

        if ((key >= APPLE_KEYBOARD_KEY_F1) && (key <= APPLE_KEYBOARD_KEY_F12)) {
            const uint8_t idx = (uint8_t)(key - APPLE_KEYBOARD_KEY_F1);
            uint8_t bit = k_apple_keyboard_fkey_aux[idx];

            if (!media_action) {
                continue; /* deliver the literal F-key */
            }
            if (idx == 3U) { /* F4 is layout-specific */
                bit = apple_keyboard_f4_aux(state->layout);
            }
            apple_keyboard_set_aux_bit(aux, bit);
            out_kbd[i] = 0U; /* remove from the keyboard report */
        }
    }

    /* Fn+Esc rising edge toggles the top-row mode. */
    if (fn_held && esc_held) {
        if (!state->toggle_chord_active) {
            state->media_default = !state->media_default;
        }
        state->toggle_chord_active = true;
    } else {
        state->toggle_chord_active = false;
    }

    /* Emit the aux report only when it changed, so each mapped key produces one
     * press and one release. */
    if ((aux[0] != state->prev_aux[0]) || (aux[1] != state->prev_aux[1])) {
        out_aux[0] = APPLE_KEYBOARD_AUX_REPORT_ID;
        out_aux[1] = aux[0];
        out_aux[2] = aux[1];
        *out_aux_len = APPLE_KEYBOARD_AUX_REPORT_LEN;
        state->prev_aux[0] = aux[0];
        state->prev_aux[1] = aux[1];
    }

    return true;
}
