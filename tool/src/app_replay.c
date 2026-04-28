#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "hid_device_map.h"

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

    return app_replay_expect_true(
        !out.led_on,
        "pairing success should transition to connected state without a long on cue"
    );
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

    app_replay_tick(&app, 2200U, false, NULL, &out);
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
        2100U,
        6200U,
        14300U,
        22400U,
        30500U,
        38600U,
        46700U,
        54800U,
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

    app_replay_tick(&app, 62900U, false, NULL, &out);
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

    app_replay_tick(&app, 2200U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "first auth failure should back off, not lock out"
        )) {
        return false;
    }

    app_replay_tick(&app, 2300U, false, &event, &out);
    app_replay_tick(&app, 6300U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "second auth failure should still retry after backoff"
        )) {
        return false;
    }

    app_replay_tick(&app, 6400U, false, &event, &out);
    app_replay_tick(&app, 14500U, false, NULL, &out);
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

    app_replay_tick(&app, (10U * 60U * 1000U) + 8100U, false, NULL, &out);
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

int main(void) {
    static const app_replay_test_case_t test_cases[] = {
        {.name = "led_startup_cue_short", .fn = app_replay_test_led_startup_cue_short},
        {.name = "led_signal_preempt_disconnect_cue",
            .fn = app_replay_test_led_signal_preempt_disconnect_cue},
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
        {.name = "pairing_attempt_led_lifecycle",
            .fn = app_replay_test_pairing_attempt_led_lifecycle},
        {.name = "pairing_auth_failure_uses_double_blink",
            .fn = app_replay_test_pairing_auth_failure_uses_double_blink},
        {.name = "pairing_phase_failures_use_diagnostic_blinks",
            .fn = app_replay_test_pairing_phase_failures_use_diagnostic_blinks},
        {.name = "pairing_close_without_active_device_keeps_attempt_led",
            .fn = app_replay_test_pairing_close_without_active_device_keeps_attempt_led},
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
        {.name = "reconnect_close_stale_hid_cid_falls_back_to_device",
            .fn = app_replay_test_reconnect_close_stale_hid_cid_falls_back_to_device},
        {.name = "reconnect_close_mismatched_link_falls_back_to_device",
            .fn = app_replay_test_reconnect_close_mismatched_link_falls_back_to_device},
        {.name = "known_open_preserves_stored_hid_metadata",
            .fn = app_replay_test_known_open_preserves_stored_hid_metadata},
        {.name = "bt_report_routed_to_usb", .fn = app_replay_test_bt_report_routed_to_usb},
        {.name = "usb_report_routed_to_bt", .fn = app_replay_test_usb_report_routed_to_bt},
        {.name = "hid_device_map_profile_detection",
            .fn = app_replay_test_hid_device_map_profile_detection},
        {.name = "hid_device_map_fn_esc_toggle",
            .fn = app_replay_test_hid_device_map_fn_esc_toggle},
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
