#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "hid_report_remap.h"
#include "operator_command.h"

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

static void app_replay_tick_with_command(
    app_t * app,
    uint32_t now_ms,
    bool button_pressed,
    app_operator_command_t operator_command,
    const hid_transport_event_t * event,
    app_output_t * out
) {
    app_input_t input = {0};

    if ((app == NULL) || (out == NULL)) {
        return;
    }

    input.button_pressed = button_pressed;
    input.now_ms = now_ms;
    input.operator_command = operator_command;
    input.transport_event.type = HID_TRANSPORT_EVENT_NONE;

    if (event != NULL) {
        input.transport_event = *event;
    }

    (void)memset(out, 0, sizeof(*out));
    app_tick(app, &input, out);
}

static void app_replay_tick(
    app_t * app,
    uint32_t now_ms,
    bool button_pressed,
    const hid_transport_event_t * event,
    app_output_t * out
) {
    app_replay_tick_with_command(
        app,
        now_ms,
        button_pressed,
        APP_OPERATOR_COMMAND_NONE,
        event,
        out
    );
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

static bool app_replay_test_pair_any_from_long_press(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    if (!app_replay_expect_true(!out.pairing_active, "pairing should be inactive on press start")) {
        return false;
    }

    app_replay_tick(&app, 3100U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.pairing_active,
            "pairing should still wait for double-long window"
        )) {
        return false;
    }

    app_replay_tick(&app, 5601U, false, NULL, &out);
    return app_replay_expect_true(
        out.pairing_active,
        "pair-any should activate after long press window"
    );
}

static bool app_replay_test_remove_last_double_long_press(void) {
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
    app_replay_tick(&app, 7100U, false, NULL, &out);
    app_replay_tick(&app, 7500U, true, NULL, &out);
    app_replay_tick(&app, 9550U, false, NULL, &out);

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

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.forget_request.device_id, &device_id),
        "forget request device id should match removed device"
    );
}

static bool app_replay_test_reconnect_backoff_schedule(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x02U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "initial reconnect request should be emitted"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
    app_replay_tick(&app, 100U, false, &event, &out);

    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "reconnect should back off after connect failure"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            app.reconnect_failure_count,
            1U,
            "connect failure should update failure counter"
        )) {
        return false;
    }

    app_replay_tick(&app, 4000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "reconnect should not fire before retry window"
        )) {
        return false;
    }

    app_replay_tick(&app, 5101U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "reconnect should fire after retry window"
        )) {
        return false;
    }

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.reconnect_request.device_id, &device_id),
        "reconnect request should target paired device"
    );
}

static bool app_replay_test_auth_lockout_recovery(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x03U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    app_replay_tick(&app, 0U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "initial reconnect request should be emitted"
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_AUTH_FAILED;
    app_replay_tick(&app, 100U, false, &event, &out);

    if (!app_replay_expect_true(
            out.security_rotate_request.valid,
            "auth failure should emit security rotate request"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            app_replay_device_id_equal(&out.security_rotate_request.device_id, &device_id),
            "security rotate request should target auth-failed device"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.security_rotate_request.reason,
            HID_TRANSPORT_SECURITY_ROTATE_REASON_AUTH_FAILURE,
            "security rotate reason should classify auth failures"
        )) {
        return false;
    }

    app_replay_tick(&app, 3600099U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "auth lockout should suppress reconnect before expiry"
        )) {
        return false;
    }

    app_replay_tick(&app, 3600101U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "auth lockout should auto-recover after cooldown expiry"
    );
}

static bool app_replay_test_threshold_lockout_recovery(void) {
    app_t app = {0};
    app_output_t out = {0};
    hid_transport_event_t event = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x04U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app.reconnect_inflight = true;
    app.reconnect_device_id = device_id;
    app.reconnect_started_ms = 0U;

    if (!pair_db_mark_reconnect_failure(&app.pair_db, &device_id, 7U, 0U)) {
        return false;
    }

    if (!pair_db_set_reconnect_allowed(&app.pair_db, &device_id, true)) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_RECONNECT_RESULT;
    event.device_id = device_id;
    event.reconnect_result = HID_TRANSPORT_RECONNECT_RESULT_CONNECT_FAILED;
    app_replay_tick(&app, 500U, false, &event, &out);

    app_replay_tick(&app, 10000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "threshold lockout should suppress reconnect before expiry"
        )) {
        return false;
    }

    app_replay_tick(&app, 600501U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "threshold lockout should auto-recover after cooldown expiry"
    );
}

