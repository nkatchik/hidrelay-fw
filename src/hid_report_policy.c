#include "hid_report_policy.h"

#include <stddef.h>

#include "hid_transport.h"

enum {
    HID_REPORT_POLICY_MIN_LEN = 4U,
    HID_REPORT_POLICY_MAX_LEN = 1024U,
    HID_REPORT_POLICY_MAX_COLLECTION_DEPTH = 16U,
    HID_REPORT_POLICY_MAX_GLOBAL_STACK_DEPTH = 8U,
    HID_REPORT_POLICY_MAX_FIELD_BITS = 8192U,
    HID_REPORT_POLICY_MAX_REPORT_ID = 32U,
    HID_USAGE_PAGE_GENERIC_DESKTOP = 0x01U,
    HID_USAGE_GENERIC_DESKTOP_MOUSE = 0x02U,
    HID_USAGE_GENERIC_DESKTOP_KEYBOARD = 0x06U,
};

typedef struct {
    bool parse_ok;
    bool has_application_collection;
    bool has_input_item;
    bool has_report_id;
    uint8_t usage_role;
    uint8_t report_id_count;
    uint8_t reject_reason;
} hid_report_policy_parse_result_t;

static uint32_t hid_report_policy_read_u32(
    const uint8_t * data,
    uint8_t len
) {
    uint32_t value = 0U;
    uint8_t index = 0U;

    if ((data == NULL) || (len == 0U) || (len > 4U)) {
        return 0U;
    }

    for (index = 0U; index < len; index++) {
        value |= ((uint32_t)data[index]) << (index * 8U);
    }

    return value;
}

static uint8_t hid_report_policy_usage_role(
    uint32_t usage_page,
    uint32_t usage
) {
    if (usage_page != HID_USAGE_PAGE_GENERIC_DESKTOP) {
        return HID_REPORT_POLICY_ROLE_UNKNOWN;
    }

    if (usage == HID_USAGE_GENERIC_DESKTOP_KEYBOARD) {
        return HID_REPORT_POLICY_ROLE_KEYBOARD;
    }

    if (usage == HID_USAGE_GENERIC_DESKTOP_MOUSE) {
        return HID_REPORT_POLICY_ROLE_MOUSE;
    }

    return HID_REPORT_POLICY_ROLE_UNKNOWN;
}

static bool hid_report_policy_update_report_field_bounds(
    uint32_t report_size,
    uint32_t report_count
) {
    if ((report_size == 0U) || (report_count == 0U)) {
        return false;
    }

    if ((report_size > HID_REPORT_POLICY_MAX_FIELD_BITS)
        || (report_count > HID_REPORT_POLICY_MAX_FIELD_BITS)
        || (report_count > (HID_REPORT_POLICY_MAX_FIELD_BITS / report_size))) {
        return false;
    }

    return true;
}

