#include "apple_trackpad.h"

#include <stddef.h>
#include <string.h>

/* USB-form Apple vendor ID used to recognize Apple trackpads. */
#define APPLE_TRACKPAD_USB_VENDOR_ID 0x05ACU

/*
 * Decode families. Magic Trackpad 2 and the USB-C Magic Trackpad share the
 * second-generation frame layout; the original Magic Trackpad (A1339) uses
 * the first-generation one.
 */
enum {
    APPLE_TRACKPAD_FAMILY_NONE = 0U,
    APPLE_TRACKPAD_FAMILY_MT1 = 1U, /* A1339 (PID 0x030E) */
    APPLE_TRACKPAD_FAMILY_MT2 = 2U /* A1535 (0x0265), USB-C (0x0324) */
};

/*
 * Vendor multitouch frame layout (both families): report ID, 4-byte prefix
 * whose byte 1 bit 0 is the physical button, then 9 bytes per touch.
 */
enum {
    APPLE_TRACKPAD_MT1_REPORT_ID = 0x28U,
    APPLE_TRACKPAD_MT2_REPORT_ID = 0x31U,
    APPLE_TRACKPAD_DOUBLE_REPORT_ID = 0xF7U, /* wraps two concatenated frames */
    APPLE_TRACKPAD_FRAME_PREFIX_LEN = 4U,
    APPLE_TRACKPAD_TOUCH_RECORD_LEN = 9U,
    APPLE_TRACKPAD_BUTTON_BYTE = 1U,
    /* MT2 per-touch state lives in record byte 3 bits 6-7; 0x80 = touching. */
    APPLE_TRACKPAD_MT2_STATE_MASK = 0xC0U,
    APPLE_TRACKPAD_MT2_STATE_DOWN = 0x80U,
    /* MT1 per-touch state lives in record byte 8 bits 4-7; 0 = lifted. */
    APPLE_TRACKPAD_MT1_STATE_MASK = 0xF0U
};

/*
 * Sensor units are ~47 per millimeter on both families. The pointer divider
 * yields ~8 counts/mm (a ~200 dpi mouse before host acceleration -- a full
 * pad swipe crosses roughly one screen); the scroll divider yields one
 * wheel detent per ~0.5 mm of finger travel. Hardware-tuned 2026-06-10:
 * the original 2/64 pair made the pointer wildly fast next to a sluggish
 * scroll.
 *
 * The per-frame delta limit rejects motion that is physically implausible
 * within one ~11 ms frame (~2.3 m/s): the trackpad occasionally reuses a
 * touch id for a brand-new contact with no lift frame in between, and the
 * resulting position step must not become a pointer jump.
 */
enum {
    APPLE_TRACKPAD_POINTER_DIV = 3,
    /* Wheel counts are quarter-quanta: the synthesized collection declares
     * 4x the native scroll resolution (see the wheel physical range). */
    APPLE_TRACKPAD_SCROLL_DIV = 3,
    APPLE_TRACKPAD_MAX_FRAME_DELTA = 1200
};

/*
 * Scroll inertia. macOS synthesizes momentum only for Apple's own
 * multitouch devices, never for HID wheels, so the relay does it: while a
 * two-finger scroll runs, finger velocity is tracked as a running average
 * in raw touch units per frame; lifting off within MOMENTUM_RECENT_MS at
 * MOMENTUM_MIN_VELOCITY or faster arms a tail that keeps scrolling every
 * MOMENTUM_STEP_MS, decaying by MOMENTUM_DECAY/256 per step until it falls
 * below MOMENTUM_FLOOR (about a one-second tail from a strong flick).
 */
enum {
    APPLE_TRACKPAD_MOMENTUM_STEP_MS = 10,
    APPLE_TRACKPAD_MOMENTUM_DECAY = 251, /* per-step factor, /256 (~1.5 s tail) */
    APPLE_TRACKPAD_MOMENTUM_MIN_VELOCITY = 24, /* raw units per frame */
    APPLE_TRACKPAD_MOMENTUM_FLOOR = 4, /* raw units per step */
    APPLE_TRACKPAD_MOMENTUM_RECENT_MS = 100,
    APPLE_TRACKPAD_MOMENTUM_MAX_STEPS = 8, /* catch-up cap per tick */
    /* Tail launch boost: the tracked average lags an accelerating flick,
     * so the tail starts at 3/2 of it -- close to the true lift velocity,
     * matching the native driver's strong initial kick. */
    APPLE_TRACKPAD_MOMENTUM_KICK_NUM = 3,
    APPLE_TRACKPAD_MOMENTUM_KICK_DEN = 2
};

/*
 * Velocity gain curve for pointer and scroll, approximating the native
 * driver's response: sub-linear below typical deliberate speed (slow
 * finger = precision), unity near it (so absolute calibration of the
 * POINTER/SCROLL dividers holds), super-linear toward flick speed. Gain
 * is Q6 fixed point (64 = 1.0x), linear in per-frame speed between the
 * SLOW and FAST anchors and clamped outside them.
 */
enum {
    APPLE_TRACKPAD_GAIN_SLOW_SPEED = 8, /* raw units per frame */
    APPLE_TRACKPAD_GAIN_FAST_SPEED = 200,
    APPLE_TRACKPAD_GAIN_MIN_Q6 = 24, /* 0.375x */
    APPLE_TRACKPAD_GAIN_MAX_Q6 = 120 /* 1.875x */
};

/* Per-episode scroll axis lock: near-axis two-finger scrolling sticks to
 * its dominant axis (the native behavior); only clearly diagonal motion
 * scrolls both axes. Locked when one axis' accumulated travel reaches
 * AXIS_LOCK_RATIO times the other's at classification time. */
enum {
    APPLE_TRACKPAD_AXIS_FREE = 0,
    APPLE_TRACKPAD_AXIS_VERTICAL = 1,
    APPLE_TRACKPAD_AXIS_HORIZONTAL = 2,
    APPLE_TRACKPAD_AXIS_LOCK_RATIO = 3
};

enum {
    APPLE_TRACKPAD_BUTTON_LEFT = 0x01U,
    APPLE_TRACKPAD_BUTTON_RIGHT = 0x02U,
    APPLE_TRACKPAD_BUTTON_MIDDLE = 0x04U
};

