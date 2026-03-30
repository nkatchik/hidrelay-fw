#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "util/cleanup.h"

enum {
    DIAG_FRAME_MAGIC_0 = 0x48U,
    DIAG_FRAME_MAGIC_1 = 0x52U,
    DIAG_FRAME_VERSION = 1U,
    DIAG_FRAME_PAYLOAD_LEN = 39U,
    DIAG_FRAME_LEN = 4U + DIAG_FRAME_PAYLOAD_LEN,
    DIAG_DEFAULT_BAUD = 115200U,
};

typedef struct {
    uint32_t sequence;
    uint8_t bt_state;
    uint8_t active_device_count;
    uint8_t usb_interface_count;
    uint8_t usb_tx_depth;
    uint8_t bt_tx_depth;
    uint8_t usb_tx_high_watermark;
    uint8_t bt_tx_high_watermark;
    uint8_t reconnect_last_result;
    uint8_t reconnect_last_status_code;
    uint32_t usb_tx_dropped;
    uint32_t bt_tx_dropped;
    uint32_t reconnect_attempt_count;
    uint32_t reconnect_success_count;
    uint32_t reconnect_failure_count;
    uint8_t stack_event_depth;
    uint8_t stack_event_high_watermark;
    uint32_t stack_event_dropped;
} diag_frame_t;

typedef struct {
    uint8_t frame[DIAG_FRAME_LEN];
    size_t used;
} diag_parser_t;

typedef struct {
    int fd;
    struct termios original_tty;
    bool restore_tty;
} diag_serial_guard_t;

static void diag_serial_guard_cleanup(diag_serial_guard_t * guard) {
    if ((guard == NULL) || !guard->restore_tty || (guard->fd < 0)) {
        return;
    }

    (void)tcsetattr(guard->fd, TCSANOW, &guard->original_tty);
    guard->restore_tty = false;
}

static void diag_print_usage(const char * program_name) {
    if (program_name == NULL) {
        program_name = "diag_capture";
    }

    (void)fprintf(
        stderr,
        "Usage: %s --device <tty> [--baud <rate>] [--count <n>] [--output <csv>]\n",
        program_name
    );
}

static bool diag_parse_u32(
    const char * text,
    uint32_t * out_value
) {
    char * end = NULL;
    unsigned long value = 0UL;

    if ((text == NULL) || (out_value == NULL)) {
        return false;
    }

    errno = 0;
    value = strtoul(text, &end, 10);

    if ((errno != 0) || (end == text) || (*end != '\0') || (value > UINT32_MAX)) {
        return false;
    }

    *out_value = (uint32_t)value;
    return true;
}

static speed_t diag_baud_to_speed(uint32_t baud) {
    switch (baud) {
        case 9600U:
            return B9600;
        case 19200U:
            return B19200;
        case 38400U:
            return B38400;
        case 57600U:
            return B57600;
        case 115200U:
            return B115200;
        case 230400U:
            return B230400;
        default:
            return 0;
    }
}

static uint32_t diag_read_u32le(const uint8_t * data) {
    if (data == NULL) {
        return 0U;
    }

    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8U)
        | ((uint32_t)data[2] << 16U)
        | ((uint32_t)data[3] << 24U);
}

static bool diag_decode_frame(
    const uint8_t * frame,
    diag_frame_t * out_frame
) {
    uint16_t offset = 4U;

    if ((frame == NULL) || (out_frame == NULL)) {
        return false;
    }

    if ((frame[0] != DIAG_FRAME_MAGIC_0)
        || (frame[1] != DIAG_FRAME_MAGIC_1)
        || (frame[2] != DIAG_FRAME_VERSION)
        || (frame[3] != DIAG_FRAME_PAYLOAD_LEN)) {
        return false;
    }

    out_frame->sequence = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->bt_state = frame[offset++];
    out_frame->active_device_count = frame[offset++];
    out_frame->usb_interface_count = frame[offset++];
    out_frame->usb_tx_depth = frame[offset++];
    out_frame->bt_tx_depth = frame[offset++];
    out_frame->usb_tx_high_watermark = frame[offset++];
    out_frame->bt_tx_high_watermark = frame[offset++];
    out_frame->reconnect_last_result = frame[offset++];
    out_frame->reconnect_last_status_code = frame[offset++];
    out_frame->usb_tx_dropped = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->bt_tx_dropped = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->reconnect_attempt_count = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->reconnect_success_count = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->reconnect_failure_count = diag_read_u32le(&frame[offset]);
    offset = (uint16_t)(offset + 4U);
    out_frame->stack_event_depth = frame[offset++];
    out_frame->stack_event_high_watermark = frame[offset++];
    out_frame->stack_event_dropped = diag_read_u32le(&frame[offset]);

    return true;
}

