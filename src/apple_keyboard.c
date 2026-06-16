#include "apple_keyboard.h"

#include <stddef.h>
#include <string.h>

/* USB-form Apple vendor ID used to recognize Apple keyboards. */
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
    APPLE_KEYBOARD_EJECT_BIT_MASK = 0x01U, /* status byte bit 0 = Eject (Consumer 0xB8) */
    APPLE_KEYBOARD_FN_BIT_MASK = 0x02U
};

/*
 * HID keyboard usage IDs for the top row and the mode-toggle key, plus the
 * keys/modifiers synthesized for top-row functions that have no usage macOS
 * honors from a non-Apple device (see k_apple_keyboard_chords below).
 */
enum {
    APPLE_KEYBOARD_KEY_F1 = 0x3AU,
    APPLE_KEYBOARD_KEY_F12 = 0x45U,
    APPLE_KEYBOARD_KEY_ESC = 0x29U,
    APPLE_KEYBOARD_KEY_DELETE_BACKSPACE = 0x2AU,
    APPLE_KEYBOARD_KEY_DELETE_FORWARD = 0x4CU,
    APPLE_KEYBOARD_KEY_UP_ARROW = 0x52U, /* Mission Control chord key   */
    APPLE_KEYBOARD_KEY_L = 0x0FU, /* Launchpad chord key         */
    APPLE_KEYBOARD_MOD_LEFT_CTRL = 0x01U, /* report byte 1, bit 0        */
    APPLE_KEYBOARD_MOD_LEFT_ALT = 0x04U, /* Option, bit 2               */
    APPLE_KEYBOARD_MOD_LEFT_GUI = 0x08U /* Command, bit 3              */
};

/*
 * Bit positions in the 2-byte aux payload. The order here must match the field
 * declaration order in the augmentation descriptor below. macOS ignores the
 * Apple-vendor page (bits 0-3) from a non-Apple device, so brightness uses the
 * standard Consumer page (bits 4-5), and Mission Control/Expose (bit 3) and
 * Launchpad (bit 2) are synthesized as keyboard chords instead of emitted as aux
 * usages (see k_apple_keyboard_chords). Spotlight/Dashboard (bits 0-1, other
 * layouts' F4) have no generic equivalent and stay inert.
 */