/*
 * Tap thresholds: a touch episode becomes a tap when every finger lifts
 * within the time limit, total travel stayed under ~2 mm, and the physical
 * button was never pressed. The synthesized click is released by time
 * (apple_trackpad_tick) since no input frames arrive after liftoff.
 */
enum {
    APPLE_TRACKPAD_TAP_MAX_MS = 250,
    APPLE_TRACKPAD_TAP_TRAVEL_LIMIT = 100,
    APPLE_TRACKPAD_TAP_PULSE_MS = 30
};

/* Two-finger mode: undecided until enough motion to classify the episode. */
enum {
    APPLE_TRACKPAD_TWO_FINGER_UNDECIDED = 0U,
    APPLE_TRACKPAD_TWO_FINGER_SCROLL = 1U,
    APPLE_TRACKPAD_TWO_FINGER_PINCH = 2U
};

/*
 * Chord-gesture thresholds. Classification fires after ~2.5 mm of dominant
 * motion; a pinch emits one zoom step per ~5 mm of spread change; a
 * three-finger swipe fires its shortcut after ~13 mm of travel.
 */
enum {
    APPLE_TRACKPAD_TWO_FINGER_CLASSIFY_THRESHOLD = 120,
    APPLE_TRACKPAD_PINCH_STEP = 250,
    APPLE_TRACKPAD_SWIPE_THRESHOLD = 600
};

/* HID keyboard usages/modifiers for synthesized gesture chords. */
enum {
    APPLE_TRACKPAD_KEY_RIGHT_ARROW = 0x4FU,
    APPLE_TRACKPAD_KEY_LEFT_ARROW = 0x50U,
    APPLE_TRACKPAD_KEY_DOWN_ARROW = 0x51U,
    APPLE_TRACKPAD_KEY_UP_ARROW = 0x52U,
    APPLE_TRACKPAD_KEY_EQUAL = 0x2EU, /* '=' / '+' */
    APPLE_TRACKPAD_KEY_MINUS = 0x2DU,
    APPLE_TRACKPAD_MOD_LEFT_CTRL = 0x01U,
    APPLE_TRACKPAD_MOD_LEFT_GUI = 0x08U /* Command */
};

static uint8_t apple_trackpad_family_for_pid(uint16_t product_id) {
    switch (product_id) {
        case 0x030EU: /* Magic Trackpad (A1339) */
            return APPLE_TRACKPAD_FAMILY_MT1;
        case 0x0265U: /* Magic Trackpad 2 (A1535) */
        case 0x0324U: /* Magic Trackpad USB-C (2024) */
            return APPLE_TRACKPAD_FAMILY_MT2;
        default:
            return APPLE_TRACKPAD_FAMILY_NONE;
    }
}

bool apple_trackpad_is_supported(
    uint16_t vendor_id,
    uint16_t product_id
) {
    return (vendor_id == APPLE_TRACKPAD_USB_VENDOR_ID)
        && (apple_trackpad_family_for_pid(product_id) != APPLE_TRACKPAD_FAMILY_NONE);
}

bool apple_trackpad_is_vendor_report(
    uint16_t product_id,
    uint8_t report_id
) {
    switch (apple_trackpad_family_for_pid(product_id)) {
        case APPLE_TRACKPAD_FAMILY_MT1:
            return (report_id == APPLE_TRACKPAD_MT1_REPORT_ID)
                || (report_id == APPLE_TRACKPAD_DOUBLE_REPORT_ID);
        case APPLE_TRACKPAD_FAMILY_MT2:
            return (report_id == APPLE_TRACKPAD_MT2_REPORT_ID)
                || (report_id == APPLE_TRACKPAD_DOUBLE_REPORT_ID);
        default:
            return false;
    }
}

bool apple_trackpad_mt_enable_report(
    uint16_t product_id,
    uint8_t * out_report_id,
    uint8_t out_payload[4],
    uint8_t * out_payload_len
) {
    if ((out_report_id == NULL) || (out_payload == NULL) || (out_payload_len == NULL)) {
        return false;
    }

    switch (apple_trackpad_family_for_pid(product_id)) {
        case APPLE_TRACKPAD_FAMILY_MT1:
            /* Feature report 0xD7 = 0x01. */
            *out_report_id = 0xD7U;
            out_payload[0] = 0x01U;
            *out_payload_len = 1U;
            return true;
        case APPLE_TRACKPAD_FAMILY_MT2:
            /* Feature report 0xF1 = 0x02 0x01 (Bluetooth transport form). */
            *out_report_id = 0xF1U;
            out_payload[0] = 0x02U;
            out_payload[1] = 0x01U;
            *out_payload_len = 2U;
            return true;
        default:
            return false;
    }
}

/*
 * Mouse collection appended to the trackpad's report descriptor. Everything
 * the gesture engine emits in its mouse report must be declared here: three
 * buttons, 16-bit relative X/Y, vertical wheel, and horizontal AC Pan.
 */
