#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "apple_trackpad.h"
#include "hid_device_map.h"
#include "hid_transport_runtime.h"

typedef bool (*app_replay_test_fn_t)(void);

typedef struct {
    const char * name;
    app_replay_test_fn_t fn;
} app_replay_test_case_t;

static pair_device_id_t app_replay_device_id(uint8_t suffix) {
    pair_device_id_t id = {.bytes = {0x00U, 0x1AU, 0x7DU, 0xDAU, 0x71U, suffix}};
    return id;
}

static bool app_replay_device_id_equal(
    const pair_device_id_t * lhs,
    const pair_device_id_t * rhs
) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static void app_replay_tick(
    app_t * app,
    uint32_t now_ms,
    bool button_pressed,
    const hid_transport_event_t * event,
    app_output_t * out
) {
    app_input_t input = {0};

    if ((app == NULL) || (out == NULL)) {
        return;
    }

    input.button_pressed = button_pressed;
    input.now_ms = now_ms;
    input.transport_event.type = HID_TRANSPORT_EVENT_NONE;

    if (event != NULL) {
        input.transport_event = *event;
    }

    (void)memset(out, 0, sizeof(*out));
    app_tick(app, &input, out);
}

static bool app_replay_expect_true(
    bool value,
    const char * message
) {
    if (value) {
        return true;
    }

    (void)fprintf(stderr, "FAIL: %s\n", message);
    return false;
}

static bool app_replay_expect_u32_eq(
    uint32_t actual,
    uint32_t expected,
    const char * message
) {
    if (actual == expected) {
        return true;
    }

    (void)fprintf(
        stderr,
        "FAIL: %s (actual=%lu expected=%lu)\n",
        message,
        (unsigned long)actual,
        (unsigned long)expected
    );
    return false;
}

static bool app_replay_test_led_startup_cue_short(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "startup cue should light LED on boot")) {
        return false;
    }

    app_replay_tick(&app, 199U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "startup cue should stay on briefly")) {
        return false;
    }

    app_replay_tick(&app, 200U, false, NULL, &out);
    return app_replay_expect_true(!out.led_on, "startup cue should end quickly");
}

static bool app_replay_test_led_signal_preempt_disconnect_cue(void) {
    led_ui_t led = {0};
    uint32_t dark_until_ms = 0U;

    led_ui_init(&led);
    led_ui_trigger_long_blink(&led, 1U, 100U);
    if (!app_replay_expect_true(led_ui_tick(&led, 100U), "long blink should start immediately")) {
        return false;
    }

    led_ui_trigger_disconnect_cue(&led, 200U);
    dark_until_ms = led.signal_dark_until_ms;
    if (!app_replay_expect_true(
            dark_until_ms > 200U,
            "preempt should schedule a dark handoff window"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !led_ui_tick(&led, 200U),
            "preempt should force LED dark immediately"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            led_ui_tick(&led, dark_until_ms),
            "disconnect cue should start after dark handoff"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            led.disconnect_cue_until_ms - dark_until_ms,
            1000U,
            "disconnect cue should be programmed for 1 second after dark handoff"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            led_ui_tick(&led, dark_until_ms + 500U),
            "disconnect cue should remain active for 1 second"
        )) {
        return false;
    }

    return app_replay_expect_true(
        !led_ui_tick(&led, dark_until_ms + 1000U),
        "disconnect cue should finish at 1 second"
    );
}

static bool app_replay_test_pairing_entry_clears_active_signal(void) {
    led_ui_t led = {0};

    led_ui_init(&led);
    led_ui_trigger_long_blink(&led, 1U, 100U);
    if (!app_replay_expect_true(led_ui_tick(&led, 100U), "long blink should start")) {
        return false;
    }

    led_ui_set_state(&led, LED_UI_STATE_PAIRING, 150U);
    if (!app_replay_expect_true(!led.cue_active, "pairing should cancel active blink cue")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            led.signal_dark_until_ms,
            0U,
            "pairing should clear preempt dark handoff"
        )) {
        return false;
    }

    if (!app_replay_expect_true(led_ui_tick(&led, 150U), "pairing signal should start on")) {
        return false;
    }

    if (!app_replay_expect_true(led_ui_tick(&led, 249U), "pairing LED should follow BLE cadence")) {
        return false;
    }

    return app_replay_expect_true(
        !led_ui_tick(&led, 250U),
        "pairing LED should toggle at BLE cadence after signal takeover"
    );
}

static bool app_replay_test_pair_any_from_long_press(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    if (!app_replay_expect_true(!out.pairing_active, "pairing should be inactive on press start")) {
        return false;
    }

    app_replay_tick(&app, 2000U, true, NULL, &out);
    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should activate once long-press threshold is reached while held"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            "initial long press should enter BLE pairing"
        )) {
        return false;
    }

    app_replay_tick(&app, 3100U, false, NULL, &out);
    return app_replay_expect_true(
        out.pairing_active,
        "pair-any should stay active after long-press release"
    );
}

static bool app_replay_test_pair_classic_after_extended_hold(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            "pairing should start in BLE mode at one second"
        )) {
        return false;
    }

    app_replay_tick(&app, 5999U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            "pairing should remain BLE before five-second hold threshold"
        )) {
        return false;
    }

    app_replay_tick(&app, 6000U, true, NULL, &out);
    if (!app_replay_expect_true(out.pairing_active, "pairing should stay active after switch")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
            "five-second hold should switch to Classic pairing"
        )) {
        return false;
    }

    app_replay_tick(&app, 6500U, false, NULL, &out);
    return app_replay_expect_u32_eq(
        out.pairing_link_type,
        HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
        "Classic pairing should stay active after release"
    );
}

static bool app_replay_test_pairing_led_cadence_follows_link_type(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            "pairing should start in BLE mode"
        )) {
        return false;
    }

    app_replay_tick(&app, 2099U, true, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "BLE pairing blink should stay on before 100ms")) {
        return false;
    }

    app_replay_tick(&app, 2100U, true, NULL, &out);
    if (!app_replay_expect_true(!out.led_on, "BLE pairing blink should toggle at 100ms")) {
        return false;
    }

    app_replay_tick(&app, 6000U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            out.pairing_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
            "extended hold should switch to Classic mode"
        )) {
        return false;
    }

    app_replay_tick(&app, 6299U, true, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "Classic pairing blink should stay on before 300ms")) {
        return false;
    }

    app_replay_tick(&app, 6300U, true, NULL, &out);
    return app_replay_expect_true(!out.led_on, "Classic pairing blink should toggle at 300ms");
}

static bool app_replay_test_duplicate_pair_command_keeps_attempt_led(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x34U);

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    app_replay_tick(&app, 2100U, false, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 2200U, false, &event, &out);
    if (!app_replay_expect_true(out.led_on, "pairing attempt should hold LED on")) {
        return false;
    }

    app_replay_tick(&app, 5000U, false, NULL, &out);
    app_replay_tick(&app, 6000U, true, NULL, &out);
    app_replay_tick(&app, 7000U, true, NULL, &out);
    app_replay_tick(&app, 7050U, true, NULL, &out);
    return app_replay_expect_true(
        out.led_on,
        "ignored duplicate pair command should not clear active attempt LED"
    );
}

static bool app_replay_test_pair_any_timeout_after_60s(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    app_replay_tick(&app, 3100U, false, NULL, &out);

    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should be active before timeout countdown"
        )) {
        return false;
    }

    app_replay_tick(&app, 61999U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should stay active until full 60s elapsed"
        )) {
        return false;
    }

    app_replay_tick(&app, 62000U, false, NULL, &out);
    if (!app_replay_expect_true(!out.pairing_active, "pair-any should stop at 60s timeout")) {
        return false;
    }

    return app_replay_expect_u32_eq(
        out.pairing_link_type,
        HID_TRANSPORT_BT_LINK_TYPE_UNKNOWN,
        "pairing link type should clear after timeout"
    );
}