static void hid_report_policy_parse(
    const uint8_t * descriptor,
    uint16_t descriptor_len,
    hid_report_policy_parse_result_t * out_result
) {
    bool report_id_seen[256] = {false};
    uint16_t offset = 0U;
    uint8_t collection_depth = 0U;
    uint8_t global_stack_depth = 0U;
    uint32_t report_size = 0U;
    uint32_t report_count = 0U;
    uint32_t usage_page = 0U;
    uint32_t local_usage = 0U;
    bool local_usage_valid = false;

    if (out_result == NULL) {
        return;
    }

    out_result->parse_ok = false;
    out_result->has_application_collection = false;
    out_result->has_input_item = false;
    out_result->has_report_id = false;
    out_result->usage_role = HID_REPORT_POLICY_ROLE_UNKNOWN;
    out_result->report_id_count = 0U;
    out_result->reject_reason = HID_REPORT_POLICY_REJECT_NONE;

    if (descriptor == NULL) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_NULL_DESCRIPTOR;
        return;
    }

    if ((descriptor_len < HID_REPORT_POLICY_MIN_LEN)
        || (descriptor_len > HID_REPORT_POLICY_MAX_LEN)) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_LENGTH;
        return;
    }

    while (offset < descriptor_len) {
        const uint8_t prefix = descriptor[offset++];
        uint8_t data_len = (uint8_t)(prefix & 0x03U);
        const uint8_t item_type = (uint8_t)((prefix >> 2U) & 0x03U);
        const uint8_t item_tag = (uint8_t)((prefix >> 4U) & 0x0FU);
        const uint8_t * data = NULL;

        if (prefix == 0xFEU) {
            out_result->reject_reason = HID_REPORT_POLICY_REJECT_ITEM_FORMAT;
            return;
        }

        if (data_len == 3U) {
            data_len = 4U;
        }

        if ((uint16_t)(offset + data_len) > descriptor_len) {
            out_result->reject_reason = HID_REPORT_POLICY_REJECT_ITEM_FORMAT;
            return;
        }

        data = &descriptor[offset];

        switch (item_type) {
            case 0U:
                if (item_tag == 0x0AU) {
                    if ((data_len != 1U)
                        || (collection_depth >= HID_REPORT_POLICY_MAX_COLLECTION_DEPTH)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_COLLECTION_DEPTH;
                        return;
                    }

                    if ((collection_depth == 0U) && (data[0] == 0x01U)) {
                        const uint8_t usage_role =
                            hid_report_policy_usage_role(usage_page, local_usage);
                        out_result->has_application_collection = true;

                        if ((out_result->usage_role == HID_REPORT_POLICY_ROLE_UNKNOWN)
                            && local_usage_valid) {
                            out_result->usage_role = usage_role;
                        }
                    }

                    collection_depth = (uint8_t)(collection_depth + 1U);
                } else if (item_tag == 0x0CU) {
                    if ((data_len != 0U) || (collection_depth == 0U)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_COLLECTION_DEPTH;
                        return;
                    }

                    collection_depth = (uint8_t)(collection_depth - 1U);
                } else if ((item_tag == 0x08U) || (item_tag == 0x09U) || (item_tag == 0x0BU)) {
                    if (!hid_report_policy_update_report_field_bounds(report_size, report_count)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_FIELD_BOUNDS;
                        return;
                    }

                    if (item_tag == 0x08U) {
                        out_result->has_input_item = true;
                    }
                }

                local_usage = 0U;
                local_usage_valid = false;
                break;
            case 1U:
                if (item_tag == 0x00U) {
                    usage_page = hid_report_policy_read_u32(data, data_len);
                } else if (item_tag == 0x07U) {
                    report_size = hid_report_policy_read_u32(data, data_len);
                } else if (item_tag == 0x08U) {
                    const uint8_t report_id = (data_len == 1U) ? data[0] : 0U;

                    if ((data_len != 1U) || (report_id == 0U)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_REPORT_ID;
                        return;
                    }

                    out_result->has_report_id = true;

                    if (!report_id_seen[report_id]) {
                        report_id_seen[report_id] = true;
                        out_result->report_id_count = (uint8_t)(out_result->report_id_count + 1U);
                    }

                    if (out_result->report_id_count > HID_REPORT_POLICY_MAX_REPORT_ID) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_REPORT_ID;
                        return;
                    }
                } else if (item_tag == 0x09U) {
                    report_count = hid_report_policy_read_u32(data, data_len);
                } else if (item_tag == 0x0AU) {
                    if ((data_len != 0U)
                        || (global_stack_depth >= HID_REPORT_POLICY_MAX_GLOBAL_STACK_DEPTH)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_GLOBAL_STACK;
                        return;
                    }

                    global_stack_depth = (uint8_t)(global_stack_depth + 1U);
                } else if (item_tag == 0x0BU) {
                    if ((data_len != 0U) || (global_stack_depth == 0U)) {
                        out_result->reject_reason = HID_REPORT_POLICY_REJECT_GLOBAL_STACK;
                        return;
                    }

                    global_stack_depth = (uint8_t)(global_stack_depth - 1U);
                }
                break;
            case 2U:
                if ((item_tag == 0x00U) || (item_tag == 0x01U)) {
                    local_usage = hid_report_policy_read_u32(data, data_len);
                    local_usage_valid = true;
                }
                break;
            default:
                out_result->reject_reason = HID_REPORT_POLICY_REJECT_ITEM_FORMAT;
                return;
        }

        offset = (uint16_t)(offset + data_len);
    }

    if (collection_depth != 0U) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_COLLECTION_DEPTH;
        return;
    }

    if (global_stack_depth != 0U) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_GLOBAL_STACK;
        return;
    }

    if (!out_result->has_application_collection) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_MISSING_APPLICATION;
        return;
    }

    if (!out_result->has_input_item) {
        out_result->reject_reason = HID_REPORT_POLICY_REJECT_MISSING_INPUT;
        return;
    }

    out_result->parse_ok = true;
}

static hid_report_descriptor_source_t hid_report_policy_select_source(
    const hid_report_policy_parse_result_t * parse_result,
    uint8_t protocol_mode
) {
    if (parse_result == NULL) {
        return HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_GENERIC;
    }

    if (!parse_result->parse_ok) {
        if (protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT) {
            if (parse_result->usage_role == HID_REPORT_POLICY_ROLE_KEYBOARD) {
                return HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_KEYBOARD;
            }

            if (parse_result->usage_role == HID_REPORT_POLICY_ROLE_MOUSE) {
                return HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_BOOT_MOUSE;
            }
        }

        return HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_GENERIC;
    }

    if ((protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT) && parse_result->has_report_id) {
        return HID_REPORT_DESCRIPTOR_SOURCE_FALLBACK_GENERIC;
    }

    return HID_REPORT_DESCRIPTOR_SOURCE_NATIVE;
}

void hid_report_policy_decide(
    const uint8_t * descriptor,
    uint16_t descriptor_len,
    uint8_t protocol_mode,
    hid_report_policy_decision_t * out_decision
) {
    hid_report_policy_parse_result_t parse_result = {0};

    if (out_decision == NULL) {
        return;
    }

    hid_report_policy_parse(descriptor, descriptor_len, &parse_result);

    out_decision->source = hid_report_policy_select_source(&parse_result, protocol_mode);
    out_decision->native_supported = out_decision->source == HID_REPORT_DESCRIPTOR_SOURCE_NATIVE;
    out_decision->has_report_id = parse_result.has_report_id;
    out_decision->has_application_collection = parse_result.has_application_collection;
    out_decision->has_input_item = parse_result.has_input_item;
    out_decision->usage_role = parse_result.usage_role;
    out_decision->report_id_count = parse_result.report_id_count;
    out_decision->reject_reason = parse_result.reject_reason;

    if ((out_decision->source != HID_REPORT_DESCRIPTOR_SOURCE_NATIVE)
        && (parse_result.reject_reason == HID_REPORT_POLICY_REJECT_NONE)
        && (protocol_mode == HID_TRANSPORT_PROTOCOL_BOOT)
        && parse_result.has_report_id) {
        out_decision->reject_reason = HID_REPORT_POLICY_REJECT_BOOT_WITH_REPORT_ID;
    }
}