static const uint8_t k_apple_trackpad_mouse_descriptor[] = {
    0x05,
    0x01, /* Usage Page (Generic Desktop)        */
    0x09,
    0x02, /* Usage (Mouse)                       */
    0xA1,
    0x01, /* Collection (Application)            */
    0x85,
    APPLE_TRACKPAD_MOUSE_REPORT_ID, /* Report ID (0xB1)  */
    0x09,
    0x01, /*   Usage (Pointer)                   */
    0xA1,
    0x00, /*   Collection (Physical)             */
    0x05,
    0x09, /*     Usage Page (Button)             */
    0x19,
    0x01, /*     Usage Minimum (1)               */
    0x29,
    0x03, /*     Usage Maximum (3)               */
    0x15,
    0x00, /*     Logical Minimum (0)             */
    0x25,
    0x01, /*     Logical Maximum (1)             */
    0x75,
    0x01, /*     Report Size (1)                 */
    0x95,
    0x03, /*     Report Count (3)                */
    0x81,
    0x02, /*     Input (Data,Var,Abs)            */
    0x75,
    0x05, /*     Report Size (5)                 */
    0x95,
    0x01, /*     Report Count (1)    pad         */
    0x81,
    0x03, /*     Input (Const,Var,Abs)           */
    0x05,
    0x01, /*     Usage Page (Generic Desktop)    */
    0x09,
    0x30, /*     Usage (X)                       */
    0x09,
    0x31, /*     Usage (Y)                       */
    /*
     * Physical Min/Max, Unit and Unit Exponent are HID GLOBAL items: they
     * persist across the whole descriptor, and this collection is appended
     * after the trackpad's native one, which declares its +/-127 X/Y as
     * +/-317 thousandths of an inch (~400 dpi). Inherited by our 16-bit
     * range they declare a ~103000 dpi device, which the macOS pointer
     * pipeline turns into wild constant-step cursor jumps. Reset all four
     * to zero so the logical counts pass through 1:1 at the host's default
     * pointer resolution.
     */
    0x35,
    0x00, /*     Physical Minimum (0 = logical)  */
    0x45,
    0x00, /*     Physical Maximum (0 = logical)  */
    0x65,
    0x00, /*     Unit (None)                     */
    0x55,
    0x00, /*     Unit Exponent (0)               */
    0x16,
    0x01,
    0x80, /*     Logical Minimum (-32767)        */
    0x26,
    0xFF,
    0x7F, /*     Logical Maximum (32767)         */
    0x75,
    0x10, /*     Report Size (16)                */
    0x95,
    0x02, /*     Report Count (2)                */
    0x81,
    0x06, /*     Input (Data,Var,Rel)            */
    0x09,
    0x38, /*     Usage (Wheel)                   */
    /*
     * Deliberately declare a physical range and unit (the native
     * collection's inch declaration, quartered): wheel counts then read as
     * ~1600 per inch, which macOS treats as a high-resolution scroll
     * device. Four times the native 400/inch so each count is a quarter
     * quantum: slow scrolling and the decaying end of a momentum tail step
     * finely instead of in visible per-count jumps (SCROLL_DIV emits 4x
     * the counts to keep the same on-screen speed). Applies to AC Pan
     * below too.
     */
    0x35,
    0xB1, /*     Physical Minimum (-79)          */
    0x45,
    0x4F, /*     Physical Maximum (79)           */
    0x65,
    0x13, /*     Unit (Inch)                     */
    0x55,
    0x0D, /*     Unit Exponent (-3)              */
    0x15,
    0x81, /*     Logical Minimum (-127)          */
    0x25,
    0x7F, /*     Logical Maximum (127)           */
    0x75,
    0x08, /*     Report Size (8)                 */
    0x95,
    0x01, /*     Report Count (1)                */
    0x81,
    0x06, /*     Input (Data,Var,Rel)            */
    0x05,
    0x0C, /*     Usage Page (Consumer)           */
    0x0A,
    0x38,
    0x02, /*     Usage (AC Pan)                  */
    0x15,
    0x81, /*     Logical Minimum (-127)          */
    0x25,
    0x7F, /*     Logical Maximum (127)           */
    0x75,
    0x08, /*     Report Size (8)                 */
    0x95,
    0x01, /*     Report Count (1)                */
    0x81,
    0x06, /*     Input (Data,Var,Rel)            */
    0xC0, /*   End Collection                    */
    0xC0 /* End Collection                      */
};

/*
 * Keyboard collection for synthesized gesture chords (three-finger swipes
 * map to the Mission Control / Spaces shortcuts, pinch to Command +/-).
 * One modifier byte plus a single key array slot.
 */
static const uint8_t k_apple_trackpad_chord_descriptor[] = {
    0x05,
    0x01, /* Usage Page (Generic Desktop)        */
    0x09,
    0x06, /* Usage (Keyboard)                    */
    0xA1,
    0x01, /* Collection (Application)            */
    0x85,
    APPLE_TRACKPAD_CHORD_REPORT_ID, /* Report ID (0xB2)  */
    0x05,
    0x07, /*   Usage Page (Keyboard/Keypad)      */
    0x19,
    0xE0, /*   Usage Minimum (Left Control)      */
    0x29,
    0xE7, /*   Usage Maximum (Right GUI)         */
    0x15,
    0x00, /*   Logical Minimum (0)               */
    0x25,
    0x01, /*   Logical Maximum (1)               */
    0x75,
    0x01, /*   Report Size (1)                   */
    0x95,
    0x08, /*   Report Count (8)                  */
    0x81,
    0x02, /*   Input (Data,Var,Abs)  modifiers   */
    0x19,
    0x00, /*   Usage Minimum (0)                 */
    0x29,
    0xFF, /*   Usage Maximum (255)               */
    0x15,
    0x00, /*   Logical Minimum (0)               */
    0x26,
    0xFF,
    0x00, /*   Logical Maximum (255)             */
    0x75,
    0x08, /*   Report Size (8)                   */
    0x95,
    0x01, /*   Report Count (1)                  */
    0x81,
    0x00, /*   Input (Data,Array)    key slot    */
    0xC0 /* End Collection                      */
};

uint16_t apple_trackpad_augment_descriptor(
    uint16_t product_id,
    const uint8_t * base_descriptor,
    uint16_t base_len,
    uint8_t * out_buf,
    uint16_t out_cap
) {
    const uint16_t mouse_len = (uint16_t)sizeof(k_apple_trackpad_mouse_descriptor);
    const uint16_t chord_len = (uint16_t)sizeof(k_apple_trackpad_chord_descriptor);
    uint16_t total = 0U;

    if ((base_descriptor == NULL) || (out_buf == NULL)) {
        return 0U;
    }
    if (apple_trackpad_family_for_pid(product_id) == APPLE_TRACKPAD_FAMILY_NONE) {
        return 0U;
    }

    total = (uint16_t)(base_len + mouse_len + chord_len);
    if (total > out_cap) {
        return 0U;
    }

    if (base_len > 0U) {
        (void)memcpy(out_buf, base_descriptor, base_len);
    }
    (void)memcpy(&out_buf[base_len], k_apple_trackpad_mouse_descriptor, mouse_len);
    (void)memcpy(&out_buf[base_len + mouse_len], k_apple_trackpad_chord_descriptor, chord_len);
    return total;
}

void apple_trackpad_state_init(
    apple_trackpad_state_t * state,
    uint16_t product_id
) {
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
    state->initialized = true;
    state->product_id = product_id;
    state->family = apple_trackpad_family_for_pid(product_id);
}

