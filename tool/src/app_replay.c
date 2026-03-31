#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "hid_report_remap.h"

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

    app_replay_tick(&app, 10U * 60U * 1000U, false, NULL, &out);
    if (!app_replay_expect_true(
            !out.reconnect_request.valid,
            "auth lockout should still be active"
        )) {
        return false;
    }

    app_replay_tick(&app, (60U * 60U * 1000U) + 200U, false, NULL, &out);
    return app_replay_expect_true(
        out.reconnect_request.valid,
        "auth lockout should recover automatically after cooldown"
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
    event.vendor_id = 0x1234U;
    event.product_id = 0x5678U;
    event.report_descriptor_len = 63U;
    app_replay_tick(&app, 1000U, false, &event, &out);

    event.type = HID_TRANSPORT_EVENT_BT_HID_REPORT;
    event.hid_cid = 0x44U;
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

int main(void) {
    static const app_replay_test_case_t test_cases[] = {
        {.name = "pair_any_from_long_press", .fn = app_replay_test_pair_any_from_long_press},
        {.name = "remove_last_double_long_press_recent_only",
            .fn = app_replay_test_remove_last_double_long_press_recent_only},
        {.name = "remove_last_ignored_when_not_recent",
            .fn = app_replay_test_remove_last_ignored_when_not_recent},
        {.name = "remove_all_very_long_press", .fn = app_replay_test_remove_all_very_long_press},
        {.name = "reconnect_backoff_schedule", .fn = app_replay_test_reconnect_backoff_schedule},
        {.name = "reconnect_auth_failure_lockout_and_recovery",
            .fn = app_replay_test_reconnect_auth_failure_lockout_and_recovery},
        {.name = "bt_report_routed_to_usb", .fn = app_replay_test_bt_report_routed_to_usb},
        {.name = "usb_report_routed_to_bt", .fn = app_replay_test_usb_report_routed_to_bt},
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