static bool app_replay_test_pair_any_cancelled_by_single_click(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    app_replay_tick(&app, 3100U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should be active before single-click cancel"
        )) {
        return false;
    }

    app_replay_tick(&app, 6000U, true, NULL, &out);
    app_replay_tick(&app, 6200U, false, NULL, &out);
    return app_replay_expect_true(
        !out.pairing_active,
        "single click should cancel pairing when active"
    );
}

static bool app_replay_test_pairing_cancel_while_connected_has_no_success_cue(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x35U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x35U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 1000U, false, &event, &out);
    if (!app_replay_expect_true(out.active_device_count == 1U, "device should be connected")) {
        return false;
    }

    app_replay_tick(&app, 5000U, true, NULL, &out);
    app_replay_tick(&app, 6000U, true, NULL, &out);
    app_replay_tick(&app, 6100U, false, NULL, &out);
    if (!app_replay_expect_true(out.pairing_active, "pairing should be active before cancel")) {
        return false;
    }

    app_replay_tick(&app, 6200U, true, NULL, &out);
    app_replay_tick(&app, 6300U, false, NULL, &out);
    if (!app_replay_expect_true(!out.pairing_active, "single click should cancel pairing")) {
        return false;
    }

    if (!app_replay_expect_true(
            !out.led_on,
            "pairing cancel over connected device should not start success cue"
        )) {
        return false;
    }

    app_replay_tick(&app, 9299U, false, NULL, &out);
    return app_replay_expect_true(
        !out.led_on,
        "pairing cancel should stay quiet instead of holding connected cue"
    );
}

static bool app_replay_test_pairing_attempt_led_lifecycle(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x12U);

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);
    if (!app_replay_expect_true(out.pairing_active, "pair-any should be active during hold")) {
        return false;
    }

    if (!app_replay_expect_true(out.led_on, "pairing LED should turn on when pairing starts")) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 2100U, false, &event, &out);
    if (!app_replay_expect_true(
            out.led_on,
            "pairing attempt should drive steady LED on while in progress"
        )) {
        return false;
    }

    app_replay_tick(&app, 2400U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.led_on,
            "pairing attempt LED should remain steady on until completion"
        )) {
        return false;
    }

    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
    event.status_code = 0x04U;
    app_replay_tick(&app, 3000U, false, &event, &out);
    if (!app_replay_expect_true(
            !out.led_on,
            "connect-failed attempt should insert dark handoff before failure cue"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !out.pairing_active,
            "connect-failed attempt should exit pairing mode"
        )) {
        return false;
    }

    app_replay_tick(&app, 4999U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.led_on,
            "failure cue should keep LED dark for two seconds before blinking"
        )) {
        return false;
    }

    app_replay_tick(&app, 5000U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.led_on,
            "failure cue should start first counted on pulse after dark gap"
        )) {
        return false;
    }

    app_replay_tick(&app, 5999U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "failure cue on pulse should last for one second")) {
        return false;
    }

    app_replay_tick(&app, 6000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.led_on,
            "single failure blink should complete after first one-second pulse"
        )) {
        return false;
    }

    app_replay_tick(&app, 7100U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.pairing_active,
            "pairing should stay inactive after failure cue"
        )) {
        return false;
    }

    app_init(&app, NULL);
    app_replay_tick(&app, 5000U, true, NULL, &out);
    app_replay_tick(&app, 6000U, true, NULL, &out);

    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 6100U, false, &event, &out);
    if (!app_replay_expect_true(
            out.led_on,
            "new pairing mode entry should accept a fresh attempt request"
        )) {
        return false;
    }

    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_SUCCESS;
    app_replay_tick(&app, 6200U, false, &event, &out);
    if (!app_replay_expect_true(
            out.led_on,
            "success result should keep attempt LED on until HID open is processed"
        )) {
        return false;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x88U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 6300U, false, &event, &out);

    if (!app_replay_expect_true(
            !out.pairing_active,
            "pairing mode should end after successful HID open"
        )) {
        return false;
    }

    if (!app_replay_expect_true(out.led_on, "pairing success should start the connected cue")) {
        return false;
    }

    app_replay_tick(&app, 9299U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "pairing success cue should last for 3 seconds")) {
        return false;
    }

    app_replay_tick(&app, 9300U, false, NULL, &out);
    return app_replay_expect_true(!out.led_on, "pairing success cue should end after 3 seconds");
}

static bool app_replay_test_pairing_auth_failure_uses_double_blink(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x13U);

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 2100U, false, &event, &out);

    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED;
    event.status_code = 0x05U;
    app_replay_tick(&app, 3000U, false, &event, &out);
    if (!app_replay_expect_true(!out.led_on, "auth-failure cue should start with dark handoff")) {
        return false;
    }

    if (!app_replay_expect_true(
            !out.pairing_active,
            "auth-failure attempt should exit pairing mode"
        )) {
        return false;
    }

    app_replay_tick(&app, 4999U, false, NULL, &out);
    if (!app_replay_expect_true(!out.led_on, "auth-failure cue should hold dark gap")) {
        return false;
    }

    app_replay_tick(&app, 5000U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "auth-failure cue should run first on pulse")) {
        return false;
    }

    app_replay_tick(&app, 5999U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "auth-failure first pulse should last one second")) {
        return false;
    }

    app_replay_tick(&app, 6000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.led_on,
            "auth-failure cue should turn LED off after first pulse"
        )) {
        return false;
    }

    app_replay_tick(&app, 6999U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.led_on,
            "auth-failure cue should wait one second between pulses"
        )) {
        return false;
    }

    app_replay_tick(&app, 7000U, false, NULL, &out);
    if (!app_replay_expect_true(out.led_on, "auth-failure cue should run a second on pulse")) {
        return false;
    }

    app_replay_tick(&app, 8000U, false, NULL, &out);
    return app_replay_expect_true(
        !out.led_on,
        "auth-failure double-blink should complete after second pulse"
    );
}

static bool app_replay_expect_pairing_error_blink_count(
    uint8_t reconnect_result,
    uint8_t expected_count,
    uint8_t device_suffix
) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    const pair_device_id_t device_id = app_replay_device_id(device_suffix);
    uint8_t blink_index = 0U;

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 2100U, false, &event, &out);

    event.reconnect_result = reconnect_result;
    event.status_code = 0x04U;
    app_replay_tick(&app, 3000U, false, &event, &out);
    if (!app_replay_expect_true(!out.led_on, "diagnostic cue should start dark")) {
        return false;
    }

    if (!app_replay_expect_true(
            !out.pairing_active,
            "diagnostic pairing failure should exit pairing mode"
        )) {
        return false;
    }

    app_replay_tick(&app, 4999U, false, NULL, &out);
    if (!app_replay_expect_true(!out.led_on, "diagnostic cue should hold dark gap")) {
        return false;
    }

    for (blink_index = 0U; blink_index < expected_count; blink_index++) {
        const uint32_t on_start_ms = 5000U + ((uint32_t)blink_index * 2000U);

        app_replay_tick(&app, on_start_ms, false, NULL, &out);
        if (!app_replay_expect_true(out.led_on, "diagnostic cue should run counted on pulse")) {
            return false;
        }

        app_replay_tick(&app, on_start_ms + 999U, false, NULL, &out);
        if (!app_replay_expect_true(out.led_on, "diagnostic cue pulse should last one second")) {
            return false;
        }

        app_replay_tick(&app, on_start_ms + 1000U, false, NULL, &out);
        if (!app_replay_expect_true(!out.led_on, "diagnostic cue should turn off after pulse")) {
            return false;
        }

        if ((uint8_t)(blink_index + 1U) < expected_count) {
            app_replay_tick(&app, on_start_ms + 1999U, false, NULL, &out);
            if (!app_replay_expect_true(
                    !out.led_on,
                    "diagnostic cue should stay off between pulses"
                )) {
                return false;
            }
        }
    }

    return app_replay_expect_true(!out.led_on, "diagnostic cue should finish after count");
}

