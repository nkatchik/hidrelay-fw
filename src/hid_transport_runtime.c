#include "hid_transport_runtime.h"

#include <stddef.h>
#include <string.h>

#include "hid_report_policy.h"
#include "hid_report_remap.h"

static bool hid_transport_runtime_event_is_report_type(uint8_t event_type) {
    return (event_type == HID_TRANSPORT_EVENT_BT_HID_REPORT)
        || (event_type == HID_TRANSPORT_EVENT_USB_HID_REPORT);
}

static bool hid_transport_runtime_device_id_equal(
    const pair_device_id_t * lhs,
    const pair_device_id_t * rhs
) {
    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    return memcmp(lhs->bytes, rhs->bytes, sizeof(lhs->bytes)) == 0;
}

static bool hid_transport_runtime_drop_oldest_report_event(hid_transport_runtime_t * runtime) {
    hid_transport_event_t ordered[HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE] = {0};
    uint8_t ordered_count = 0U;
    uint8_t drop_index = 0U;
    bool found_report = false;
    uint8_t index = 0U;

    if ((runtime == NULL) || (runtime->event_queue_count == 0U)) {
        return false;
    }

    for (index = 0U; index < runtime->event_queue_count; index++) {
        ordered[index] =
            runtime->event_queue
                [(runtime->event_queue_head + index) % HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE];
        ordered_count = (uint8_t)(ordered_count + 1U);
    }

    for (index = 0U; index < ordered_count; index++) {
        if (!hid_transport_runtime_event_is_report_type(ordered[index].type)) {
            continue;
        }

        drop_index = index;
        found_report = true;
        break;
    }

    if (!found_report) {
        return false;
    }

    (void)memset(runtime->event_queue, 0, sizeof(runtime->event_queue));
    runtime->event_queue_head = 0U;
    runtime->event_queue_tail = 0U;
    runtime->event_queue_count = 0U;

    for (index = 0U; index < ordered_count; index++) {
        if (index == drop_index) {
            continue;
        }

        runtime->event_queue[runtime->event_queue_tail] = ordered[index];
        runtime->event_queue_tail =
            (uint8_t)((runtime->event_queue_tail + 1U) % HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE);
        runtime->event_queue_count = (uint8_t)(runtime->event_queue_count + 1U);
    }

    return true;
}

static uint8_t hid_transport_runtime_report_remap_profile(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    hid_transport_runtime_descriptor_fn_t descriptor_fn,
    void * descriptor_context
) {
    const uint8_t * descriptor = NULL;
    uint16_t descriptor_len = 0U;
    const uint8_t protocol_mode =
        hid_transport_runtime_usb_protocol_mode(runtime, interface_number);
    hid_report_policy_decision_t decision = {0};

    if (descriptor_fn != NULL) {
        descriptor = descriptor_fn(interface_number, &descriptor_len, descriptor_context);
    }

    hid_report_policy_decide(descriptor, descriptor_len, protocol_mode, &decision);
    return hid_report_remap_profile_from_policy(&decision);
}

void hid_transport_runtime_init(hid_transport_runtime_t * runtime) {
    if (runtime == NULL) {
        return;
    }

    (void)memset(runtime, 0, sizeof(*runtime));
    runtime->usb_interface_count = 1U;
    runtime->usb_descriptor_generation = 1U;
}

bool hid_transport_runtime_set_usb_plan(
    hid_transport_runtime_t * runtime,
    uint8_t interface_count,
    uint32_t descriptor_generation,
    const hid_transport_usb_interface_plan_t * interface_plan,
    bool * out_descriptor_changed
) {
    hid_transport_usb_interface_plan_t next_plan[HID_TRANSPORT_MAX_INTERFACE] = {0};
    size_t copy_len = 0U;
    bool descriptor_changed = false;
    bool plan_changed = false;

    if (out_descriptor_changed != NULL) {
        *out_descriptor_changed = false;
    }

    if (runtime == NULL) {
        return false;
    }

    if (interface_count > HID_TRANSPORT_MAX_INTERFACE) {
        interface_count = HID_TRANSPORT_MAX_INTERFACE;
    }

    copy_len = (size_t)interface_count * sizeof(next_plan[0]);
    if ((interface_plan != NULL) && (copy_len > 0U)) {
        (void)memcpy(next_plan, interface_plan, copy_len);
    }

    descriptor_changed = descriptor_generation != runtime->usb_descriptor_generation;
    plan_changed = (interface_count != runtime->usb_interface_count)
        || (memcmp(next_plan, runtime->usb_interface_plan, sizeof(next_plan)) != 0);

    if (!descriptor_changed && !plan_changed) {
        return false;
    }

    runtime->usb_interface_count = interface_count;
    (void)memcpy(runtime->usb_interface_plan, next_plan, sizeof(runtime->usb_interface_plan));

    if (descriptor_changed) {
        runtime->usb_descriptor_generation = descriptor_generation;
        if (out_descriptor_changed != NULL) {
            *out_descriptor_changed = true;
        }
    }

    return true;
}

uint8_t hid_transport_runtime_usb_interface_count(const hid_transport_runtime_t * runtime) {
    if (runtime == NULL) {
        return 0U;
    }

    return runtime->usb_interface_count;
}

bool hid_transport_runtime_usb_interface_plan_get(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    hid_transport_usb_interface_plan_t * out_plan
) {
    if ((runtime == NULL)
        || (out_plan == NULL)
        || (interface_number >= runtime->usb_interface_count)) {
        return false;
    }

    *out_plan = runtime->usb_interface_plan[interface_number];
    return true;
}