static int16_t apple_trackpad_sign_extend_13(uint16_t raw) {
    uint16_t value = (uint16_t)(raw & 0x1FFFU);

    if ((value & 0x1000U) != 0U) {
        value = (uint16_t)(value | 0xE000U);
    }
    return (int16_t)value;
}

/* One decoded touch record. */
typedef struct {
    uint8_t id;
    bool down;
    int16_t x;
    int16_t y;
} apple_trackpad_record_t;

/*
 * Decode one 9-byte touch record. Bit layout per the hid-magicmouse
 * protocol: x is a signed 13-bit field across bytes 0-1, y a signed 13-bit
 * field across bytes 1-3 (negated so positive y points toward the user,
 * matching HID mouse +Y). Touch id and state placement differ per family.
 */
static void apple_trackpad_decode_record(
    uint8_t family,
    const uint8_t * tdata,
    apple_trackpad_record_t * out
) {
    const uint16_t raw_x = (uint16_t)(tdata[0] | (uint16_t)((uint16_t)(tdata[1] & 0x1FU) << 8U));
    const uint16_t raw_y = (uint16_t)((tdata[1] >> 5U)
        | (uint16_t)((uint16_t)tdata[2] << 3U)
        | (uint16_t)((uint16_t)(tdata[3] & 0x03U) << 11U));

    out->x = apple_trackpad_sign_extend_13(raw_x);
    out->y = (int16_t)-apple_trackpad_sign_extend_13(raw_y);

    if (family == APPLE_TRACKPAD_FAMILY_MT2) {
        out->id = (uint8_t)(tdata[8] & 0x0FU);
        out->down = (tdata[3] & APPLE_TRACKPAD_MT2_STATE_MASK) == APPLE_TRACKPAD_MT2_STATE_DOWN;
    } else {
        out->id = (uint8_t)(((uint16_t)((uint16_t)tdata[7] << 2U) | (tdata[6] >> 6U)) & 0x0FU);
        out->down = (tdata[8] & APPLE_TRACKPAD_MT1_STATE_MASK) != 0U;
    }
}

static uint8_t apple_trackpad_report_id_for_family(uint8_t family) {
    return (family == APPLE_TRACKPAD_FAMILY_MT2) ? APPLE_TRACKPAD_MT2_REPORT_ID
                                                 : APPLE_TRACKPAD_MT1_REPORT_ID;
}

static void apple_trackpad_out_push(
    apple_trackpad_out_t * out,
    const uint8_t * bytes,
    uint8_t len
) {
    if ((out->count >= APPLE_TRACKPAD_MAX_OUT_REPORTS)
        || (len > APPLE_TRACKPAD_OUT_REPORT_MAX_LEN)) {
        return;
    }
    (void)memcpy(out->bytes[out->count], bytes, len);
    out->len[out->count] = len;
    out->count = (uint8_t)(out->count + 1U);
}

static int8_t apple_trackpad_clamp_i8(int32_t value) {
    if (value > 127) {
        return 127;
    }
    if (value < -127) {
        return -127;
    }
    return (int8_t)value;
}

static int16_t apple_trackpad_clamp_i16(int32_t value) {
    if (value > 32767) {
        return 32767;
    }
    if (value < -32767) {
        return -32767;
    }
    return (int16_t)value;
}

static void apple_trackpad_emit_mouse(
    apple_trackpad_state_t * state,
    apple_trackpad_out_t * out,
    uint8_t buttons,
    int32_t dx,
    int32_t dy,
    int32_t wheel,
    int32_t pan
) {
    uint8_t report[APPLE_TRACKPAD_MOUSE_REPORT_LEN] = {0};
    const int16_t x16 = apple_trackpad_clamp_i16(dx);
    const int16_t y16 = apple_trackpad_clamp_i16(dy);

    if ((buttons == state->buttons) && (dx == 0) && (dy == 0) && (wheel == 0) && (pan == 0)) {
        return;
    }

    report[0] = APPLE_TRACKPAD_MOUSE_REPORT_ID;
    report[1] = buttons;
    report[2] = (uint8_t)((uint16_t)x16 & 0xFFU);
    report[3] = (uint8_t)(((uint16_t)x16 >> 8U) & 0xFFU);
    report[4] = (uint8_t)((uint16_t)y16 & 0xFFU);
    report[5] = (uint8_t)(((uint16_t)y16 >> 8U) & 0xFFU);
    report[6] = (uint8_t)apple_trackpad_clamp_i8(wheel);
    report[7] = (uint8_t)apple_trackpad_clamp_i8(pan);
    apple_trackpad_out_push(out, report, (uint8_t)sizeof(report));
    state->buttons = buttons;
}

static int32_t apple_trackpad_abs_i32(int32_t value) {
    return (value < 0) ? -value : value;
}

/* Q6 velocity gain for a per-frame speed (|dx| + |dy| in raw units). */
static int32_t apple_trackpad_speed_gain_q6(int32_t speed) {
    if (speed <= APPLE_TRACKPAD_GAIN_SLOW_SPEED) {
        return APPLE_TRACKPAD_GAIN_MIN_Q6;
    }
    if (speed >= APPLE_TRACKPAD_GAIN_FAST_SPEED) {
        return APPLE_TRACKPAD_GAIN_MAX_Q6;
    }
    return APPLE_TRACKPAD_GAIN_MIN_Q6
        + (((speed - APPLE_TRACKPAD_GAIN_SLOW_SPEED)
               * (APPLE_TRACKPAD_GAIN_MAX_Q6 - APPLE_TRACKPAD_GAIN_MIN_Q6))
            / (APPLE_TRACKPAD_GAIN_FAST_SPEED - APPLE_TRACKPAD_GAIN_SLOW_SPEED));
}

/* Emit a gesture chord as an immediate press + release pair. */
static void apple_trackpad_emit_chord(
    apple_trackpad_out_t * out,
    uint8_t modifiers,
    uint8_t keycode
) {
    uint8_t report[APPLE_TRACKPAD_CHORD_REPORT_LEN] = {0};

    report[0] = APPLE_TRACKPAD_CHORD_REPORT_ID;
    report[1] = modifiers;
    report[2] = keycode;
    apple_trackpad_out_push(out, report, (uint8_t)sizeof(report));
    report[1] = 0U;
    report[2] = 0U;
    apple_trackpad_out_push(out, report, (uint8_t)sizeof(report));
}