static bool diag_parser_ingest(
    diag_parser_t * parser,
    uint8_t byte,
    diag_frame_t * out_frame
) {
    if ((parser == NULL) || (out_frame == NULL)) {
        return false;
    }

    if (parser->used == 0U) {
        if (byte == DIAG_FRAME_MAGIC_0) {
            parser->frame[0] = byte;
            parser->used = 1U;
        }

        return false;
    }

    if (parser->used == 1U) {
        if (byte == DIAG_FRAME_MAGIC_1) {
            parser->frame[1] = byte;
            parser->used = 2U;
        } else if (byte == DIAG_FRAME_MAGIC_0) {
            parser->frame[0] = byte;
            parser->used = 1U;
        } else {
            parser->used = 0U;
        }

        return false;
    }

    parser->frame[parser->used++] = byte;

    if (parser->used == 4U) {
        if ((parser->frame[2] != DIAG_FRAME_VERSION)
            || (parser->frame[3] != DIAG_FRAME_PAYLOAD_LEN)) {
            parser->used = (byte == DIAG_FRAME_MAGIC_0) ? 1U : 0U;
            parser->frame[0] = byte;
        }

        return false;
    }

    if (parser->used < DIAG_FRAME_LEN) {
        return false;
    }

    parser->used = 0U;
    return diag_decode_frame(parser->frame, out_frame);
}

static uint64_t diag_monotonic_ms(void) {
    struct timespec ts = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0U;
    }

    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static void diag_write_csv_header(FILE * stream) {
    if (stream == NULL) {
        return;
    }

    (void)fprintf(
        stream,
        "host_ms,sequence,bt_state,active_device_count,usb_interface_count,"
        "usb_tx_depth,bt_tx_depth,usb_tx_high_watermark,bt_tx_high_watermark,"
        "reconnect_last_result,reconnect_last_status_code,usb_tx_dropped,bt_tx_dropped,"
        "reconnect_attempt_count,reconnect_success_count,reconnect_failure_count,"
        "stack_event_depth,stack_event_high_watermark,stack_event_dropped\n"
    );
}

static void diag_write_csv_row(
    FILE * stream,
    uint64_t host_ms,
    const diag_frame_t * frame
) {
    if ((stream == NULL) || (frame == NULL)) {
        return;
    }

    (void)fprintf(
        stream,
        "%llu,%lu,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lu,%lu,%lu,%lu,%lu,%u,%u,%lu\n",
        (unsigned long long)host_ms,
        (unsigned long)frame->sequence,
        frame->bt_state,
        frame->active_device_count,
        frame->usb_interface_count,
        frame->usb_tx_depth,
        frame->bt_tx_depth,
        frame->usb_tx_high_watermark,
        frame->bt_tx_high_watermark,
        frame->reconnect_last_result,
        frame->reconnect_last_status_code,
        (unsigned long)frame->usb_tx_dropped,
        (unsigned long)frame->bt_tx_dropped,
        (unsigned long)frame->reconnect_attempt_count,
        (unsigned long)frame->reconnect_success_count,
        (unsigned long)frame->reconnect_failure_count,
        frame->stack_event_depth,
        frame->stack_event_high_watermark,
        (unsigned long)frame->stack_event_dropped
    );
    (void)fflush(stream);
}