static bool app_replay_test_pairing_phase_failures_use_diagnostic_blinks(void) {
    return app_replay_expect_pairing_error_blink_count(
               HID_TRANSPORT_RECONNECT_RESULT_PAIRING_CLASSIC_CONNECT_FAILED,
               5U,
               0x31U
           )
        && app_replay_expect_pairing_error_blink_count(
            HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_CONNECT_FAILED,
            6U,
            0x32U
        )
        && app_replay_expect_pairing_error_blink_count(
            HID_TRANSPORT_RECONNECT_RESULT_PAIRING_LE_HIDS_FAILED,
            7U,
            0x33U
        );
}

static bool app_replay_test_pairing_close_without_active_device_keeps_attempt_led(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x14U);

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 2000U, true, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_REQUESTED;
    event.status_code = 0U;
    app_replay_tick(&app, 2100U, false, &event, &out);
    if (!app_replay_expect_true(out.led_on, "attempt LED should be on after request")) {
        return false;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.device_id = device_id;
    event.hid_cid = 0U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    app_replay_tick(&app, 2200U, false, &event, &out);

    return app_replay_expect_true(
        out.led_on,
        "close without an active device should not cancel pairing attempt LED"
    );
}

static bool app_replay_test_pairing_suppresses_disconnect_cue(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x15U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x51U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 1000U, false, &event, &out);

    app_replay_tick(&app, 5000U, true, NULL, &out);
    app_replay_tick(&app, 6000U, true, NULL, &out);
    if (!app_replay_expect_true(out.pairing_active, "pairing should start while connected")) {
        return false;
    }

    app_replay_tick(&app, 6100U, false, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.hid_cid = 0x51U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    app_replay_tick(&app, 6150U, false, &event, &out);
    if (!app_replay_expect_true(
            out.pairing_active,
            "disconnect during pairing should keep pairing mode active"
        )) {
        return false;
    }

    app_replay_tick(&app, 6200U, false, NULL, &out);
    app_replay_tick(&app, 6300U, false, NULL, &out);
    return app_replay_expect_true(
        !out.led_on,
        "disconnect during pairing should not replace the pairing blink with disconnect cue"
    );
}

static bool app_replay_test_sleep_adaptive_idle_vs_connected(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x11U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_u32_eq(
            out.sleep_us,
            5000U,
            "idle loop should use relaxed sleep budget"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x33U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 1000U, false, &event, &out);
    if (!app_replay_expect_u32_eq(
            out.sleep_us,
            5U,
            "active connection should use low-latency sleep budget"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.hid_cid = 0x33U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    app_replay_tick(&app, 1010U, false, &event, &out);
    return app_replay_expect_u32_eq(
        out.sleep_us,
        5000U,
        "sleep budget should relax again after disconnect"
    );
}

static bool app_replay_test_remove_last_double_long_press_recent_only(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x01U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 1000U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 5000U, true, NULL, &out);
    app_replay_tick(&app, 6100U, true, NULL, &out);
    app_replay_tick(&app, 7100U, false, NULL, &out);
    app_replay_tick(&app, 7500U, true, NULL, &out);
    app_replay_tick(&app, 8600U, true, NULL, &out);

    if (!app_replay_expect_u32_eq(
            pair_db_count(&app.pair_db),
            0U,
            "pair db should remove last on double long press"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            out.forget_request.valid,
            "remove-last should emit forget request"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            app_replay_device_id_equal(&out.forget_request.device_id, &device_id),
            "forget request device id should match removed device"
        )) {
        return false;
    }

    app_replay_tick(&app, 9550U, false, NULL, &out);
    return app_replay_expect_u32_eq(
        pair_db_count(&app.pair_db),
        0U,
        "remove-last result should persist after release"
    );
}

static bool app_replay_test_remove_last_ignored_when_not_recent(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x02U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 1000U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, (60U * 60U * 1000U) + 5000U, true, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 6100U, true, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 7100U, false, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 7500U, true, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 8600U, true, NULL, &out);

    if (!app_replay_expect_u32_eq(
            pair_db_count(&app.pair_db),
            1U,
            "remove-last should do nothing when last pairing is stale"
        )) {
        return false;
    }

    return app_replay_expect_true(
        !out.forget_request.valid,
        "stale remove-last should not emit forget request"
    );
}

static bool app_replay_test_remove_all_very_long_press(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t first_device = app_replay_device_id(0x03U);
    const pair_device_id_t second_device = app_replay_device_id(0x04U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &first_device, 10U)) {
        return false;
    }
    if (!pair_db_add(&initial_pair_db, &second_device, 20U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            pair_db_count(&app.pair_db),
            2U,
            "no reset before very long threshold"
        )) {
        return false;
    }

    app_replay_tick(&app, 11001U, true, NULL, &out);
    if (!app_replay_expect_u32_eq(
            pair_db_count(&app.pair_db),
            0U,
            "very long press should clear pair db"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !out.factory_reset_requested,
            "factory reset reboot should wait while BOOTSEL remains held"
        )) {
        return false;
    }

    app_replay_tick(&app, 11010U, false, NULL, &out);
    return app_replay_expect_true(
        out.factory_reset_requested,
        "factory reset reboot should trigger after BOOTSEL release"
    );
}

static bool app_replay_test_remove_all_waits_across_late_button_release_before_reboot(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x44U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 10U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 11001U, true, NULL, &out);
    app_replay_tick(&app, 13610U, true, NULL, &out);
    if (!app_replay_expect_true(
            !out.factory_reset_requested,
            "factory reset reboot should keep waiting while BOOTSEL remains held"
        )) {
        return false;
    }

    app_replay_tick(&app, 13620U, false, NULL, &out);
    return app_replay_expect_true(
        out.factory_reset_requested,
        "factory reset reboot should trigger after BOOTSEL release"
    );
}

static bool app_replay_test_reconnect_backoff_schedule(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x05U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "initial reconnect request expected"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
    app_replay_tick(&app, 100U, false, &event, &out);

    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "reconnect should back off after failure"
        )) {
        return false;
    }

    app_replay_tick(&app, 700U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "reconnect should retry after first backoff window"
    );
}

static bool app_replay_test_reconnect_connect_failed_no_lockout(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x0CU);
    const uint32_t attempt_times_ms[] = {
        0U,
        700U,
        1800U,
        3900U,
        6000U,
        8100U,
        10200U,
        12300U,
        14400U,
    };
    size_t index = 0U;

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
    event.status_code = 0x3EU;

    for (index = 0U; index < (sizeof(attempt_times_ms) / sizeof(attempt_times_ms[0])); index++) {
        app_replay_tick(&app, attempt_times_ms[index], false, NULL, &out);
        if (!app_replay_expect_true(
                out.reconnect_request.valid,
                "connect failure path should continue scheduling reconnect attempts"
            )) {
            return false;
        }

        app_replay_tick(&app, attempt_times_ms[index] + 100U, false, &event, &out);
    }

    if (!app_replay_expect_u32_eq(
            app.pair_db.entries[0].reconnect_allowed,
            1U,
            "connect failure retries should not disable reconnect"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            app.pair_db.entries[0].reconnect_fail_count,
            9U,
            "connect failure retries should keep counting failures"
        )) {
        return false;
    }

    app_replay_tick(&app, 16600U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "connect failure retries should continue at capped backoff interval"
    );
}