/* One finger taps/clicks left, two right, three or more middle. */
static uint8_t apple_trackpad_buttons_for_fingers(uint8_t fingers) {
    if (fingers >= 3U) {
        return APPLE_TRACKPAD_BUTTON_MIDDLE;
    }
    if (fingers == 2U) {
        return APPLE_TRACKPAD_BUTTON_RIGHT;
    }
    return APPLE_TRACKPAD_BUTTON_LEFT;
}

static void apple_trackpad_flush_tap_release(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out,
    bool force
) {
    if (state->pending_release_buttons == 0U) {
        return;
    }
    if (!force && ((int32_t)(now_ms - state->tap_release_deadline_ms) < 0)) {
        return;
    }
    state->pending_release_buttons = 0U;
    apple_trackpad_emit_mouse(state, out, 0U, 0, 0, 0, 0);
}

/*
 * Validate the frame and decode its touch records. Returns false when the
 * report is not this trackpad's multitouch frame (caller forwards it raw).
 */
static bool apple_trackpad_parse_frame(
    const apple_trackpad_state_t * state,
    const uint8_t * report,
    uint16_t report_len,
    apple_trackpad_record_t * record,
    uint8_t * out_record_count,
    uint8_t * out_finger_count
) {
    uint16_t touch_bytes = 0U;
    uint8_t record_count = 0U;
    uint8_t finger_count = 0U;
    uint8_t i = 0U;

    if ((report_len < APPLE_TRACKPAD_FRAME_PREFIX_LEN)
        || (report[0] != apple_trackpad_report_id_for_family(state->family))) {
        return false;
    }
    touch_bytes = (uint16_t)(report_len - APPLE_TRACKPAD_FRAME_PREFIX_LEN);
    if ((touch_bytes % APPLE_TRACKPAD_TOUCH_RECORD_LEN) != 0U) {
        return false;
    }

    record_count = (uint8_t)(touch_bytes / APPLE_TRACKPAD_TOUCH_RECORD_LEN);
    if (record_count > APPLE_TRACKPAD_MAX_TOUCH) {
        record_count = APPLE_TRACKPAD_MAX_TOUCH;
    }

    for (i = 0U; i < record_count; i++) {
        apple_trackpad_decode_record(
            state->family,
            &report
                [APPLE_TRACKPAD_FRAME_PREFIX_LEN + ((uint16_t)i * APPLE_TRACKPAD_TOUCH_RECORD_LEN)],
            &record[i]
        );
        if (record[i].down) {
            finger_count = (uint8_t)(finger_count + 1U);
        }
    }

    *out_record_count = record_count;
    *out_finger_count = finger_count;
    return true;
}

/*
 * Physical click, mapped by resting finger count (one = left, two = right,
 * three = middle) and latched for the whole press so lifting a finger
 * mid-press cannot morph the button.
 */
static uint8_t apple_trackpad_map_click(
    apple_trackpad_state_t * state,
    const uint8_t * report,
    uint8_t finger_count
) {
    if ((report[APPLE_TRACKPAD_BUTTON_BYTE] & 0x01U) != 0U) {
        if (state->click_buttons == 0U) {
            state->click_buttons = apple_trackpad_buttons_for_fingers(finger_count);
        }
        return state->click_buttons;
    }
    state->click_buttons = 0U;
    return 0U;
}

/*
 * Sum per-finger motion against the previous frame's touch table; returns
 * the number of matched fingers. A finger-count change instead resets all
 * motion and gesture accumulators and reports no motion: the frame where a
 * finger lands or lifts otherwise injects a large bogus delta (e.g. the
 * pointer jumping when a second finger starts a scroll).
 */
static uint8_t apple_trackpad_motion_delta(
    apple_trackpad_state_t * state,
    const apple_trackpad_record_t * record,
    uint8_t record_count,
    uint8_t finger_count,
    int32_t * sum_dx,
    int32_t * sum_dy
) {
    uint8_t matched = 0U;
    uint8_t i = 0U;

    if (finger_count != state->finger_count) {
        state->move_rem_x = 0;
        state->move_rem_y = 0;
        state->scroll_rem_x = 0;
        state->scroll_rem_y = 0;
        state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_UNDECIDED;
        state->two_finger_parallel_acc_x = 0;
        state->two_finger_parallel_acc_y = 0;
        state->two_finger_spread_acc = 0;
        state->two_finger_spread_valid = false;
        state->scroll_axis_lock = APPLE_TRACKPAD_AXIS_FREE;
        state->pinch_rem = 0;
        state->swipe_acc_x = 0;
        state->swipe_acc_y = 0;
        state->swipe_fired = false;
        return 0U;
    }

    for (i = 0U; i < record_count; i++) {
        const apple_trackpad_touch_t * prev = &state->touch[record[i].id];
        int32_t delta_x = 0;
        int32_t delta_y = 0;

        if (!record[i].down || !prev->valid || !prev->down) {
            continue;
        }
        delta_x = (int32_t)record[i].x - (int32_t)prev->x;
        delta_y = (int32_t)record[i].y - (int32_t)prev->y;
        if ((apple_trackpad_abs_i32(delta_x) > APPLE_TRACKPAD_MAX_FRAME_DELTA)
            || (apple_trackpad_abs_i32(delta_y) > APPLE_TRACKPAD_MAX_FRAME_DELTA)) {
            /* Touch id reused for a new contact, not finger motion. */
            continue;
        }
        *sum_dx += delta_x;
        *sum_dy += delta_y;
        matched = (uint8_t)(matched + 1U);
    }
    return matched;
}