static bool diag_configure_serial(
    int fd,
    speed_t speed,
    struct termios * out_original_tty
) {
    struct termios original_tty = {0};
    struct termios config = {0};

    if ((fd < 0) || (speed == 0) || (out_original_tty == NULL)) {
        return false;
    }

    if (tcgetattr(fd, &original_tty) != 0) {
        return false;
    }

    config = original_tty;
    config.c_iflag &=
        (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    config.c_oflag &= (tcflag_t)~OPOST;
    config.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    config.c_cflag &= (tcflag_t) ~(CSIZE | PARENB);
    config.c_cflag |= (tcflag_t)(CS8 | CLOCAL | CREAD);
    config.c_cc[VMIN] = 1;
    config.c_cc[VTIME] = 0;

    if ((cfsetispeed(&config, speed) != 0) || (cfsetospeed(&config, speed) != 0)) {
        return false;
    }

    if (tcsetattr(fd, TCSANOW, &config) != 0) {
        return false;
    }

    *out_original_tty = original_tty;
    return true;
}

int main(
    int argc,
    char ** argv
) {
    const char * device_path = NULL;
    const char * output_path = NULL;
    uint32_t baud = DIAG_DEFAULT_BAUD;
    uint32_t frame_limit = 0U;
    speed_t speed = 0;
    UTIL_SCOPED_FD int serial_fd = -1;
    UTIL_CLEANUP(diag_serial_guard_cleanup)
    diag_serial_guard_t serial_guard = {
        .fd = -1,
        .original_tty = {0},
        .restore_tty = false,
    };
    UTIL_SCOPED_FILE FILE * output_file = NULL;
    FILE * output_stream = stdout;
    diag_parser_t parser = {.frame = {0}, .used = 0U};
    uint32_t captured = 0U;
    int argi = 0;

    for (argi = 1; argi < argc; argi++) {
        if ((strcmp(argv[argi], "-d") == 0) || (strcmp(argv[argi], "--device") == 0)) {
            if ((argi + 1) >= argc) {
                diag_print_usage(argv[0]);
                return 2;
            }

            device_path = argv[++argi];
        } else if ((strcmp(argv[argi], "-b") == 0) || (strcmp(argv[argi], "--baud") == 0)) {
            if (((argi + 1) >= argc) || !diag_parse_u32(argv[++argi], &baud)) {
                diag_print_usage(argv[0]);
                return 2;
            }
        } else if ((strcmp(argv[argi], "-n") == 0) || (strcmp(argv[argi], "--count") == 0)) {
            if (((argi + 1) >= argc) || !diag_parse_u32(argv[++argi], &frame_limit)) {
                diag_print_usage(argv[0]);
                return 2;
            }
        } else if ((strcmp(argv[argi], "-o") == 0) || (strcmp(argv[argi], "--output") == 0)) {
            if ((argi + 1) >= argc) {
                diag_print_usage(argv[0]);
                return 2;
            }

            output_path = argv[++argi];
        } else if ((strcmp(argv[argi], "-h") == 0) || (strcmp(argv[argi], "--help") == 0)) {
            diag_print_usage(argv[0]);
            return 0;
        } else {
            diag_print_usage(argv[0]);
            return 2;
        }
    }

    if (device_path == NULL) {
        diag_print_usage(argv[0]);
        return 2;
    }

    speed = diag_baud_to_speed(baud);
    if (speed == 0) {
        (void)fprintf(stderr, "Unsupported baud rate: %lu\n", (unsigned long)baud);
        return 2;
    }

    if (output_path != NULL) {
        output_file = fopen(output_path, "w");
        if (output_file == NULL) {
            (void)fprintf(
                stderr,
                "Failed to open output file '%s': %s\n",
                output_path,
                strerror(errno)
            );
            return 1;
        }

        output_stream = output_file;
    }

    serial_fd = open(device_path, O_RDONLY | O_NOCTTY);
    if (serial_fd < 0) {
        (void)fprintf(stderr, "Failed to open device '%s': %s\n", device_path, strerror(errno));
        return 1;
    }

    if (!diag_configure_serial(serial_fd, speed, &serial_guard.original_tty)) {
        (void)fprintf(stderr, "Failed to configure serial device '%s'\n", device_path);
        return 1;
    }

    serial_guard.fd = serial_fd;
    serial_guard.restore_tty = true;

    diag_write_csv_header(output_stream);

    while ((frame_limit == 0U) || (captured < frame_limit)) {
        struct pollfd poll_fd = {
            .fd = serial_fd,
            .events = POLLIN,
            .revents = 0,
        };
        uint8_t chunk[64] = {0};
        ssize_t read_len = 0;
        int poll_result = poll(&poll_fd, 1, 1000);

        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }

            (void)fprintf(stderr, "poll failed: %s\n", strerror(errno));
            return 1;
        }

        if (poll_result == 0) {
            continue;
        }

        read_len = read(serial_fd, chunk, sizeof(chunk));
        if (read_len < 0) {
            if (errno == EINTR) {
                continue;
            }

            (void)fprintf(stderr, "read failed: %s\n", strerror(errno));
            return 1;
        }

        for (ssize_t i = 0; i < read_len; i++) {
            diag_frame_t frame = {0};

            if (!diag_parser_ingest(&parser, chunk[i], &frame)) {
                continue;
            }

            diag_write_csv_row(output_stream, diag_monotonic_ms(), &frame);
            captured = (uint32_t)(captured + 1U);

            if ((frame_limit > 0U) && (captured >= frame_limit)) {
                break;
            }
        }
    }

    return 0;
}