static bool app_replay_test_reconnect_boot_epoch_normalized(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x0BU);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 300000U)) {
        return false;
    }

    if (!pair_db_mark_reconnect_failure(&initial_pair_db, &device_id, 4U, 450000U)) {
        return false;
    }

    if (!pair_db_set_reconnect_allowed(&initial_pair_db, &device_id, false)) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app_replay_tick(&app, 10U, false, NULL, &out);

    return app_replay_expect_true(
        out.reconnect_request.valid,
        "boot should normalize persisted reconnect lockout/backoff timestamps"
    );
}

static bool app_replay_test_reconnect_auth_failure_no_lockout(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x06U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "initial reconnect request expected"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED;
    event.status_code = 7U;
    app_replay_tick(&app, 100U, false, &event, &out);

    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "auth failure should block immediate reconnect"
        )) {
        return false;
    }

    app_replay_tick(&app, 700U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "first auth failure should back off, not lock out"
        )) {
        return false;
    }

    app_replay_tick(&app, 800U, false, &event, &out);
    app_replay_tick(&app, 1900U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "second auth failure should still retry after backoff"
        )) {
        return false;
    }

    app_replay_tick(&app, 2000U, false, &event, &out);
    app_replay_tick(&app, 4100U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "third auth failure should keep retrying with backoff"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            app.pair_db.entries[0].reconnect_allowed,
            1U,
            "auth failures should not disable reconnect"
        )) {
        return false;
    }

    app_replay_tick(&app, 10U * 60U * 1000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "inflight reconnect timeout should respect backoff before next retry"
        )) {
        return false;
    }

    app_replay_tick(&app, (10U * 60U * 1000U) + 2100U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "auth failure path should keep reconnecting after long idle intervals"
    );
}

static bool app_replay_test_reconnect_request_uses_last_link_hint(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x0AU);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x77U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.hid_cid = 0x77U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    app_replay_tick(&app, 1010U, false, &event, &out);

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "reconnect request should be emitted after close"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            app_replay_device_id_equal(&out.reconnect_request.device_id, &device_id),
            "reconnect request should target the closed device"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.reconnect_request.bt_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            "reconnect should keep LE link hint"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        out.reconnect_request.bt_addr_type,
        HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM,
        "reconnect should keep LE address-type hint"
    );
}

static bool app_replay_test_reconnect_fills_spare_active_slot(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t first_device = app_replay_device_id(0x31U);
    const pair_device_id_t second_device = app_replay_device_id(0x32U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &first_device, 10U)) {
        return false;
    }
    if (!pair_db_add(&initial_pair_db, &second_device, 20U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app_replay_tick(&app, 100U, false, NULL, &out);

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "boot should request reconnect for newest paired device"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            app_replay_device_id_equal(&out.reconnect_request.device_id, &second_device),
            "first reconnect should target most recently seen device"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = second_device;
    event.hid_cid = 0x32U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 110U, false, &event, &out);

    if (!app_replay_expect_u32_eq(
            out.active_device_count,
            1U,
            "first reconnect open should leave one active device"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "spare active slot should trigger reconnect for another paired device"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            app_replay_device_id_equal(&out.reconnect_request.device_id, &first_device),
            "second reconnect should skip the already active device"
        )) {
        return false;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = first_device;
    event.hid_cid = 0x31U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 120U, false, &event, &out);

    if (!app_replay_expect_u32_eq(
            out.active_device_count,
            2U,
            "second reconnect open should produce two active devices"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.usb_interface_count,
            2U,
            "two active devices should publish two USB interfaces"
        )) {
        return false;
    }

    return app_replay_expect_true(
        !out.reconnect_request.valid,
        "all paired active devices should stop reconnect scheduling"
    );
}

static bool app_replay_test_reconnect_success_waits_for_hid_open(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x33U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 10U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app_replay_tick(&app, 100U, false, NULL, &out);
    if (!app_replay_expect_true(out.reconnect_request.valid, "initial reconnect expected")) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_SUCCESS;
    app_replay_tick(&app, 110U, false, &event, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "success result should not reschedule before HID open"
        )) {
        return false;
    }

    app_replay_tick(&app, 120U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "pending successful reconnect should remain in flight until open"
        )) {
        return false;
    }

    (void)memset(&event, 0, sizeof(event));
    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x33U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_ACL;
    app_replay_tick(&app, 130U, false, &event, &out);

    if (!app_replay_expect_u32_eq(
            out.active_device_count,
            1U,
            "HID open should complete successful reconnect"
        )) {
        return false;
    }

    return app_replay_expect_true(
        !out.reconnect_request.valid,
        "active reconnected device should not be requested again"
    );
}

static bool app_replay_test_reconnect_close_stale_hid_cid_falls_back_to_device(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x0DU);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x77U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.device_id = device_id;
    event.hid_cid = 0x99U; /* stale/non-matching hid_cid */
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    app_replay_tick(&app, 1010U, false, &event, &out);

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "stale close hid_cid should still allow reconnect via device-id fallback"
        )) {
        return false;
    }

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.reconnect_request.device_id, &device_id),
        "fallback reconnect should target original device"
    );
}

static bool app_replay_test_reconnect_close_mismatched_link_falls_back_to_device(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x0EU);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x55U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_CLOSE;
    event.device_id = device_id;
    event.hid_cid = 0x00U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC; /* mismatched link metadata */
    app_replay_tick(&app, 1010U, false, &event, &out);

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "mismatched close link should still clear active session via device-id fallback"
        )) {
        return false;
    }

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.reconnect_request.device_id, &device_id),
        "device-id fallback should preserve reconnect target"
    );
}