uint8_t hid_transport_runtime_usb_protocol_mode(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number
) {
    if ((runtime == NULL) || (interface_number >= runtime->usb_interface_count)) {
        return HID_TRANSPORT_PROTOCOL_UNKNOWN;
    }

    return runtime->usb_interface_plan[interface_number].protocol_mode;
}

bool hid_transport_runtime_find_hid_cid_for_device(
    const hid_transport_runtime_t * runtime,
    const pair_device_id_t * device_id,
    uint16_t * out_hid_cid,
    uint8_t * out_bt_link_type
) {
    uint8_t index = 0U;

    if ((runtime == NULL)
        || (device_id == NULL)
        || (out_hid_cid == NULL)
        || (out_bt_link_type == NULL)) {
        return false;
    }

    for (index = 0U; index < runtime->usb_interface_count; index++) {
        if (!hid_transport_runtime_device_id_equal(
                &runtime->usb_interface_plan[index].device_id,
                device_id
            )
            || (runtime->usb_interface_plan[index].hid_cid == 0U)) {
            continue;
        }

        *out_hid_cid = runtime->usb_interface_plan[index].hid_cid;
        *out_bt_link_type = runtime->usb_interface_plan[index].bt_link_type;
        return true;
    }

    return false;
}

bool hid_transport_runtime_push_event(
    hid_transport_runtime_t * runtime,
    const hid_transport_event_t * event
) {
    if ((runtime == NULL) || (event == NULL) || (event->type == HID_TRANSPORT_EVENT_NONE)) {
        return false;
    }

    if (runtime->event_queue_count >= HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE) {
        if (!hid_transport_runtime_event_is_report_type(event->type)
            && hid_transport_runtime_drop_oldest_report_event(runtime)) {
            /* queue space was reclaimed */
        } else {
            runtime->event_queue_dropped = runtime->event_queue_dropped + 1U;
            return false;
        }
    }

    if (runtime->event_queue_count >= HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE) {
        runtime->event_queue_dropped = runtime->event_queue_dropped + 1U;
        return false;
    }

    runtime->event_queue[runtime->event_queue_tail] = *event;
    runtime->event_queue_tail =
        (uint8_t)((runtime->event_queue_tail + 1U) % HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE);
    runtime->event_queue_count = (uint8_t)(runtime->event_queue_count + 1U);

    if (runtime->event_queue_count > runtime->event_queue_high_watermark) {
        runtime->event_queue_high_watermark = runtime->event_queue_count;
    }

    return true;
}

bool hid_transport_runtime_take_event(
    hid_transport_runtime_t * runtime,
    hid_transport_event_t * out_event
) {
    if (out_event == NULL) {
        return false;
    }

    (void)memset(out_event, 0, sizeof(*out_event));

    if ((runtime == NULL) || (runtime->event_queue_count == 0U)) {
        return false;
    }

    *out_event = runtime->event_queue[runtime->event_queue_head];
    (void)memset(
        &runtime->event_queue[runtime->event_queue_head],
        0,
        sizeof(runtime->event_queue[runtime->event_queue_head])
    );
    runtime->event_queue_head =
        (uint8_t)((runtime->event_queue_head + 1U) % HID_TRANSPORT_RUNTIME_EVENT_QUEUE_SIZE);
    runtime->event_queue_count = (uint8_t)(runtime->event_queue_count - 1U);
    return true;
}

bool hid_transport_runtime_ingest_usb_report(
    hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len,
    hid_transport_runtime_descriptor_fn_t descriptor_fn,
    void * descriptor_context
) {
    hid_transport_event_t event = {0};
    uint8_t remapped_report[HID_TRANSPORT_REPORT_MAX_LEN] = {0};
    uint16_t remapped_report_len = 0U;
    const uint8_t remap_profile = hid_transport_runtime_report_remap_profile(
        runtime,
        interface_number,
        descriptor_fn,
        descriptor_context
    );
    const uint8_t protocol_mode =
        hid_transport_runtime_usb_protocol_mode(runtime, interface_number);

    if (!hid_report_remap_usb_to_bt(
            remap_profile,
            protocol_mode,
            report,
            report_len,
            remapped_report,
            &remapped_report_len
        )) {
        return false;
    }

    event.type = HID_TRANSPORT_EVENT_USB_HID_REPORT;
    event.interface_number = interface_number;
    event.report_len = remapped_report_len;

    if (remapped_report_len > 0U) {
        (void)memcpy(event.report, remapped_report, remapped_report_len);
    }

    return hid_transport_runtime_push_event(runtime, &event);
}

bool hid_transport_runtime_remap_bt_to_usb(
    const hid_transport_runtime_t * runtime,
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len,
    hid_transport_runtime_descriptor_fn_t descriptor_fn,
    void * descriptor_context,
    uint8_t * out_report,
    uint16_t * out_report_len
) {
    const uint8_t remap_profile = hid_transport_runtime_report_remap_profile(
        runtime,
        interface_number,
        descriptor_fn,
        descriptor_context
    );

    return hid_report_remap_bt_to_usb(
        remap_profile,
        report,
        report_len,
        out_report,
        out_report_len
    );
}

bool hid_transport_runtime_queue_state_get(
    const hid_transport_runtime_t * runtime,
    hid_transport_runtime_queue_state_t * out_state
) {
    if ((runtime == NULL) || (out_state == NULL)) {
        return false;
    }

    out_state->event_queue_depth = runtime->event_queue_count;
    out_state->event_queue_high_watermark = runtime->event_queue_high_watermark;
    out_state->event_queue_dropped = runtime->event_queue_dropped;
    return true;
}