static bool app_replay_test_bridge_drops_oldest_on_overflow(void) {
    pair_db_t pair_db = {0};
    bt_manager_t manager = {0};
    usb_bridge_t bridge = {0};
    usb_bridge_telemetry_t telemetry = {0};
    hid_transport_usb_tx_t tx = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x05U);
    uint8_t report_counter = 0U;

    pair_db_init(&pair_db);
    if (!pair_db_add(&pair_db, &device_id, 0U)) {
        return false;
    }

    bt_manager_init(&manager, &pair_db);
    if (!bt_manager_ingest_hid_open(&manager, &device_id, 0x0042U, 0U, 0U, 0U, 10U)) {
        return false;
    }

    usb_bridge_init(&bridge);
    usb_bridge_sync_from_bt_manager(&bridge, &manager);

    for (report_counter = 0U; report_counter <= USB_BRIDGE_MAX_INTERFACE; report_counter++) {
        const uint8_t report[1] = {report_counter};

        if (!usb_bridge_ingest_bt_report(&bridge, 0x0042U, report, 1U)) {
            return false;
        }
    }

    if (!usb_bridge_telemetry_get(&bridge, &telemetry)) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            telemetry.usb_tx_dropped,
            1U,
            "bridge should drop oldest report on queue overflow"
        )) {
        return false;
    }

    if (!usb_bridge_take_usb_tx(&bridge, &tx)) {
        return false;
    }

    if (!app_replay_expect_true(tx.valid, "bridge dequeue should produce a valid report")) {
        return false;
    }

    return app_replay_expect_u32_eq(tx.report[0], 1U, "oldest report should be dropped first");
}