/* Touch-episode bookkeeping driving tap detection and momentum arming. */
static void apple_trackpad_episode_update(
    apple_trackpad_state_t * state,
    uint8_t finger_count,
    uint8_t buttons,
    uint8_t matched,
    int32_t sum_dx,
    int32_t sum_dy,
    uint32_t now_ms
) {
    if ((finger_count > 0U) && !state->touch_active) {
        state->touch_active = true;
        state->touch_started_ms = now_ms;
        state->episode_max_fingers = finger_count;
        state->episode_moved = false;
        state->episode_clicked = false;
        state->episode_travel = 0;
    } else if (finger_count > state->episode_max_fingers) {
        state->episode_max_fingers = finger_count;
    }
    if (buttons != 0U) {
        state->episode_clicked = true;
    }
    if (matched > 0U) {
        state->episode_travel +=
            (apple_trackpad_abs_i32(sum_dx) + apple_trackpad_abs_i32(sum_dy)) / (int32_t)matched;
        if (state->episode_travel > APPLE_TRACKPAD_TAP_TRAVEL_LIMIT) {
            state->episode_moved = true;
        }
    }
}

/*
 * Two-finger handling: classify the episode as scroll or pinch once, from
 * whether parallel motion or spread change dominates, then emit wheel/pan
 * counts or zoom chords. A scroll episode locks to its dominant axis (the
 * native behavior: near-vertical scrolling must not drift sideways) and
 * tracks finger velocity for the momentum tail.
 */
static void apple_trackpad_two_finger(
    apple_trackpad_state_t * state,
    const apple_trackpad_record_t * record,
    uint8_t record_count,
    uint8_t matched,
    int32_t sum_dx,
    int32_t sum_dy,
    uint32_t now_ms,
    apple_trackpad_out_t * out,
    int32_t * wheel,
    int32_t * pan
) {
    /* Runs on the touch-down frame too (matched == 0) so the spread
     * baseline exists before any motion is classified. */
    int32_t avg_dx = (matched > 0U) ? (sum_dx / (int32_t)matched) : 0;
    int32_t avg_dy = (matched > 0U) ? (sum_dy / (int32_t)matched) : 0;
    const apple_trackpad_record_t * first = NULL;
    const apple_trackpad_record_t * second = NULL;
    int32_t dspread = 0;
    uint8_t i = 0U;

    /* Spread = Manhattan distance between the two fingers; its change
     * versus parallel motion separates pinch from scroll. */
    for (i = 0U; i < record_count; i++) {
        if (!record[i].down) {
            continue;
        }
        if (first == NULL) {
            first = &record[i];
        } else {
            second = &record[i];
            break;
        }
    }
    if (second != NULL) {
        const int32_t spread = apple_trackpad_abs_i32((int32_t)first->x - (int32_t)second->x)
            + apple_trackpad_abs_i32((int32_t)first->y - (int32_t)second->y);

        if (state->two_finger_spread_valid) {
            dspread = spread - state->two_finger_prev_spread;
        }
        state->two_finger_prev_spread = spread;
        state->two_finger_spread_valid = true;
    }

    if (state->two_finger_mode == APPLE_TRACKPAD_TWO_FINGER_UNDECIDED) {
        const int32_t parallel_acc_total = state->two_finger_parallel_acc_x
            + state->two_finger_parallel_acc_y
            + apple_trackpad_abs_i32(avg_dx)
            + apple_trackpad_abs_i32(avg_dy);

        state->two_finger_parallel_acc_x += apple_trackpad_abs_i32(avg_dx);
        state->two_finger_parallel_acc_y += apple_trackpad_abs_i32(avg_dy);
        state->two_finger_spread_acc += apple_trackpad_abs_i32(dspread);
        if ((parallel_acc_total >= APPLE_TRACKPAD_TWO_FINGER_CLASSIFY_THRESHOLD)
            || (state->two_finger_spread_acc >= APPLE_TRACKPAD_TWO_FINGER_CLASSIFY_THRESHOLD)) {
            if (state->two_finger_spread_acc > parallel_acc_total) {
                state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_PINCH;
                state->scroll_rem_x = 0;
                state->scroll_rem_y = 0;
                state->pinch_rem = 0;
            } else {
                state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_SCROLL;
                /* Fresh velocity per scroll episode so a stale flick cannot
                 * re-arm momentum from a brief slow touch. */
                state->scroll_vel_x = 0;
                state->scroll_vel_y = 0;
                /* The locked axis' remainder still holds drift accumulated
                 * before classification; clear it so none of it leaks into
                 * the first scroll frame. */
                if (state->two_finger_parallel_acc_y
                    >= (state->two_finger_parallel_acc_x * APPLE_TRACKPAD_AXIS_LOCK_RATIO)) {
                    state->scroll_axis_lock = APPLE_TRACKPAD_AXIS_VERTICAL;
                    state->scroll_rem_x = 0;
                } else if (state->two_finger_parallel_acc_x
                    >= (state->two_finger_parallel_acc_y * APPLE_TRACKPAD_AXIS_LOCK_RATIO)) {
                    state->scroll_axis_lock = APPLE_TRACKPAD_AXIS_HORIZONTAL;
                    state->scroll_rem_y = 0;
                } else {
                    state->scroll_axis_lock = APPLE_TRACKPAD_AXIS_FREE;
                }
            }
            /* A classified gesture is never a tap, even if the average
             * position barely moved (a symmetric pinch). */
            state->episode_moved = true;
        }
    }

    /* The locked-out axis contributes nothing: not to the emitted counts
     * and not to the velocity the momentum tail inherits. */
    if (state->scroll_axis_lock == APPLE_TRACKPAD_AXIS_VERTICAL) {
        avg_dx = 0;
    } else if (state->scroll_axis_lock == APPLE_TRACKPAD_AXIS_HORIZONTAL) {
        avg_dy = 0;
    }

    if (state->two_finger_mode != APPLE_TRACKPAD_TWO_FINGER_PINCH) {
        /* Velocity gain after classification (the classifier thresholds
         * are tuned in raw units): slow scrolling is finer than linear,
         * fast scrolling brisker, unity at typical deliberate speed. The
         * momentum tail inherits the gained velocity via the average. */
        const int32_t gain_q6 = apple_trackpad_speed_gain_q6(
            apple_trackpad_abs_i32(avg_dx) + apple_trackpad_abs_i32(avg_dy)
        );

        avg_dx = (avg_dx * gain_q6) / 64;
        avg_dy = (avg_dy * gain_q6) / 64;
        state->scroll_rem_x += avg_dx;
        state->scroll_rem_y += avg_dy;
    }

    if (state->two_finger_mode == APPLE_TRACKPAD_TWO_FINGER_SCROLL) {
        /*
         * Traditional wheel semantics: fingers moving up (negative dy)
         * scroll up (positive wheel), fingers moving right pan right. A
         * host with "natural" scrolling enabled inverts these itself,
         * landing on the native finger-follows-content feel.
         */
        *wheel = -(state->scroll_rem_y / APPLE_TRACKPAD_SCROLL_DIV);
        *pan = state->scroll_rem_x / APPLE_TRACKPAD_SCROLL_DIV;
        state->scroll_rem_y += *wheel * APPLE_TRACKPAD_SCROLL_DIV;
        state->scroll_rem_x -= *pan * APPLE_TRACKPAD_SCROLL_DIV;
        /* Half-old half-new running average: smooth but quick enough to
         * be meaningful within even a two-frame flick. */
        state->scroll_vel_x = (state->scroll_vel_x + avg_dx) / 2;
        state->scroll_vel_y = (state->scroll_vel_y + avg_dy) / 2;
        state->scroll_vel_ms = now_ms;
    } else if (state->two_finger_mode == APPLE_TRACKPAD_TWO_FINGER_PINCH) {
        /*
         * Pinch approximates zoom as Command +/- steps -- macOS has no
         * generic smooth-zoom input from a non-Apple device. One chord
         * pair per frame keeps the output queue bounded.
         */
        state->pinch_rem += dspread;
        if (state->pinch_rem >= APPLE_TRACKPAD_PINCH_STEP) {
            apple_trackpad_emit_chord(out, APPLE_TRACKPAD_MOD_LEFT_GUI, APPLE_TRACKPAD_KEY_EQUAL);
            state->pinch_rem -= APPLE_TRACKPAD_PINCH_STEP;
        } else if (state->pinch_rem <= -APPLE_TRACKPAD_PINCH_STEP) {
            apple_trackpad_emit_chord(out, APPLE_TRACKPAD_MOD_LEFT_GUI, APPLE_TRACKPAD_KEY_MINUS);
            state->pinch_rem += APPLE_TRACKPAD_PINCH_STEP;
        }
    }
}

