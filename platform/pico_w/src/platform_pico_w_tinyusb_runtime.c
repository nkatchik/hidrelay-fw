#include "platform_pico_w_tinyusb_runtime.h"

#include <stddef.h>
#include <string.h>

#include "operator_auth.h"
#include "operator_command.h"

#ifdef APP_PICO_HAS_TINYUSB
#include "pico/time.h"
#include "tusb.h"
#endif

#ifndef APP_PICO_OPERATOR_AUTH_KEY_HEX
#define APP_PICO_OPERATOR_AUTH_KEY_HEX ""
#endif

#ifndef APP_PICO_OPERATOR_AUTH_SESSION_TTL_MS
#define APP_PICO_OPERATOR_AUTH_SESSION_TTL_MS 60000U
#endif

#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
enum {
    PICO_W_OPERATOR_COMMAND_QUEUE_SIZE = 8U,
    PICO_W_OPERATOR_COMMAND_LINE_MAX = 191U,
    PICO_W_OPERATOR_COMMAND_MIN_INTERVAL_MS = 500U,
    PICO_W_OPERATOR_AUTH_LOCKOUT_MS = 30000U,
    PICO_W_OPERATOR_AUTH_MAX_FAILURES = 5U,
};

static app_operator_command_t g_operator_command_queue[PICO_W_OPERATOR_COMMAND_QUEUE_SIZE] = {
    APP_OPERATOR_COMMAND_NONE
};
static uint8_t g_operator_command_queue_head = 0U;
static uint8_t g_operator_command_queue_tail = 0U;
static uint8_t g_operator_command_queue_count = 0U;
static char g_operator_command_line[PICO_W_OPERATOR_COMMAND_LINE_MAX + 1U] = {0};
static uint8_t g_operator_command_line_len = 0U;
static operator_command_policy_t g_operator_command_policy = {0};
static operator_auth_state_t g_operator_auth_state = {0};
static bool g_operator_auth_ready = false;
static uint64_t g_operator_auth_entropy_counter = 0U;

static uint64_t pico_w_tinyusb_runtime_operator_entropy(uint32_t now_ms) {
    uint64_t mixed = 0U;

    g_operator_auth_entropy_counter = g_operator_auth_entropy_counter + 1U;
    mixed = ((uint64_t)now_ms << 32U) ^ g_operator_auth_entropy_counter;
    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    mixed *= 0xc4ceb9fe1a85ec53ULL;
    mixed ^= mixed >> 33U;
    return mixed;
}

static void pico_w_tinyusb_runtime_operator_write_line(const char * line) {
    uint32_t available = 0U;
    size_t line_len = 0U;
    uint32_t written = 0U;

    if ((line == NULL) || (line[0] == '\0')) {
        return;
    }

    if (!tud_ready() || !tud_cdc_n_connected(0U)) {
        return;
    }

    available = tud_cdc_n_write_available(0U);
    line_len = strlen(line);
    if (available < (line_len + 1U)) {
        return;
    }

    written = tud_cdc_n_write(0U, line, line_len);
    if (written == line_len) {
        (void)tud_cdc_n_write(0U, "\n", 1U);
    }
    (void)tud_cdc_n_write_flush(0U);
}

static void pico_w_tinyusb_runtime_operator_reset(void) {
    const operator_command_policy_config_t policy_config = {
        .min_interval_ms = PICO_W_OPERATOR_COMMAND_MIN_INTERVAL_MS,
        .auth_lockout_ms = 0U,
        .auth_max_failures = 0U,
    };
    const operator_auth_config_t auth_config = {
        .session_ttl_ms = APP_PICO_OPERATOR_AUTH_SESSION_TTL_MS,
        .lockout_ms = PICO_W_OPERATOR_AUTH_LOCKOUT_MS,
        .max_auth_failures = PICO_W_OPERATOR_AUTH_MAX_FAILURES,
    };

    (void)memset(g_operator_command_queue, 0, sizeof(g_operator_command_queue));
    g_operator_command_queue_head = 0U;
    g_operator_command_queue_tail = 0U;
    g_operator_command_queue_count = 0U;
    (void)memset(g_operator_command_line, 0, sizeof(g_operator_command_line));
    g_operator_command_line_len = 0U;
    operator_command_policy_init(&g_operator_command_policy, &policy_config);
    g_operator_auth_entropy_counter = 0U;
    g_operator_auth_ready = operator_auth_state_init(
        &g_operator_auth_state,
        &auth_config,
        APP_PICO_OPERATOR_AUTH_KEY_HEX
    );
}

static bool pico_w_tinyusb_runtime_operator_push(app_operator_command_t command) {
    if (command == APP_OPERATOR_COMMAND_NONE) {
        return false;
    }

    if (g_operator_command_queue_count >= PICO_W_OPERATOR_COMMAND_QUEUE_SIZE) {
        g_operator_command_queue_head =
            (uint8_t)((g_operator_command_queue_head + 1U) % PICO_W_OPERATOR_COMMAND_QUEUE_SIZE);
        g_operator_command_queue_count = (uint8_t)(g_operator_command_queue_count - 1U);
    }

    g_operator_command_queue[g_operator_command_queue_tail] = command;
    g_operator_command_queue_tail =
        (uint8_t)((g_operator_command_queue_tail + 1U) % PICO_W_OPERATOR_COMMAND_QUEUE_SIZE);
    g_operator_command_queue_count = (uint8_t)(g_operator_command_queue_count + 1U);
    return true;
}

