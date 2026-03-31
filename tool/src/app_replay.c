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
        "pair-any should activate after long-press release window"
    );
}

static bool app_replay_test_pair_any_timeout_after_60s(void) {
    app_t app = {0};
    app_output_t out = {0};

    app_init(&app, NULL);

    app_replay_tick(&app, 1000U, true, NULL, &out);
    app_replay_tick(&app, 3100U, false, NULL, &out);
    app_replay_tick(&app, 5601U, false, NULL, &out);

    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should be active before timeout countdown"
        )) {
        return false;
    }

    app_replay_tick(&app, 65600U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.pairing_active,
            "pair-any should stay active until full 60s elapsed"
        )) {
        return false;
    }

    app_replay_tick(&app, 65601U, false, NULL, &out);
    return app_replay_expect_true(!out.pairing_active, "pair-any should stop after 60s timeout");
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
    app_replay_tick(&app, (60U * 60U * 1000U) + 7100U, false, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 7500U, true, NULL, &out);
    app_replay_tick(&app, (60U * 60U * 1000U) + 9550U, false, NULL, &out);

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
            "factory reset should be delayed for LED sequence"
        )) {
        return false;
    }

    app_replay_tick(&app, 13610U, false, NULL, &out);
    return app_replay_expect_true(
        out.factory_reset_requested,
        "factory reset should trigger after delay"
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

    app_replay_tick(&app, 5100U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "reconnect should retry after first backoff window"
    );
}

static bool app_replay_test_reconnect_auth_failure_lockout_and_recovery(void) {
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

    app_replay_tick(&app, 5100U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "first auth failure should back off, not lock out"
        )) {
        return false;
    }

    app_replay_tick(&app, 5200U, false, &event, &out);
    app_replay_tick(&app, 15200U, false, NULL, &out);
    if (!app_replay_expect_true(
            out.reconnect_request.valid,
            "second auth failure should still retry after backoff"
        )) {
        return false;
    }

    app_replay_tick(&app, 15300U, false, &event, &out);
    app_replay_tick(&app, 10U * 60U * 1000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "third auth failure should enter lockout window"
        )) {
        return false;
    }

    app_replay_tick(&app, (60U * 60U * 1000U) + 16000U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "auth lockout should recover automatically after cooldown"
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
        {.name = "pair_any_from_long_press", .fn = app_replay_test_pair_any_from_long_press},
        {.name = "pair_any_timeout_after_60s", .fn = app_replay_test_pair_any_timeout_after_60s},
        {.name = "remove_last_double_long_press_recent_only",
            .fn = app_replay_test_remove_last_double_long_press_recent_only},
        {.name = "remove_last_ignored_when_not_recent",
            .fn = app_replay_test_remove_last_ignored_when_not_recent},
        {.name = "remove_all_very_long_press", .fn = app_replay_test_remove_all_very_long_press},
        {.name = "reconnect_backoff_schedule", .fn = app_replay_test_reconnect_backoff_schedule},
        {.name = "reconnect_auth_failure_lockout_and_recovery",
            .fn = app_replay_test_reconnect_auth_failure_lockout_and_recovery},
        {.name = "reconnect_request_uses_last_link_hint",
            .fn = app_replay_test_reconnect_request_uses_last_link_hint},
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