static bool app_replay_test_remap_boot_keyboard_bt_to_usb(void) {
    uint8_t output[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t output_len = 0U;
    const uint8_t bt_report_with_id[] =
        {0x01U, 0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

    if (!hid_report_remap_bt_to_usb(
            HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD,
            bt_report_with_id,
            (uint16_t)sizeof(bt_report_with_id),
            output,
            &output_len
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            output_len,
            8U,
            "boot keyboard BT->USB should drop report-id byte"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        output[2],
        0x04U,
        "boot keyboard keycode payload should be preserved"
    );
}

static bool app_replay_test_remap_boot_keyboard_usb_to_bt_report_mode(void) {
    uint8_t output[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t output_len = 0U;
    const uint8_t usb_report[] = {0x00U, 0x00U, 0x04U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U};

    if (!hid_report_remap_usb_to_bt(
            HID_REPORT_REMAP_PROFILE_BOOT_KEYBOARD,
            HID_TRANSPORT_PROTOCOL_REPORT,
            usb_report,
            (uint16_t)sizeof(usb_report),
            output,
            &output_len
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            output_len,
            9U,
            "boot keyboard USB->BT report mode should prepend report-id byte"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(output[0], 0x01U, "boot keyboard report-id hint should be 1")) {
        return false;
    }

    return app_replay_expect_u32_eq(output[3], 0x04U, "boot keyboard payload should be preserved");
}

static bool app_replay_test_remap_boot_mouse_usb_to_bt_boot_mode(void) {
    uint8_t output[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t output_len = 0U;
    const uint8_t usb_report[] = {0x01U, 0x10U, 0xF0U};

    if (!hid_report_remap_usb_to_bt(
            HID_REPORT_REMAP_PROFILE_BOOT_MOUSE,
            HID_TRANSPORT_PROTOCOL_BOOT,
            usb_report,
            (uint16_t)sizeof(usb_report),
            output,
            &output_len
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            output_len,
            3U,
            "boot mouse USB->BT boot mode should remain payload-only"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(output[2], 0xF0U, "boot mouse payload should be preserved");
}

static bool app_replay_test_operator_clear_lockout_last(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x06U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);

    if (!pair_db_mark_reconnect_failure(&app.pair_db, &device_id, 3U, 60000U)) {
        return false;
    }

    if (!pair_db_set_reconnect_allowed(&app.pair_db, &device_id, false)) {
        return false;
    }

    app_replay_tick_with_command(
        &app,
        1000U,
        false,
        APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_LAST,
        NULL,
        &out
    );

    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "operator clear lockout last should re-enable reconnect scheduling"
        )) {
        return false;
    }

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.reconnect_request.device_id, &device_id),
        "clear lockout last should schedule reconnect for last paired device"
    );
}

static bool app_replay_test_operator_rotate_security_last(void) {
    app_t app = {0};
    app_output_t out = {0};
    pair_db_t initial_pair_db = {0};
    const pair_device_id_t device_id = app_replay_device_id(0x07U);

    pair_db_init(&initial_pair_db);
    if (!pair_db_add(&initial_pair_db, &device_id, 0U)) {
        return false;
    }

    app_init(&app, &initial_pair_db);
    app_replay_tick_with_command(
        &app,
        1000U,
        false,
        APP_OPERATOR_COMMAND_ROTATE_SECURITY_LAST,
        NULL,
        &out
    );

    if (!app_replay_expect_true(
            out.security_rotate_request.valid,
            "operator rotate security should emit security rotate request"
        )) {
        return false;
    }

    if (!app_replay_expect_u32_eq(
            out.security_rotate_request.reason,
            HID_TRANSPORT_SECURITY_ROTATE_REASON_OPERATOR_RECOVERY,
            "operator rotate request should carry operator-recovery reason"
        )) {
        return false;
    }

    return app_replay_expect_true(
        app_replay_device_id_equal(&out.security_rotate_request.device_id, &device_id),
        "operator rotate request should target last paired device"
    );
}

static bool app_replay_test_operator_command_parse_with_token(void) {
    app_operator_command_t command = APP_OPERATOR_COMMAND_NONE;
    operator_command_parse_result_t parse_result = OPERATOR_COMMAND_PARSE_RESULT_INVALID;

    parse_result =
        operator_command_parse_line_result("HIDRELAY LOCKOUT_CLEAR_ALL", "HIDRELAY", &command);
    if (!app_replay_expect_u32_eq(
            (uint32_t)parse_result,
            (uint32_t)OPERATOR_COMMAND_PARSE_RESULT_OK,
            "tokenized operator command parse result"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        (uint32_t)command,
        (uint32_t)APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL,
        "tokenized operator command should parse"
    );
}

static bool app_replay_test_operator_command_parse_reject_without_token(void) {
    app_operator_command_t command = APP_OPERATOR_COMMAND_NONE;
    operator_command_parse_result_t parse_result = OPERATOR_COMMAND_PARSE_RESULT_INVALID;

    parse_result = operator_command_parse_line_result("LOCKOUT_CLEAR_ALL", "HIDRELAY", &command);
    if (!app_replay_expect_u32_eq(
            (uint32_t)parse_result,
            (uint32_t)OPERATOR_COMMAND_PARSE_RESULT_TOKEN_MISMATCH,
            "missing token should return token mismatch"
        )) {
        return false;
    }

    return app_replay_expect_u32_eq(
        (uint32_t)command,
        (uint32_t)APP_OPERATOR_COMMAND_NONE,
        "rejected command should not produce operator action"
    );
}

static bool app_replay_test_operator_command_policy_rate_limit(void) {
    operator_command_policy_t policy = {0};
    operator_command_policy_config_t config = {
        .min_interval_ms = 500U,
        .auth_lockout_ms = 1000U,
        .auth_max_failures = 3U,
    };

    operator_command_policy_init(&policy, &config);

    if (!app_replay_expect_true(
            operator_command_policy_accept(
                &policy,
                OPERATOR_COMMAND_PARSE_RESULT_OK,
                APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL,
                0U
            ),
            "first operator command should pass rate limit"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !operator_command_policy_accept(
                &policy,
                OPERATOR_COMMAND_PARSE_RESULT_OK,
                APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL,
                100U
            ),
            "operator command should be rate-limited inside interval window"
        )) {
        return false;
    }

    return app_replay_expect_true(
        operator_command_policy_accept(
            &policy,
            OPERATOR_COMMAND_PARSE_RESULT_OK,
            APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL,
            500U
        ),
        "operator command should pass once interval window expires"
    );
}

static bool app_replay_test_operator_command_policy_auth_lockout(void) {
    operator_command_policy_t policy = {0};
    operator_command_policy_config_t config = {
        .min_interval_ms = 0U,
        .auth_lockout_ms = 1000U,
        .auth_max_failures = 3U,
    };
    app_operator_command_t command = APP_OPERATOR_COMMAND_NONE;
    operator_command_parse_result_t parse_result = OPERATOR_COMMAND_PARSE_RESULT_INVALID;

    operator_command_policy_init(&policy, &config);

    parse_result =
        operator_command_parse_line_result("BADTOKEN LOCKOUT_CLEAR_ALL", "HIDRELAY", &command);
    if (!app_replay_expect_true(
            parse_result == OPERATOR_COMMAND_PARSE_RESULT_TOKEN_MISMATCH,
            "bad token should be detected for auth lockout test"
        )) {
        return false;
    }

    (void)operator_command_policy_accept(&policy, parse_result, command, 0U);
    (void)operator_command_policy_accept(&policy, parse_result, command, 10U);
    (void)operator_command_policy_accept(&policy, parse_result, command, 20U);

    command = APP_OPERATOR_COMMAND_NONE;
    parse_result =
        operator_command_parse_line_result("HIDRELAY LOCKOUT_CLEAR_ALL", "HIDRELAY", &command);
    if (!app_replay_expect_u32_eq(
            (uint32_t)parse_result,
            (uint32_t)OPERATOR_COMMAND_PARSE_RESULT_OK,
            "valid token command should parse for auth lockout test"
        )) {
        return false;
    }

    if (!app_replay_expect_true(
            !operator_command_policy_accept(&policy, parse_result, command, 50U),
            "valid token command should be blocked during auth lockout window"
        )) {
        return false;
    }

    return app_replay_expect_true(
        operator_command_policy_accept(&policy, parse_result, command, 1500U),
        "valid token command should pass after auth lockout window"
    );
}

int main(void) {
    const app_replay_test_case_t cases[] = {
        {.name = "pair_any_from_long_press", .fn = app_replay_test_pair_any_from_long_press},
        {.name = "remove_last_double_long_press",
            .fn = app_replay_test_remove_last_double_long_press},
        {.name = "reconnect_backoff_schedule", .fn = app_replay_test_reconnect_backoff_schedule},
        {.name = "auth_lockout_recovery", .fn = app_replay_test_auth_lockout_recovery},
        {.name = "threshold_lockout_recovery", .fn = app_replay_test_threshold_lockout_recovery},
        {.name = "bridge_drops_oldest_on_overflow",
            .fn = app_replay_test_bridge_drops_oldest_on_overflow},
        {.name = "remap_boot_keyboard_bt_to_usb",
            .fn = app_replay_test_remap_boot_keyboard_bt_to_usb},
        {.name = "remap_boot_keyboard_usb_to_bt_report_mode",
            .fn = app_replay_test_remap_boot_keyboard_usb_to_bt_report_mode},
        {.name = "remap_boot_mouse_usb_to_bt_boot_mode",
            .fn = app_replay_test_remap_boot_mouse_usb_to_bt_boot_mode},
        {.name = "operator_clear_lockout_last", .fn = app_replay_test_operator_clear_lockout_last},
        {.name = "operator_rotate_security_last",
            .fn = app_replay_test_operator_rotate_security_last},
        {.name = "operator_command_parse_with_token",
            .fn = app_replay_test_operator_command_parse_with_token},
        {.name = "operator_command_parse_reject_without_token",
            .fn = app_replay_test_operator_command_parse_reject_without_token},
        {.name = "operator_command_policy_rate_limit",
            .fn = app_replay_test_operator_command_policy_rate_limit},
        {.name = "operator_command_policy_auth_lockout",
            .fn = app_replay_test_operator_command_policy_auth_lockout},
    };
    const size_t case_count = sizeof(cases) / sizeof(cases[0]);
    size_t index = 0U;
    size_t passed = 0U;

    for (index = 0U; index < case_count; index++) {
        if (!cases[index].fn()) {
            (void)fprintf(stderr, "FAIL: %s\n", cases[index].name);
            return 1;
        }

        (void)fprintf(stdout, "PASS: %s\n", cases[index].name);
        passed++;
    }

    (void)fprintf(
        stdout,
        "PASS: all tests (%lu/%lu)\n",
        (unsigned long)passed,
        (unsigned long)case_count
    );
    return 0;
}