static void pico_w_tinyusb_runtime_operator_commit_line(void) {
    operator_auth_output_t auth_output = {0};
    uint32_t now_ms = 0U;
    uint64_t entropy = 0U;

    g_operator_command_line[g_operator_command_line_len] = '\0';

    now_ms = to_ms_since_boot(get_absolute_time());
    entropy = pico_w_tinyusb_runtime_operator_entropy(now_ms);
    if (!g_operator_auth_ready) {
        pico_w_tinyusb_runtime_operator_write_line("ERR AUTH_CONFIG");
    } else if (operator_auth_process_line(
                   &g_operator_auth_state,
                   g_operator_command_line,
                   now_ms,
                   entropy,
                   &auth_output
               )) {
        if (auth_output.has_response) {
            pico_w_tinyusb_runtime_operator_write_line(auth_output.response);
        }

        if (auth_output.has_command
            && operator_command_policy_accept(
                &g_operator_command_policy,
                OPERATOR_COMMAND_PARSE_RESULT_OK,
                auth_output.command,
                now_ms
            )) {
            (void)pico_w_tinyusb_runtime_operator_push(auth_output.command);
        } else if (auth_output.has_command) {
            pico_w_tinyusb_runtime_operator_write_line("CMD FAIL RATE_LIMIT");
        }
    }

    (void)memset(g_operator_command_line, 0, sizeof(g_operator_command_line));
    g_operator_command_line_len = 0U;
}
#endif

bool pico_w_tinyusb_runtime_init(void) {
#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
    pico_w_tinyusb_runtime_operator_reset();
#endif
#ifdef APP_PICO_HAS_TINYUSB
    return tusb_init();
#else
    return true;
#endif
}

void pico_w_tinyusb_runtime_poll(void) {
#ifdef APP_PICO_HAS_TINYUSB
    tud_task();
#endif
}

bool pico_w_tinyusb_runtime_send_in_report(
    uint8_t interface_number,
    const uint8_t * report,
    uint16_t report_len
) {
#ifdef APP_PICO_HAS_TINYUSB
    if ((report_len > 0U) && (report == NULL)) {
        return false;
    }

    if (report_len > UINT8_MAX) {
        return false;
    }

    if (!tud_hid_n_ready(interface_number)) {
        return false;
    }

    return tud_hid_n_report(interface_number, 0U, report, report_len);
#else
    (void)interface_number;
    (void)report;
    (void)report_len;
    return false;
#endif
}

bool pico_w_tinyusb_runtime_diag_write(
    const uint8_t * data,
    uint16_t data_len
) {
#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
    uint16_t remaining = data_len;
    const uint8_t * cursor = data;

    if ((data_len > 0U) && (data == NULL)) {
        return false;
    }

    if (data_len == 0U) {
        return true;
    }

    if (!tud_ready() || !tud_cdc_n_connected(0U)) {
        return false;
    }

    while (remaining > 0U) {
        uint32_t available = tud_cdc_n_write_available(0U);
        uint32_t chunk = 0U;
        uint32_t written = 0U;

        if (available == 0U) {
            (void)tud_cdc_n_write_flush(0U);
            return false;
        }

        chunk = (remaining < available) ? remaining : available;
        written = tud_cdc_n_write(0U, cursor, chunk);

        if (written == 0U) {
            return false;
        }

        cursor += written;
        remaining = (uint16_t)(remaining - written);
    }

    (void)tud_cdc_n_write_flush(0U);
    return true;
#else
    (void)data;
    (void)data_len;
    return false;
#endif
}

bool pico_w_tinyusb_runtime_take_operator_command(app_operator_command_t * out_command) {
#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
    if (out_command == NULL) {
        return false;
    }

    *out_command = APP_OPERATOR_COMMAND_NONE;

    if (g_operator_command_queue_count == 0U) {
        return false;
    }

    *out_command = g_operator_command_queue[g_operator_command_queue_head];
    g_operator_command_queue[g_operator_command_queue_head] = APP_OPERATOR_COMMAND_NONE;
    g_operator_command_queue_head =
        (uint8_t)((g_operator_command_queue_head + 1U) % PICO_W_OPERATOR_COMMAND_QUEUE_SIZE);
    g_operator_command_queue_count = (uint8_t)(g_operator_command_queue_count - 1U);
    return true;
#else
    (void)out_command;
    return false;
#endif
}

#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
void tud_cdc_rx_cb(uint8_t itf) {
    if (itf != 0U) {
        while (tud_cdc_n_available(itf) > 0U) {
            (void)tud_cdc_n_read_char(itf);
        }
        return;
    }

    while (tud_cdc_n_available(itf) > 0U) {
        const int read_char = tud_cdc_n_read_char(itf);

        if (read_char < 0) {
            continue;
        }

        if ((read_char == '\n') || (read_char == '\r')) {
            if (g_operator_command_line_len > 0U) {
                pico_w_tinyusb_runtime_operator_commit_line();
            }

            continue;
        }

        if (g_operator_command_line_len >= PICO_W_OPERATOR_COMMAND_LINE_MAX) {
            g_operator_command_line_len = 0U;
            (void)memset(g_operator_command_line, 0, sizeof(g_operator_command_line));
            continue;
        }

        g_operator_command_line[g_operator_command_line_len++] = (char)read_char;
    }
}
#endif