static bool app_replay_test_known_open_preserves_stored_hid_metadata(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    uint32_t open_generation = 0U;
    const pair_device_id_t device_id = app_replay_device_id(0x45U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    if (!pair_db_touch_session(
            &initial_pair_db,
            &device_id,
            10U,
            0x05ACU,
            0x030DU,
            77U,
            HID_TRANSPORT_PROTOCOL_REPORT,
            HID_TRANSPORT_BT_LINK_TYPE_LE,
            HID_TRANSPORT_BT_ADDR_TYPE_LE_RANDOM_IDENTITY
        )) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app_replay_tick(&app, 100U, false, NULL, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x45U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_LE;
    event.bt_addr_type = HID_TRANSPORT_BT_ADDR_TYPE_UNKNOWN;
    event.report_descriptor_len = 0U;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_UNKNOWN;
    app_replay_tick(&app, 110U, false, &event, &out);
    open_generation = out.usb_descriptor_generation;

    if (!app_replay_expect_u32_eq(
            out.usb_interface_plan[0].report_descriptor_len,
            77U,
            "known open should reuse stored descriptor length"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.usb_interface_plan[0].protocol_mode,
            HID_TRANSPORT_PROTOCOL_REPORT,
            "known open should reuse stored protocol mode"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_DESCRIPTOR;
    event.report_descriptor_len = 77U;
    app_replay_tick(&app, 120U, false, &event, &out);
    if (!app_replay_expect_u32_eq(
            out.usb_descriptor_generation,
            open_generation,
            "matching descriptor event should not dirty USB topology"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_BT_HID_PROTOCOL;
    event.protocol_mode = HID_TRANSPORT_PROTOCOL_REPORT;
    app_replay_tick(&app, 130U, false, &event, &out);
    return app_replay_expect_u32_eq(
        out.usb_descriptor_generation,
        open_generation,
        "matching protocol event should not dirty USB topology"
    );
}

static bool app_replay_test_runtime_report_queue_preserves_unforwarded_on_overflow(void) {
    hid_transport_runtime_t runtime = {0};
    hid_transport_event_t event = {0};
    hid_transport_event_t out_event = {0};
    hid_transport_event_t last_event = {0};
    hid_transport_runtime_queue_state_t queue_state = {0};
    uint8_t index = 0U;
    uint8_t taken_count = 0U;

    hid_transport_runtime_init(&runtime);

    event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    event.hid_cid = 0x44U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.report_len = 1U;

    for (index = 0U; index < HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE; index++) {
        event.report[0] = (uint8_t)(index + 1U);
        if (!app_replay_expect_true(
                hid_transport_runtime_push_event(&runtime, &event),
                "initial report event should enqueue"
            )) {
            return false;
        }
    }

    event.report[0] = 0U;
    if (!app_replay_expect_true(
            !hid_transport_runtime_push_event(&runtime, &event),
            "newest report should be refused when queue is full"
        )) {
        return false;
    }

    if (!hid_transport_runtime_queue_state_get(&runtime, &queue_state)) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            queue_state.event_queue_depth,
            HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE,
            "overflow should keep queue at capacity"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            queue_state.event_queue_dropped,
            1U,
            "overflow should count the refused ingress report"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            hid_transport_runtime_take_event(&runtime, &out_event),
            "queue should have a first event after overflow"
        )) {
        return false;
    }

    taken_count = 1U;
    last_event = out_event;

    if (!app_replay_expect_u32_eq(
            out_event.report[0],
            1U,
            "overflow should preserve the oldest unforwarded report"
        )) {
        return false;
    }

    while (hid_transport_runtime_take_event(&runtime, &out_event)) {
        taken_count = (uint8_t)(taken_count + 1U);
        last_event = out_event;
    }

    if (!app_replay_expect_u32_eq(
            taken_count,
            HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE,
            "overflow should leave the expected number of events"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        last_event.report[0],
        HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE,
        "overflow should preserve queued reports in FIFO order"
    );
}

static bool app_replay_test_usb_bridge_tx_queue_buffers_report_burst(void) {
    usb_bridge_t bridge = {0};
    usb_bridge_telemetry_t telemetry = {0};
    hid_transport_usb_tx_t tx = {0};
    uint8_t report[1] = {0U};
    uint8_t index = 0U;
    uint8_t taken_count = 0U;

    usb_bridge_init(&bridge);
    bridge.exported_interface_count = 1U;
    bridge.interface_slot[0].used = true;
    bridge.interface_slot[0].interface_number = 0U;
    bridge.interface_slot[0].hid_cid = 0x44U;
    bridge.interface_slot[0].bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;

    for (index = 0U; index < USB_BRIDGE_TX_QUEUE_SIZE; index++) {
        report[0] = (uint8_t)(index + 1U);
        if (!app_replay_expect_true(
                usb_bridge_ingest_bt_report(
                    &bridge,
                    0x44U,
                    HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                    report,
                    sizeof(report)
                ),
                "USB bridge should buffer a full report burst"
            )) {
            return false;
        }
    }

    report[0] = 0U;
    if (!app_replay_expect_true(
            !usb_bridge_ingest_bt_report(
                &bridge,
                0x44U,
                HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
                report,
                sizeof(report)
            ),
            "USB bridge should refuse a report when the burst queue is full"
        )) {
        return false;
    }

    if (!usb_bridge_telemetry_get(&bridge, &telemetry)) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            telemetry.usb_tx_depth,
            USB_BRIDGE_TX_QUEUE_SIZE,
            "USB bridge burst queue depth should reach configured capacity"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            telemetry.usb_tx_dropped,
            1U,
            "USB bridge should count refused reports at capacity"
        )) {
        return false;
    }

    while (usb_bridge_take_usb_tx(&bridge, &tx)) {
        if (!app_replay_expect_u32_eq(
                tx.report[0],
                (uint32_t)(taken_count + 1U),
                "USB bridge burst queue should preserve FIFO report order"
            )) {
            return false;
        }

        taken_count = (uint8_t)(taken_count + 1U);
    }

    return app_replay_expect_u32_eq(
        taken_count,
        USB_BRIDGE_TX_QUEUE_SIZE,
        "USB bridge should drain the configured burst queue size"
    );
}

static bool app_replay_test_bt_report_routed_to_usb(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x07U);
    const uint8_t sample_report[] = {0xA1U, 0x02U, 0x03U};

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x44U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.vendor_id = 0x1234U;
    event.product_id = 0x5678U;
    event.report_descriptor_len = 63U;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    event.hid_cid = 0x44U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.report_len = (uint16_t)sizeof(sample_report);
    (void)memcpy(event.report, sample_report, sizeof(sample_report));
    app_replay_tick(&app, 1010U, false, &event, &out);

    if (!app_replay_expect_true(out.usb_tx.valid, "BT report should enqueue USB report")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.usb_tx.interface_number,
            0U,
            "first active device should map to USB interface 0"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.usb_tx.report_len,
            (uint32_t)sizeof(sample_report),
            "USB report length should match source report"
        )) {
        return false;
    }

    return app_replay_expect_true(
        memcmp(out.usb_tx.report, sample_report, sizeof(sample_report)) == 0,
        "USB report bytes should match source report"
    );
}

static bool app_replay_test_usb_tx_blocked_defers_bridge_dequeue(void) {
    app_t app = {0};
    app_output_t out = {0};
    app_input_t input = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x47U);
    const uint8_t sample_report[] = {0x11U, 0x22U, 0x33U};

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x47U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.report_descriptor_len = 63U;
    app_replay_tick(&app, 1000U, false, &event, &out);

    input.now_ms = 1010U;
    input.transport_event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    input.transport_event.hid_cid = 0x47U;
    input.transport_event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    input.transport_event.report_len = (uint16_t)sizeof(sample_report);
    input.usb_tx_blocked = true;
    (void)memcpy(input.transport_event.report, sample_report, sizeof(sample_report));

    (void)memset(&out, 0, sizeof(out));
    app_tick(&app, &input, &out);

    if (!app_replay_expect_true(!out.usb_tx.valid, "blocked USB tx should not be popped")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.usb_tx_queue_depth,
            1U,
            "blocked USB tx should remain queued in the bridge"
        )) {
        return false;
    }

    app_replay_tick(&app, 1020U, false, NULL, &out);
    if (!app_replay_expect_true(out.usb_tx.valid, "unblocked USB tx should pop queued report")) {
        return false;
    }

    return app_replay_expect_true(
        memcmp(out.usb_tx.report, sample_report, sizeof(sample_report)) == 0,
        "deferred USB tx report bytes should be preserved"
    );
}

static bool app_replay_test_usb_report_routed_to_bt(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x08U);
    const uint8_t sample_report[] = {0x04U, 0x05U, 0x06U, 0x07U};

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    event.type = HID_TRANSPORT_EVENT_BT_HID_OPEN;
    event.device_id = device_id;
    event.hid_cid = 0x66U;
    event.bt_link_type = HID_TRANSPORT_BT_LINK_TYPE_CLASSIC;
    event.report_descriptor_len = 77U;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_USB_HID_REPORT;
    event.interface_number = 0U;
    event.report_len = (uint16_t)sizeof(sample_report);
    (void)memcpy(event.report, sample_report, sizeof(sample_report));
    app_replay_tick(&app, 1010U, false, &event, &out);

    if (!app_replay_expect_true(out.bt_tx.valid, "USB report should enqueue BT report")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(out.bt_tx.hid_cid, 0x66U, "BT tx should target mapped hid_cid")) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.bt_tx.bt_link_type,
            HID_TRANSPORT_BT_LINK_TYPE_CLASSIC,
            "BT tx should keep mapped link type"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.bt_tx.report_len,
            (uint32_t)sizeof(sample_report),
            "BT report length should match source report"
        )) {
        return false;
    }

    return app_replay_expect_true(
        memcmp(out.bt_tx.report, sample_report, sizeof(sample_report)) == 0,
        "BT report bytes should match source report"
    );
}

