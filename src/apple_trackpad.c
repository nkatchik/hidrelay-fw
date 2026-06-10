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
 * yields ~24 counts/mm (a ~600 dpi mouse, before host acceleration); the
 * scroll divider yields one wheel detent per ~1.4 mm of finger travel.
 */
enum {
    APPLE_TRACKPAD_POINTER_DIV = 2,
    APPLE_TRACKPAD_SCROLL_DIV = 64
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
    uint16_t touch_bytes = 0U;
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

    /* A tap pulse still in flight is released before this frame's output so
     * a quick follow-up touch cannot leave the synthesized button stuck. */
    apple_trackpad_flush_tap_release(state, now_ms, out, finger_count > 0U);

    /*
     * Physical click, mapped by resting finger count (one = left, two =
     * right, three = middle) and latched for the whole press so lifting a
     * finger mid-press cannot morph the button.
     */
    if ((report[APPLE_TRACKPAD_BUTTON_BYTE] & 0x01U) != 0U) {
        if (state->click_buttons == 0U) {
            state->click_buttons = apple_trackpad_buttons_for_fingers(finger_count);
        }
        buttons = state->click_buttons;
    } else {
        state->click_buttons = 0U;
    }

    /*
     * Motion is only accumulated while the finger count is stable; the frame
     * where a finger lands or lifts otherwise injects a large bogus delta
     * (e.g. the pointer jumping when a second finger starts a scroll).
     */
    if (finger_count == state->finger_count) {
        for (i = 0U; i < record_count; i++) {
            const apple_trackpad_touch_t * prev = &state->touch[record[i].id];

            if (!record[i].down || !prev->valid || !prev->down) {
                continue;
            }
            sum_dx += (int32_t)record[i].x - (int32_t)prev->x;
            sum_dy += (int32_t)record[i].y - (int32_t)prev->y;
            matched = (uint8_t)(matched + 1U);
        }
    } else {
        state->move_rem_x = 0;
        state->move_rem_y = 0;
        state->scroll_rem_x = 0;
        state->scroll_rem_y = 0;
        state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_UNDECIDED;
        state->two_finger_parallel_acc = 0;
        state->two_finger_spread_acc = 0;
        state->two_finger_spread_valid = false;
        state->pinch_rem = 0;
        state->swipe_acc_x = 0;
        state->swipe_acc_y = 0;
        state->swipe_fired = false;
    }

    /* Touch-episode bookkeeping for tap detection. */
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

    if ((finger_count == 1U) && (matched == 1U)) {
        state->move_rem_x += sum_dx;
        state->move_rem_y += sum_dy;
        dx = state->move_rem_x / APPLE_TRACKPAD_POINTER_DIV;
        dy = state->move_rem_y / APPLE_TRACKPAD_POINTER_DIV;
        state->move_rem_x -= dx * APPLE_TRACKPAD_POINTER_DIV;
        state->move_rem_y -= dy * APPLE_TRACKPAD_POINTER_DIV;
    } else if (finger_count == 2U) {
        /* Runs on the touch-down frame too (matched == 0) so the spread
         * baseline exists before any motion is classified. */
        const int32_t avg_dx = (matched > 0U) ? (sum_dx / (int32_t)matched) : 0;
        const int32_t avg_dy = (matched > 0U) ? (sum_dy / (int32_t)matched) : 0;
        const apple_trackpad_record_t * first = NULL;
        const apple_trackpad_record_t * second = NULL;
        int32_t dspread = 0;

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
            state->two_finger_parallel_acc +=
                apple_trackpad_abs_i32(avg_dx) + apple_trackpad_abs_i32(avg_dy);
            state->two_finger_spread_acc += apple_trackpad_abs_i32(dspread);
            if ((state->two_finger_parallel_acc >= APPLE_TRACKPAD_TWO_FINGER_CLASSIFY_THRESHOLD)
                || (state->two_finger_spread_acc >= APPLE_TRACKPAD_TWO_FINGER_CLASSIFY_THRESHOLD)) {
                if (state->two_finger_spread_acc > state->two_finger_parallel_acc) {
                    state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_PINCH;
                    state->scroll_rem_x = 0;
                    state->scroll_rem_y = 0;
                    state->pinch_rem = 0;
                } else {
                    state->two_finger_mode = APPLE_TRACKPAD_TWO_FINGER_SCROLL;
                }
                /* A classified gesture is never a tap, even if the average
                 * position barely moved (a symmetric pinch). */
                state->episode_moved = true;
            }
        }