/*
 * Three-finger swipes fire the macOS shortcut equivalents once per
 * three-finger segment: horizontally the Spaces switch (fingers moving
 * left go to the space on the right, matching the native content-follows-
 * fingers direction), vertically Mission Control (up) and App Expose
 * (down) via their default Ctrl+arrow bindings.
 */
static void apple_trackpad_three_finger_swipe(
    apple_trackpad_state_t * state,
    uint8_t matched,
    int32_t sum_dx,
    int32_t sum_dy,
    apple_trackpad_out_t * out
) {
    int32_t travel_x = 0;
    int32_t travel_y = 0;

    state->swipe_acc_x += sum_dx / (int32_t)matched;
    state->swipe_acc_y += sum_dy / (int32_t)matched;
    if (state->swipe_fired) {
        return;
    }

    travel_x = apple_trackpad_abs_i32(state->swipe_acc_x);
    travel_y = apple_trackpad_abs_i32(state->swipe_acc_y);
    if ((travel_x >= APPLE_TRACKPAD_SWIPE_THRESHOLD) && (travel_x >= travel_y)) {
        apple_trackpad_emit_chord(
            out,
            APPLE_TRACKPAD_MOD_LEFT_CTRL,
            (state->swipe_acc_x < 0) ? APPLE_TRACKPAD_KEY_RIGHT_ARROW
                                     : APPLE_TRACKPAD_KEY_LEFT_ARROW
        );
        state->swipe_fired = true;
    } else if ((travel_y >= APPLE_TRACKPAD_SWIPE_THRESHOLD) && (travel_y > travel_x)) {
        apple_trackpad_emit_chord(
            out,
            APPLE_TRACKPAD_MOD_LEFT_CTRL,
            (state->swipe_acc_y < 0) ? APPLE_TRACKPAD_KEY_UP_ARROW : APPLE_TRACKPAD_KEY_DOWN_ARROW
        );
        state->swipe_fired = true;
    }
}

/*
 * Episode end, all fingers up: a fast scroll liftoff becomes a coasting
 * momentum tail, and a short, still, click-free touch becomes a tap (when
 * tap-to-click is enabled). The tail launches at KICK times the tracked
 * average velocity: the average lags an accelerating flick, so the boost
 * lands near the true lift velocity, matching the native driver's strong
 * initial kick. The tap press is emitted here; its release is timed
 * (apple_trackpad_tick) because the trackpad stops sending frames once
 * all fingers are up.
 */
static void apple_trackpad_episode_end(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out
) {
    state->touch_active = false;

    /* Recency stands in for "was scrolling": the scroll mode itself is
     * reset on the way down (2 -> 1 -> 0 finger transitions). */
    if (((uint32_t)(now_ms - state->scroll_vel_ms) <= APPLE_TRACKPAD_MOMENTUM_RECENT_MS)
        && ((apple_trackpad_abs_i32(state->scroll_vel_x) >= APPLE_TRACKPAD_MOMENTUM_MIN_VELOCITY)
            || (apple_trackpad_abs_i32(state->scroll_vel_y)
                >= APPLE_TRACKPAD_MOMENTUM_MIN_VELOCITY))) {
        state->momentum_active = true;
        state->momentum_vel_x = (state->scroll_vel_x * 256 * APPLE_TRACKPAD_MOMENTUM_KICK_NUM)
            / APPLE_TRACKPAD_MOMENTUM_KICK_DEN;
        state->momentum_vel_y = (state->scroll_vel_y * 256 * APPLE_TRACKPAD_MOMENTUM_KICK_NUM)
            / APPLE_TRACKPAD_MOMENTUM_KICK_DEN;
        state->momentum_next_step_ms = now_ms + APPLE_TRACKPAD_MOMENTUM_STEP_MS;
        state->scroll_vel_x = 0;
        state->scroll_vel_y = 0;
    }

    if (state->tap_to_click
        && !state->episode_moved
        && !state->episode_clicked
        && ((uint32_t)(now_ms - state->touch_started_ms) <= APPLE_TRACKPAD_TAP_MAX_MS)) {
        const uint8_t pulse = apple_trackpad_buttons_for_fingers(state->episode_max_fingers);

        apple_trackpad_emit_mouse(state, out, pulse, 0, 0, 0, 0);
        state->pending_release_buttons = pulse;
        state->tap_release_deadline_ms = now_ms + APPLE_TRACKPAD_TAP_PULSE_MS;
    }
}