static bool app_replay_test_hid_device_map_profile_detection(void) {
    if (!app_replay_expect_u32_eq(
            hid_device_map_profile_detect(0x05ACU, 0x0267U),
            HID_DEVICE_MAP_PROFILE_APPLE_MAGIC_KEYBOARD,
            "Apple Magic Keyboard VID/PID should map to apple profile"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        hid_device_map_profile_detect(0x1234U, 0x5678U),
        HID_DEVICE_MAP_PROFILE_NONE,
        "unknown VID/PID should map to none profile"
    );
}

static bool app_replay_test_hid_device_map_fn_esc_toggle(void) {
    hid_device_map_state_t state = {0};
    const uint8_t combo_report[] = {0x00U, 0x00U, 0x29U, 0x3FU, 0x00U, 0x00U, 0x00U, 0x00U};
    const uint8_t release_report[] = {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

    hid_device_map_state_reset(&state, 0x05ACU, 0x0267U);

    if (!app_replay_expect_u32_eq(
            state.fn_mode,
            HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT,
            "Apple profile should default to media mode"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            hid_device_map_track_fn_esc_toggle(
                &state,
                combo_report,
                (uint16_t)sizeof(combo_report)
            ),
            "first Fn+Esc combo should toggle mode"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            state.fn_mode,
            HID_DEVICE_MAP_FN_MODE_FUNCTION_DEFAULT,
            "Fn+Esc should switch to function mode"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !hid_device_map_track_fn_esc_toggle(
                &state,
                combo_report,
                (uint16_t)sizeof(combo_report)
            ),
            "held combo should not retrigger while latched"
        )) {
        return false;
    }

    (void)hid_device_map_track_fn_esc_toggle(
        &state,
        release_report,
        (uint16_t)sizeof(release_report)
    );

    if (!app_replay_expect_true(
            hid_device_map_track_fn_esc_toggle(
                &state,
                combo_report,
                (uint16_t)sizeof(combo_report)
            ),
            "second Fn+Esc combo should toggle mode back"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        state.fn_mode,
        HID_DEVICE_MAP_FN_MODE_MEDIA_DEFAULT,
        "Fn+Esc should switch back to media mode"
    );
}

/*
 * Encode one Magic Trackpad 2 vendor touch record (the inverse of the
 * decode in apple_trackpad.c): x is 13-bit signed across bytes 0-1, y is
 * 13-bit signed (sensor-negated) across bytes 1-3, state 0x80 in byte 3
 * means touching, id is the low nibble of byte 8.
 */
static void app_replay_trackpad_encode_touch(
    uint8_t * record,
    int16_t x,
    int16_t y,
    uint8_t touch_id,
    bool down
) {
    const uint16_t raw_x = (uint16_t)x & 0x1FFFU;
    const uint16_t raw_y = (uint16_t)(-y) & 0x1FFFU;

    (void)memset(record, 0, 9U);
    record[0] = (uint8_t)(raw_x & 0xFFU);
    record[1] = (uint8_t)(((raw_x >> 8U) & 0x1FU) | ((raw_y & 0x07U) << 5U));
    record[2] = (uint8_t)((raw_y >> 3U) & 0xFFU);
    record[3] = (uint8_t)(((raw_y >> 11U) & 0x03U) | (down ? 0x80U : 0x00U));
    record[8] = (uint8_t)(touch_id & 0x0FU);
}

typedef struct {
    int16_t x;
    int16_t y;
    uint8_t touch_id;
    bool down;
} app_replay_trackpad_touch_t;

static uint16_t app_replay_trackpad_frame(
    uint8_t * buf,
    uint8_t button,
    const app_replay_trackpad_touch_t * touches,
    uint8_t touch_count
) {
    uint8_t i = 0U;

    buf[0] = 0x31U; /* Magic Trackpad 2 Bluetooth multitouch report ID */
    buf[1] = button;
    buf[2] = 0U;
    buf[3] = 0U;
    for (i = 0U; i < touch_count; i++) {
        app_replay_trackpad_encode_touch(
            &buf[4U + ((uint16_t)i * 9U)],
            touches[i].x,
            touches[i].y,
            touches[i].touch_id,
            touches[i].down
        );
    }
    return (uint16_t)(4U + ((uint16_t)touch_count * 9U));
}

static bool app_replay_test_trackpad_recognition(void) {
    uint8_t report_id = 0U;
    uint8_t payload[4] = {0};
    uint8_t payload_len = 0U;

    if (!app_replay_expect_true(
            apple_trackpad_is_supported(0x05ACU, 0x0265U)
                && apple_trackpad_is_supported(0x05ACU, 0x0324U)
                && apple_trackpad_is_supported(0x05ACU, 0x030EU),
            "known Apple trackpad PIDs should be supported"
        )) {
        return false;
    }
    if (!app_replay_expect_true(
            !apple_trackpad_is_supported(0x05ACU, 0x0267U)
                && !apple_trackpad_is_supported(0x046DU, 0x0265U),
            "keyboard PID and foreign vendor should not be supported"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            apple_trackpad_mt_enable_report(0x0265U, &report_id, payload, &payload_len),
            "Magic Trackpad 2 should have an enable report"
        )) {
        return false;
    }
    if (!app_replay_expect_true(
            (report_id == 0xF1U)
                && (payload_len == 2U)
                && (payload[0] == 0x02U)
                && (payload[1] == 0x01U),
            "Magic Trackpad 2 enable report should be F1 02 01"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            apple_trackpad_mt_enable_report(0x030EU, &report_id, payload, &payload_len),
            "original Magic Trackpad should have an enable report"
        )) {
        return false;
    }
    return app_replay_expect_true(
        (report_id == 0xD7U) && (payload_len == 1U) && (payload[0] == 0x01U),
        "original Magic Trackpad enable report should be D7 01"
    );
}

static bool app_replay_test_trackpad_pointer_motion(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touch = {.x = 100, .y = -50, .touch_id = 3U, .down = true};

    apple_trackpad_state_init(&state, 0x0265U);

    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    if (!app_replay_expect_true(
            apple_trackpad_process_report(&state, frame, frame_len, 0U, &out),
            "multitouch frame should be consumed"
        )) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.count, 0U, "touch-down frame should emit nothing")) {
        return false;
    }

    touch.x = 300;
    touch.y = 150;
    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    out.count = 0U;
    if (!app_replay_expect_true(
            apple_trackpad_process_report(&state, frame, frame_len, 10U, &out),
            "movement frame should be consumed"
        )) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.count, 1U, "movement should emit one mouse report")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(
            out.bytes[0][0],
            APPLE_TRACKPAD_MOUSE_REPORT_ID,
            "report should use the synthesized mouse report ID"
        )) {
        return false;
    }
    /* 200 raw units / pointer divider 2 = 100 counts on both axes. */
    if (!app_replay_expect_u32_eq(
            (uint32_t)(uint16_t)(out.bytes[0][2] | (out.bytes[0][3] << 8U)),
            100U,
            "x delta should be scaled raw motion"
        )) {
        return false;
    }
    return app_replay_expect_u32_eq(
        (uint32_t)(uint16_t)(out.bytes[0][4] | (out.bytes[0][5] << 8U)),
        100U,
        "y delta should be scaled raw motion"
    );
}