        if (state->two_finger_mode != APPLE_TRACKPAD_TWO_FINGER_PINCH) {
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
            wheel = -(state->scroll_rem_y / APPLE_TRACKPAD_SCROLL_DIV);
            pan = state->scroll_rem_x / APPLE_TRACKPAD_SCROLL_DIV;
            state->scroll_rem_y += wheel * APPLE_TRACKPAD_SCROLL_DIV;
            state->scroll_rem_x -= pan * APPLE_TRACKPAD_SCROLL_DIV;
        } else if (state->two_finger_mode == APPLE_TRACKPAD_TWO_FINGER_PINCH) {
            /*
             * Pinch approximates zoom as Command +/- steps -- macOS has no
             * generic smooth-zoom input from a non-Apple device. One chord
             * pair per frame keeps the output queue bounded.
             */
            state->pinch_rem += dspread;
            if (state->pinch_rem >= APPLE_TRACKPAD_PINCH_STEP) {
                apple_trackpad_emit_chord(
                    out,
                    APPLE_TRACKPAD_MOD_LEFT_GUI,
                    APPLE_TRACKPAD_KEY_EQUAL
                );
                state->pinch_rem -= APPLE_TRACKPAD_PINCH_STEP;
            } else if (state->pinch_rem <= -APPLE_TRACKPAD_PINCH_STEP) {
                apple_trackpad_emit_chord(
                    out,
                    APPLE_TRACKPAD_MOD_LEFT_GUI,
                    APPLE_TRACKPAD_KEY_MINUS
                );
                state->pinch_rem += APPLE_TRACKPAD_PINCH_STEP;
            }
        }
    } else if ((finger_count == 3U) && (matched > 0U)) {
        /*
         * Three-finger swipes fire the macOS shortcut equivalents once per
         * three-finger segment: horizontally the Spaces switch (fingers
         * moving left go to the space on the right, matching the native
         * content-follows-fingers direction), vertically Mission Control
         * (up) and App Expose (down) via their default Ctrl+arrow bindings.
         */
        state->swipe_acc_x += sum_dx / (int32_t)matched;
        state->swipe_acc_y += sum_dy / (int32_t)matched;
        if (!state->swipe_fired) {
            const int32_t travel_x = apple_trackpad_abs_i32(state->swipe_acc_x);
            const int32_t travel_y = apple_trackpad_abs_i32(state->swipe_acc_y);

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
                    (state->swipe_acc_y < 0) ? APPLE_TRACKPAD_KEY_UP_ARROW
                                             : APPLE_TRACKPAD_KEY_DOWN_ARROW
                );
                state->swipe_fired = true;
            }
        }
    }

    apple_trackpad_emit_mouse(state, out, buttons, dx, dy, wheel, pan);

    /*
     * Episode end: a short, still, click-free touch becomes a tap. The press
     * is emitted now; the release is timed (apple_trackpad_tick) because the
     * trackpad stops sending frames once all fingers are up.
     */
    if ((finger_count == 0U) && state->touch_active) {
        state->touch_active = false;
        if (!state->episode_moved
            && !state->episode_clicked
            && ((uint32_t)(now_ms - state->touch_started_ms) <= APPLE_TRACKPAD_TAP_MAX_MS)) {
            const uint8_t pulse = apple_trackpad_buttons_for_fingers(state->episode_max_fingers);

            apple_trackpad_emit_mouse(state, out, pulse, 0, 0, 0, 0);
            state->pending_release_buttons = pulse;
            state->tap_release_deadline_ms = now_ms + APPLE_TRACKPAD_TAP_PULSE_MS;
        }
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

void apple_trackpad_tick(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out
) {
    if ((state == NULL) || !state->initialized || (out == NULL)) {
        return;
    }
    apple_trackpad_flush_tap_release(state, now_ms, out, false);
}