bool apple_trackpad_process_report(
    apple_trackpad_state_t * state,
    const uint8_t * report,
    uint16_t report_len,
    uint32_t now_ms,
    apple_trackpad_out_t * out
) {
    apple_trackpad_record_t record[APPLE_TRACKPAD_MAX_TOUCH];
    uint8_t record_count = 0U;
    uint8_t finger_count = 0U;
    uint8_t matched = 0U;
    uint8_t buttons = 0U;
    int32_t sum_dx = 0;
    int32_t sum_dy = 0;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;
    int32_t pan = 0;
    uint8_t i = 0U;

    if ((state == NULL) || !state->initialized || (report == NULL) || (out == NULL)) {
        return false;
    }
    if (!apple_trackpad_parse_frame(
            state,
            report,
            report_len,
            record,
            &record_count,
            &finger_count
        )) {
        return false;
    }

    /* A tap pulse still in flight is released before this frame's output so
     * a quick follow-up touch cannot leave the synthesized button stuck. */
    apple_trackpad_flush_tap_release(state, now_ms, out, finger_count > 0U);

    /* Touching the pad catches a coasting scroll, as on a native trackpad. */
    if (finger_count > 0U) {
        state->momentum_active = false;
    }

    buttons = apple_trackpad_map_click(state, report, finger_count);
    matched =
        apple_trackpad_motion_delta(state, record, record_count, finger_count, &sum_dx, &sum_dy);
    apple_trackpad_episode_update(state, finger_count, buttons, matched, sum_dx, sum_dy, now_ms);

    if ((finger_count == 1U) && (matched == 1U)) {
        const int32_t gain_q6 = apple_trackpad_speed_gain_q6(
            apple_trackpad_abs_i32(sum_dx) + apple_trackpad_abs_i32(sum_dy)
        );

        state->move_rem_x += (sum_dx * gain_q6) / 64;
        state->move_rem_y += (sum_dy * gain_q6) / 64;
        dx = state->move_rem_x / APPLE_TRACKPAD_POINTER_DIV;
        dy = state->move_rem_y / APPLE_TRACKPAD_POINTER_DIV;
        state->move_rem_x -= dx * APPLE_TRACKPAD_POINTER_DIV;
        state->move_rem_y -= dy * APPLE_TRACKPAD_POINTER_DIV;
    } else if (finger_count == 2U) {
        apple_trackpad_two_finger(
            state,
            record,
            record_count,
            matched,
            sum_dx,
            sum_dy,
            now_ms,
            out,
            &wheel,
            &pan
        );
    } else if ((finger_count == 3U) && (matched > 0U)) {
        apple_trackpad_three_finger_swipe(state, matched, sum_dx, sum_dy, out);
    }

    apple_trackpad_emit_mouse(state, out, buttons, dx, dy, wheel, pan);

    if ((finger_count == 0U) && state->touch_active) {
        apple_trackpad_episode_end(state, now_ms, out);
    }

    /* Replace the touch table with this frame's view. */
    (void)memset(state->touch, 0, sizeof(state->touch));
    for (i = 0U; i < record_count; i++) {
        apple_trackpad_touch_t * slot = &state->touch[record[i].id];

        slot->valid = true;
        slot->down = record[i].down;
        slot->x = record[i].x;
        slot->y = record[i].y;
    }
    state->finger_count = finger_count;

    return true;
}

/*
 * Advance a coasting scroll: decay the velocity once per elapsed step and
 * emit the accumulated wheel/pan counts through the same sub-count
 * remainders live scrolling uses, so the tail continues seamlessly.
 */
static void apple_trackpad_flush_momentum(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out
) {
    uint8_t steps = 0U;
    int32_t wheel = 0;
    int32_t pan = 0;

    if (!state->momentum_active) {
        return;
    }

    while (((int32_t)(now_ms - state->momentum_next_step_ms) >= 0)
        && (steps < APPLE_TRACKPAD_MOMENTUM_MAX_STEPS)) {
        state->momentum_vel_x = (state->momentum_vel_x * APPLE_TRACKPAD_MOMENTUM_DECAY) / 256;
        state->momentum_vel_y = (state->momentum_vel_y * APPLE_TRACKPAD_MOMENTUM_DECAY) / 256;
        state->scroll_rem_x += state->momentum_vel_x / 256;
        state->scroll_rem_y += state->momentum_vel_y / 256;
        state->momentum_next_step_ms += APPLE_TRACKPAD_MOMENTUM_STEP_MS;
        steps = (uint8_t)(steps + 1U);
    }
    if (steps == 0U) {
        return;
    }
    if (steps == APPLE_TRACKPAD_MOMENTUM_MAX_STEPS) {
        /* Stalled for longer than the catch-up budget (e.g. host suspend):
         * drop the backlog instead of replaying it as a burst. */
        state->momentum_next_step_ms = now_ms + APPLE_TRACKPAD_MOMENTUM_STEP_MS;
    }

    if ((apple_trackpad_abs_i32(state->momentum_vel_x / 256) < APPLE_TRACKPAD_MOMENTUM_FLOOR)
        && (apple_trackpad_abs_i32(state->momentum_vel_y / 256) < APPLE_TRACKPAD_MOMENTUM_FLOOR)) {
        state->momentum_active = false;
    }

    wheel = -(state->scroll_rem_y / APPLE_TRACKPAD_SCROLL_DIV);
    pan = state->scroll_rem_x / APPLE_TRACKPAD_SCROLL_DIV;
    state->scroll_rem_y += wheel * APPLE_TRACKPAD_SCROLL_DIV;
    state->scroll_rem_x -= pan * APPLE_TRACKPAD_SCROLL_DIV;
    if ((wheel != 0) || (pan != 0)) {
        apple_trackpad_emit_mouse(state, out, state->buttons, 0, 0, wheel, pan);
    }
}

void apple_trackpad_tick(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out
) {
    if ((state == NULL) || !state->initialized || (out == NULL)) {
        return;
    }
    apple_trackpad_flush_tap_release(state, now_ms, out, false);
    apple_trackpad_flush_momentum(state, now_ms, out);
}