static bool app_replay_test_trackpad_two_finger_scroll(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touches[2] = {
        {.x = 0, .y = 0, .touch_id = 0U, .down = true},
        {.x = 500, .y = 0, .touch_id = 1U, .down = true},
    };

    apple_trackpad_state_init(&state, 0x0265U);

    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);
    if (!app_replay_expect_u32_eq(out.count, 0U, "two-finger touch-down should emit nothing")) {
        return false;
    }

    /* Both fingers move up 128 units: wheel +2 (traditional scroll-up). */
    touches[0].y = -128;
    touches[1].y = -128;
    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    out.count = 0U;
    if (!app_replay_expect_true(
            apple_trackpad_process_report(&state, frame, frame_len, 10U, &out),
            "scroll frame should be consumed"
        )) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.count, 1U, "scroll should emit one mouse report")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.bytes[0][6], 2U, "fingers up should be wheel +2")) {
        return false;
    }
    if (!app_replay_expect_true(
            (out.bytes[0][2] == 0U)
                && (out.bytes[0][3] == 0U)
                && (out.bytes[0][4] == 0U)
                && (out.bytes[0][5] == 0U),
            "scroll should not move the pointer"
        )) {
        return false;
    }

    /* Lifting one finger changes the count; no motion may leak that frame. */
    touches[0].down = false;
    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 20U, &out);
    return app_replay_expect_u32_eq(out.count, 0U, "finger-count change should emit nothing");
}

static bool app_replay_test_trackpad_click_and_passthrough(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    uint8_t plain_mouse_report[4] = {0x02U, 0x00U, 0x05U, 0x00U};
    app_replay_trackpad_touch_t touch = {.x = 0, .y = 0, .touch_id = 0U, .down = true};

    apple_trackpad_state_init(&state, 0x0265U);

    /* Pre-enable plain-mouse reports are not consumed (forwarded raw). */
    if (!app_replay_expect_true(
            !apple_trackpad_process_report(
                &state,
                plain_mouse_report,
                (uint16_t)sizeof(plain_mouse_report),
                0U,
                &out
            ),
            "non-multitouch report should not be consumed"
        )) {
        return false;
    }

    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);

    frame_len = app_replay_trackpad_frame(frame, 1U, &touch, 1U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 10U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "click should emit one mouse report")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.bytes[0][1], 1U, "physical click should press button 1")) {
        return false;
    }

    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 20U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "release should emit one mouse report")) {
        return false;
    }
    return app_replay_expect_u32_eq(out.bytes[0][1], 0U, "release should clear button 1");
}

static bool app_replay_test_trackpad_tap_to_click(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touch = {.x = 0, .y = 0, .touch_id = 0U, .down = true};

    apple_trackpad_state_init(&state, 0x0265U);

    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);

    /* All fingers up after 100 ms with no movement: tap press is emitted. */
    frame_len = app_replay_trackpad_frame(frame, 0U, NULL, 0U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 100U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "tap liftoff should emit the click press")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.bytes[0][1], 1U, "single-finger tap should press button 1")) {
        return false;
    }

    /* Release is timed: nothing before the pulse deadline, release after. */
    out.count = 0U;
    apple_trackpad_tick(&state, 110U, &out);
    if (!app_replay_expect_u32_eq(out.count, 0U, "tap release should wait for the pulse")) {
        return false;
    }
    out.count = 0U;
    apple_trackpad_tick(&state, 140U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "tap release should fire after the pulse")) {
        return false;
    }
    return app_replay_expect_u32_eq(out.bytes[0][1], 0U, "tap release should clear the button");
}

static bool app_replay_test_trackpad_multi_finger_taps(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touches[3] = {
        {.x = 0, .y = 0, .touch_id = 0U, .down = true},
        {.x = 500, .y = 0, .touch_id = 1U, .down = true},
        {.x = 1000, .y = 0, .touch_id = 2U, .down = true},
    };

    /* Two-finger tap = right click. */
    apple_trackpad_state_init(&state, 0x0265U);
    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);
    frame_len = app_replay_trackpad_frame(frame, 0U, NULL, 0U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 100U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "two-finger tap should emit a press")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(out.bytes[0][1], 2U, "two-finger tap should press button 2")) {
        return false;
    }
    out.count = 0U;
    apple_trackpad_tick(&state, 200U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "two-finger tap should release by time")) {
        return false;
    }

    /* Three-finger tap = middle click. */
    apple_trackpad_state_init(&state, 0x0265U);
    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 3U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);
    frame_len = app_replay_trackpad_frame(frame, 0U, NULL, 0U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 100U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "three-finger tap should emit a press")) {
        return false;
    }
    return app_replay_expect_u32_eq(out.bytes[0][1], 4U, "three-finger tap should press button 3");
}

static bool app_replay_test_trackpad_tap_suppression(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touch = {.x = 0, .y = 0, .touch_id = 0U, .down = true};

    /* A touch that moved is not a tap. */
    apple_trackpad_state_init(&state, 0x0265U);
    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);
    touch.x = 400;
    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 50U, &out);
    frame_len = app_replay_trackpad_frame(frame, 0U, NULL, 0U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 100U, &out);
    if (!app_replay_expect_u32_eq(out.count, 0U, "moved touch should not click")) {
        return false;
    }

    /* A touch that stayed down too long is not a tap. */
    apple_trackpad_state_init(&state, 0x0265U);
    touch.x = 0;
    frame_len = app_replay_trackpad_frame(frame, 0U, &touch, 1U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);
    frame_len = app_replay_trackpad_frame(frame, 0U, NULL, 0U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 400U, &out);
    return app_replay_expect_u32_eq(out.count, 0U, "slow touch should not click");
}

static bool app_replay_test_trackpad_multi_finger_physical_click(void) {
    apple_trackpad_state_t state = {0};
    apple_trackpad_out_t out = {0};
    uint8_t frame[64] = {0};
    uint16_t frame_len = 0U;
    app_replay_trackpad_touch_t touches[2] = {
        {.x = 0, .y = 0, .touch_id = 0U, .down = true},
        {.x = 500, .y = 0, .touch_id = 1U, .down = true},
    };

    apple_trackpad_state_init(&state, 0x0265U);

    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    (void)apple_trackpad_process_report(&state, frame, frame_len, 0U, &out);

    frame_len = app_replay_trackpad_frame(frame, 1U, touches, 2U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 10U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "two-finger click should emit a report")) {
        return false;
    }
    if (!app_replay_expect_u32_eq(
            out.bytes[0][1],
            2U,
            "two-finger physical click should press button 2"
        )) {
        return false;
    }

    /* Lifting a finger mid-press must not morph the held button. */
    touches[1].down = false;
    frame_len = app_replay_trackpad_frame(frame, 1U, touches, 2U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 20U, &out);
    if (!app_replay_expect_u32_eq(out.count, 0U, "held click should not re-report")) {
        return false;
    }

    frame_len = app_replay_trackpad_frame(frame, 0U, touches, 2U);
    out.count = 0U;
    (void)apple_trackpad_process_report(&state, frame, frame_len, 30U, &out);
    if (!app_replay_expect_u32_eq(out.count, 1U, "click release should emit a report")) {
        return false;
    }
    return app_replay_expect_u32_eq(out.bytes[0][1], 0U, "click release should clear buttons");
}