enum {
    AUX_BIT_SPOTLIGHT = 0, /* inert: Apple-vendor page, ignored by macOS */
    AUX_BIT_DASHBOARD = 1, /* inert */
    AUX_BIT_LAUNCHPAD = 2, /* chord: Ctrl+Opt+Cmd+L (user-bound) */
    AUX_BIT_EXPOSE = 3, /* chord: Ctrl+Up (Mission Control) */
    AUX_BIT_BRIGHT_UP = 4, /* Consumer 0x6F */
    AUX_BIT_BRIGHT_DOWN = 5, /* Consumer 0x70 */
    AUX_BIT_SCAN_PREV = 6,
    AUX_BIT_PLAY_PAUSE = 7,
    AUX_BIT_SCAN_NEXT = 8,
    AUX_BIT_MUTE = 9,
    AUX_BIT_VOL_UP = 10,
    AUX_BIT_VOL_DOWN = 11,
    AUX_BIT_VOICE = 12,
    AUX_BIT_DND = 13,
    AUX_BIT_LOCK = 14, /* top-right Eject key remapped to lock screen */
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
 * Top-row functions with no usage macOS honors from a non-Apple device are
 * emitted as keyboard chords (modifiers + key) instead of aux usages. Mission
 * Control uses its built-in Ctrl+Up shortcut; Launchpad has no default shortcut,
 * so it sends Ctrl+Opt+Cmd+L for the user to bind to "Show Launchpad" in System
 * Settings. Any aux bit not listed here is emitted as a normal aux usage.
 */
typedef struct {
    uint8_t aux_bit;
    uint8_t modifiers;
    uint8_t keycode;
} apple_keyboard_chord_t;

static const apple_keyboard_chord_t k_apple_keyboard_chords[] = {
    {AUX_BIT_EXPOSE, APPLE_KEYBOARD_MOD_LEFT_CTRL, APPLE_KEYBOARD_KEY_UP_ARROW},
    {AUX_BIT_LAUNCHPAD,
        (uint8_t)(APPLE_KEYBOARD_MOD_LEFT_CTRL
            | APPLE_KEYBOARD_MOD_LEFT_ALT
            | APPLE_KEYBOARD_MOD_LEFT_GUI),
        APPLE_KEYBOARD_KEY_L}
};

static const apple_keyboard_chord_t * apple_keyboard_chord_for_bit(uint8_t bit) {
    size_t n = 0U;

    for (n = 0U; n < (sizeof(k_apple_keyboard_chords) / sizeof(k_apple_keyboard_chords[0])); n++) {
        if (k_apple_keyboard_chords[n].aux_bit == bit) {
            return &k_apple_keyboard_chords[n];
        }
    }
    return NULL;
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
    0x95,
    0x04, /*   Report Count (4)                         */
    0x81,
    0x02, /*   Input (Data,Var,Abs)                     */
    0x05,
    0x0C, /*   Usage Page (Consumer)                    */
    0x09,
    0x6F, /*   Usage (Brightness Increment) bit 4       */
    0x09,
    0x70, /*   Usage (Brightness Decrement) bit 5       */
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
    0x09, /*   Report Count (9)                         */
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
    0x05,
    0x0C, /*   Usage Page (Consumer)                    */
    0x0A,
    0x9E,
    0x01, /*   Usage (AL Terminal Lock)     bit 14      */
    0x95,
    0x01, /*   Report Count (1)                         */
    0x81,
    0x02, /*   Input (Data,Var,Abs)                     */
    0x95,
    0x01, /*   Report Count (1)             pad bit     */
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
    bool delete_backspace_held = false;
    bool media_action = false;
    uint8_t i = 0U;
    const apple_keyboard_chord_t * chord = NULL;

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
            /*
             * Fn+Esc is our private mode toggle; that Esc must never reach the
             * host. Suppression latches for the whole physical Esc press: the
             * keyboard does not transition the Fn status bit and the Esc keycode
             * atomically, so on chord release the Fn bit can clear a report
             * before Esc leaves the keycode array. Keying suppression off the
             * current report's Fn bit alone would leak that trailing Esc as a
             * real Escape (an intermittent keystroke into the focused app).
             */
            if (fn_held || state->esc_suppress_latched) {
                out_kbd[i] = 0U;
                state->esc_suppress_latched = true;
            }
            continue;
        }

        if (key == APPLE_KEYBOARD_KEY_DELETE_BACKSPACE) {
            delete_backspace_held = true;
        }

        if ((key == APPLE_KEYBOARD_KEY_DELETE_BACKSPACE)
            && (fn_held || state->delete_forward_latched)) {
            out_kbd[i] = APPLE_KEYBOARD_KEY_DELETE_FORWARD;
            state->delete_forward_latched = true;
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
            /*
             * Functions with no usage macOS honors from a non-Apple device are
             * synthesized as keyboard chords (modifiers + key) in the keyboard
             * report rather than emitted as aux usages.
             */
            chord = apple_keyboard_chord_for_bit(bit);
            if (chord != NULL) {
                out_kbd[1] = (uint8_t)(out_kbd[1] | chord->modifiers);
                out_kbd[i] = chord->keycode;
                continue;
            }
            apple_keyboard_set_aux_bit(aux, bit);
            out_kbd[i] = 0U; /* remove from the keyboard report */
        }
    }

    /* Top-right Eject key acts as the lock screen key (Fn+Eject = real Eject). */
    if ((report[APPLE_KEYBOARD_STATUS_BYTE] & APPLE_KEYBOARD_EJECT_BIT_MASK) != 0U) {
        if (media_action) {
            apple_keyboard_set_aux_bit(aux, AUX_BIT_LOCK);
            out_kbd[APPLE_KEYBOARD_STATUS_BYTE] =
                (uint8_t)(out_kbd[APPLE_KEYBOARD_STATUS_BYTE] & ~APPLE_KEYBOARD_EJECT_BIT_MASK);
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

    /* Release the Esc-suppression latch once the key is physically up. */
    if (!esc_held) {
        state->esc_suppress_latched = false;
    }
    if (!delete_backspace_held) {
        state->delete_forward_latched = false;
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
