#ifndef HIDRELAY_APPLE_TRACKPAD_H
#define HIDRELAY_APPLE_TRACKPAD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Relay-side gesture engine for recognized Apple Magic Trackpads.
 *
 * In its default HID mode a Magic Trackpad reports as a plain mouse: no
 * scroll, no touch data. All multitouch lives in Apple's vendor protocol,
 * which stays dormant until the host writes a model-specific "enable
 * multitouch" feature report; natively only Apple's drivers do that, keyed
 * off Apple identity the relay does not present. So the relay plays the
 * driver itself: it sends the enable report over Bluetooth, consumes the
 * resulting vendor touch frames, runs a small gesture engine, and emits
 * standard HID reports declared by collections appended to the trackpad's
 * own descriptor (apple_trackpad_augment_descriptor). Non-Apple devices are
 * never touched, and if the enable write fails the trackpad keeps working
 * as the plain mouse its native descriptor declares.
 *
 * The vendor frame format follows the public reverse-engineered protocol
 * used by Linux hid-magicmouse: a per-model report ID, a 4-byte prefix
 * whose second byte carries the physical button, then 9 bytes per touch
 * (13-bit signed x/y, touch id, touch state).
 */

/* Synthesized mouse report: id, buttons, x16, y16, wheel, AC pan. */
#define APPLE_TRACKPAD_MOUSE_REPORT_ID 0xB1U
#define APPLE_TRACKPAD_MOUSE_REPORT_LEN 8U

/* Synthesized keyboard-chord report (gesture shortcuts): id, modifiers, key. */
#define APPLE_TRACKPAD_CHORD_REPORT_ID 0xB2U
#define APPLE_TRACKPAD_CHORD_REPORT_LEN 3U

#define APPLE_TRACKPAD_MAX_OUT_REPORTS 4U
#define APPLE_TRACKPAD_OUT_REPORT_MAX_LEN 8U
#define APPLE_TRACKPAD_MAX_TOUCH 16U

/* True when (vendor_id, product_id) is a recognized Apple trackpad. The
 * vendor_id is the USB-form Apple vendor ID (0x05AC). */
bool apple_trackpad_is_supported(
    uint16_t vendor_id,
    uint16_t product_id
);

/*
 * Multitouch-enable feature report for this model (sent host->device over
 * the HID control channel as SET_REPORT/Feature). False when product_id is
 * not a recognized trackpad. out_payload excludes the report ID.
 */
bool apple_trackpad_mt_enable_report(
    uint16_t product_id,
    uint8_t * out_report_id,
    uint8_t out_payload[4],
    uint8_t * out_payload_len
);

/*
 * Build base_descriptor + the synthesized-report collections into out_buf.
 * Returns the total length, or 0 if product_id is unsupported or out_buf is
 * too small.
 */
uint16_t apple_trackpad_augment_descriptor(
    uint16_t product_id,
    const uint8_t * base_descriptor,
    uint16_t base_len,
    uint8_t * out_buf,
    uint16_t out_cap
);

/* Reports synthesized for one input frame, in emit order. */
typedef struct {
    uint8_t count;
    uint8_t len[APPLE_TRACKPAD_MAX_OUT_REPORTS];
    uint8_t bytes[APPLE_TRACKPAD_MAX_OUT_REPORTS][APPLE_TRACKPAD_OUT_REPORT_MAX_LEN];
} apple_trackpad_out_t;

/* One tracked touch slot, keyed by the protocol's 4-bit touch id. */
typedef struct {
    bool valid; /* slot seen in the last frame */
    bool down;
    int16_t x;
    int16_t y;
} apple_trackpad_touch_t;

/* Per-trackpad engine state. Treat as opaque; init with apple_trackpad_state_init. */
typedef struct {
    bool initialized;
    uint16_t product_id;
    uint8_t family; /* decode family (Magic Trackpad 1 vs 2/USB-C) */
    apple_trackpad_touch_t touch[APPLE_TRACKPAD_MAX_TOUCH];
    uint8_t finger_count; /* down touches in the last frame */
    uint8_t buttons; /* last emitted button bits */
    /* Sub-count remainders so slow motion is not truncated away. */
    int32_t move_rem_x;
    int32_t move_rem_y;
    int32_t scroll_rem_x;
    int32_t scroll_rem_y;
    /* Tap/click synthesis. A touch episode runs from first finger down to
     * last finger up; short, still, click-free episodes become taps. */
    bool touch_active;
    uint32_t touch_started_ms;
    uint8_t episode_max_fingers;
    bool episode_moved;
    bool episode_clicked;
    int32_t episode_travel;
    uint8_t click_buttons; /* mapping latched while the physical button is down */
    uint8_t pending_release_buttons; /* tap pulse awaiting its timed release */
    uint32_t tap_release_deadline_ms;
    /* Two-finger mode (scroll vs pinch), classified once per episode from
     * whether parallel motion or spread change dominates. */
    uint8_t two_finger_mode;
    int32_t two_finger_parallel_acc;
    int32_t two_finger_spread_acc;
    bool two_finger_spread_valid;
    int32_t two_finger_prev_spread;
    int32_t pinch_rem; /* spread change accumulated toward the next zoom step */
    /* Three-finger swipe accumulation; one chord per three-finger segment. */
    int32_t swipe_acc_x;
    int32_t swipe_acc_y;
    bool swipe_fired;
} apple_trackpad_state_t;

void apple_trackpad_state_init(
    apple_trackpad_state_t * state,
    uint16_t product_id
);

/*
 * Consume one incoming input report (Bluetooth transaction header already
 * stripped). Returns true when the report was a vendor multitouch frame and
 * was consumed; out then holds 0..N synthesized reports to forward in order.
 * Returns false for any other report (e.g. plain-mouse reports before the
 * multitouch enable lands) -- the caller forwards the original unchanged.
 */
bool apple_trackpad_process_report(
    apple_trackpad_state_t * state,
    const uint8_t * report,
    uint16_t report_len,
    uint32_t now_ms,
    apple_trackpad_out_t * out
);

/*
 * Time-driven output (deferred tap releases and similar). Call periodically;
 * appends to out like process_report.
 */
void apple_trackpad_tick(
    apple_trackpad_state_t * state,
    uint32_t now_ms,
    apple_trackpad_out_t * out
);

#endif