static bool app_replay_test_trackpad_descriptor_augment(void) {
    const uint8_t base[5] = {0x05U, 0x01U, 0x09U, 0x02U, 0xC0U};
    uint8_t out_buf[512] = {0};
    uint16_t total = 0U;

    total = apple_trackpad_augment_descriptor(
        0x0265U,
        base,
        (uint16_t)sizeof(base),
        out_buf,
        (uint16_t)sizeof(out_buf)
    );
    if (!app_replay_expect_true(
            total > (uint16_t)sizeof(base),
            "augmented descriptor should grow beyond the base"
        )) {
        return false;
    }
    if (!app_replay_expect_true(
            memcmp(out_buf, base, sizeof(base)) == 0,
            "augmented descriptor should start with the base descriptor"
        )) {
        return false;
    }
    if (!app_replay_expect_true(
            (out_buf[sizeof(base)] == 0x05U)
                && (out_buf[sizeof(base) + 1U] == 0x01U)
                && (out_buf[total - 1U] == 0xC0U),
            "appended collection should be the relay mouse collection"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        apple_trackpad_augment_descriptor(
            0x0267U,
            base,
            (uint16_t)sizeof(base),
            out_buf,
            (uint16_t)sizeof(out_buf)
        ),
        0U,
        "unsupported product should not augment"
    );
}

int main(void) {
    static const app_replay_test_case_t test_cases[] = {
        {.name = "led_startup_cue_short", .fn = app_replay_test_led_startup_cue_short},
        {.name = "led_signal_preempt_disconnect_cue",
            .fn = app_replay_test_led_signal_preempt_disconnect_cue},
        {.name = "pairing_entry_clears_active_signal",
            .fn = app_replay_test_pairing_entry_clears_active_signal},
        {.name = "pair_any_from_long_press", .fn = app_replay_test_pair_any_from_long_press},
        {.name = "pair_classic_after_extended_hold",
            .fn = app_replay_test_pair_classic_after_extended_hold},
        {.name = "pairing_led_cadence_follows_link_type",
            .fn = app_replay_test_pairing_led_cadence_follows_link_type},
        {.name = "duplicate_pair_command_keeps_attempt_led",
            .fn = app_replay_test_duplicate_pair_command_keeps_attempt_led},
        {.name = "pair_any_timeout_after_60s", .fn = app_replay_test_pair_any_timeout_after_60s},
        {.name = "pair_any_cancelled_by_single_click",
            .fn = app_replay_test_pair_any_cancelled_by_single_click},
        {.name = "pairing_cancel_while_connected_has_no_success_cue",
            .fn = app_replay_test_pairing_cancel_while_connected_has_no_success_cue},
        {.name = "pairing_attempt_led_lifecycle",
            .fn = app_replay_test_pairing_attempt_led_lifecycle},
        {.name = "pairing_auth_failure_uses_double_blink",
            .fn = app_replay_test_pairing_auth_failure_uses_double_blink},
        {.name = "pairing_phase_failures_use_diagnostic_blinks",
            .fn = app_replay_test_pairing_phase_failures_use_diagnostic_blinks},
        {.name = "pairing_close_without_active_device_keeps_attempt_led",
            .fn = app_replay_test_pairing_close_without_active_device_keeps_attempt_led},
        {.name = "pairing_suppresses_disconnect_cue",
            .fn = app_replay_test_pairing_suppresses_disconnect_cue},
        {.name = "sleep_adaptive_idle_vs_connected",
            .fn = app_replay_test_sleep_adaptive_idle_vs_connected},
        {.name = "remove_last_double_long_press_recent_only",
            .fn = app_replay_test_remove_last_double_long_press_recent_only},
        {.name = "remove_last_ignored_when_not_recent",
            .fn = app_replay_test_remove_last_ignored_when_not_recent},
        {.name = "remove_all_very_long_press", .fn = app_replay_test_remove_all_very_long_press},
        {.name = "remove_all_waits_across_late_button_release_before_reboot",
            .fn = app_replay_test_remove_all_waits_across_late_button_release_before_reboot},
        {.name = "reconnect_backoff_schedule", .fn = app_replay_test_reconnect_backoff_schedule},
        {.name = "reconnect_connect_failed_no_lockout",
            .fn = app_replay_test_reconnect_connect_failed_no_lockout},
        {.name = "reconnect_boot_epoch_normalized",
            .fn = app_replay_test_reconnect_boot_epoch_normalized},
        {.name = "reconnect_auth_failure_no_lockout",
            .fn = app_replay_test_reconnect_auth_failure_no_lockout},
        {.name = "reconnect_request_uses_last_link_hint",
            .fn = app_replay_test_reconnect_request_uses_last_link_hint},
        {.name = "reconnect_fills_spare_active_slot",
            .fn = app_replay_test_reconnect_fills_spare_active_slot},
        {.name = "reconnect_success_waits_for_hid_open",
            .fn = app_replay_test_reconnect_success_waits_for_hid_open},
        {.name = "reconnect_close_stale_hid_cid_falls_back_to_device",
            .fn = app_replay_test_reconnect_close_stale_hid_cid_falls_back_to_device},
        {.name = "reconnect_close_mismatched_link_falls_back_to_device",
            .fn = app_replay_test_reconnect_close_mismatched_link_falls_back_to_device},
        {.name = "known_open_preserves_stored_hid_metadata",
            .fn = app_replay_test_known_open_preserves_stored_hid_metadata},
        {.name = "runtime_report_queue_preserves_unforwarded_on_overflow",
            .fn = app_replay_test_runtime_report_queue_preserves_unforwarded_on_overflow},
        {.name = "usb_bridge_tx_queue_buffers_report_burst",
            .fn = app_replay_test_usb_bridge_tx_queue_buffers_report_burst},
        {.name = "bt_report_routed_to_usb", .fn = app_replay_test_bt_report_routed_to_usb},
        {.name = "usb_tx_blocked_defers_bridge_dequeue",
            .fn = app_replay_test_usb_tx_blocked_defers_bridge_dequeue},
        {.name = "usb_report_routed_to_bt", .fn = app_replay_test_usb_report_routed_to_bt},
        {.name = "hid_device_map_profile_detection",
            .fn = app_replay_test_hid_device_map_profile_detection},
        {.name = "hid_device_map_fn_esc_toggle",
            .fn = app_replay_test_hid_device_map_fn_esc_toggle},
        {.name = "trackpad_recognition", .fn = app_replay_test_trackpad_recognition},
        {.name = "trackpad_pointer_motion", .fn = app_replay_test_trackpad_pointer_motion},
        {.name = "trackpad_two_finger_scroll", .fn = app_replay_test_trackpad_two_finger_scroll},
        {.name = "trackpad_click_and_passthrough",
            .fn = app_replay_test_trackpad_click_and_passthrough},
        {.name = "trackpad_descriptor_augment", .fn = app_replay_test_trackpad_descriptor_augment},
        {.name = "trackpad_tap_to_click", .fn = app_replay_test_trackpad_tap_to_click},
        {.name = "trackpad_multi_finger_taps", .fn = app_replay_test_trackpad_multi_finger_taps},
        {.name = "trackpad_tap_suppression", .fn = app_replay_test_trackpad_tap_suppression},
        {.name = "trackpad_multi_finger_physical_click",
            .fn = app_replay_test_trackpad_multi_finger_physical_click},
    };
    const size_t test_count = sizeof(test_cases) / sizeof(test_cases[0]);
    size_t index = 0U;

    for (index = 0U; index < test_count; index++) {
        if (!test_cases[index].fn()) {
            (void)fprintf(stderr, "TEST FAILED: %s\n", test_cases[index].name);
            return 1;
        }
    }

    (void)printf(
        "app_replay: %lu/%lu tests passed\n",
        (unsigned long)test_count,
        (unsigned long)test_count
    );
    return 0;
}
