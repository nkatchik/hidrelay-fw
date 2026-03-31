#include "platform_pico_w_tinyusb_runtime.h"

#include <stddef.h>
#include <string.h>

#include "operator_command.h"

#ifdef APP_PICO_HAS_TINYUSB
#include "pico/time.h"
#include "tusb.h"
#endif

#ifndef APP_PICO_OPERATOR_COMMAND_TOKEN
#define APP_PICO_OPERATOR_COMMAND_TOKEN "HIDRELAY"
#endif

#if defined(APP_PICO_HAS_TINYUSB) && defined(APP_PICO_HAS_DIAG_CDC)
enum {
    PICO_W_OPERATOR_COMMAND_QUEUE_SIZE = 8U,
    PICO_W_OPERATOR_COMMAND_LINE_MAX = 63U,
    PICO_W_OPERATOR_COMMAND_MIN_INTERVAL_MS = 500U,
};

static app_operator_command_t g_operator_command_queue[PICO_W_OPERATOR_COMMAND_QUEUE_SIZE] = {
    APP_OPERATOR_COMMAND_NONE
};
static uint8_t g_operator_command_queue_head = 0U;
static uint8_t g_operator_command_queue_tail = 0U;
static uint8_t g_operator_command_queue_count = 0U;
static char g_operator_command_line[PICO_W_OPERATOR_COMMAND_LINE_MAX + 1U] = {0};
static uint8_t g_operator_command_line_len = 0U;
static uint32_t g_operator_command_last_accept_ms = 0U;
static bool g_operator_command_has_accept = false;

static void pico_w_tinyusb_runtime_operator_reset(void) {
    (void)memset(g_operator_command_queue, 0, sizeof(g_operator_command_queue));
    g_operator_command_queue_head = 0U;
    g_operator_command_queue_tail = 0U;
    g_operator_command_queue_count = 0U;
    (void)memset(g_operator_command_line, 0, sizeof(g_operator_command_line));
    g_operator_command_line_len = 0U;
    g_operator_command_last_accept_ms = 0U;
    g_operator_command_has_accept = false;
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
    app_operator_command_t command = APP_OPERATOR_COMMAND_NONE;
    uint32_t now_ms = 0U;

    g_operator_command_line[g_operator_command_line_len] = '\0';
    if (!operator_command_parse_line(
            g_operator_command_line,
            APP_PICO_OPERATOR_COMMAND_TOKEN,
            &command
        )) {
        command = APP_OPERATOR_COMMAND_NONE;
    }

    if (command != APP_OPERATOR_COMMAND_NONE) {
        now_ms = to_ms_since_boot(get_absolute_time());

        if (!g_operator_command_has_accept
            || ((uint32_t)(now_ms - g_operator_command_last_accept_ms)
                >= PICO_W_OPERATOR_COMMAND_MIN_INTERVAL_MS)) {
            if (pico_w_tinyusb_runtime_operator_push(command)) {
                g_operator_command_last_accept_ms = now_ms;
                g_operator_command_has_accept = true;
            }
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
