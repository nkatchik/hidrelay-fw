#ifndef HIDRELAY_OPERATOR_COMMAND_H
#define HIDRELAY_OPERATOR_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

typedef enum operator_command_parse_result {
    OPERATOR_COMMAND_PARSE_RESULT_INVALID = 0,
    OPERATOR_COMMAND_PARSE_RESULT_OK = 1,
    OPERATOR_COMMAND_PARSE_RESULT_EMPTY = 2,
    OPERATOR_COMMAND_PARSE_RESULT_TOKEN_MISMATCH = 3,
    OPERATOR_COMMAND_PARSE_RESULT_UNKNOWN_COMMAND = 4,
    OPERATOR_COMMAND_PARSE_RESULT_TRAILING_DATA = 5,
} operator_command_parse_result_t;

typedef struct operator_command_policy_config {
    uint32_t min_interval_ms;
    uint32_t auth_lockout_ms;
    uint8_t auth_max_failures;
} operator_command_policy_config_t;

typedef struct operator_command_policy {
    operator_command_policy_config_t config;
    uint32_t last_accept_ms;
    uint32_t lockout_until_ms;
    uint8_t auth_failures;
    bool has_last_accept;
} operator_command_policy_t;

operator_command_parse_result_t operator_command_parse_line_result(
    const char * line,
    const char * token,
    app_operator_command_t * out_command
);

bool operator_command_parse_line(
    const char * line,
    const char * token,
    app_operator_command_t * out_command
);

void operator_command_policy_init(
    operator_command_policy_t * policy,
    const operator_command_policy_config_t * config
);

bool operator_command_policy_accept(
    operator_command_policy_t * policy,
    operator_command_parse_result_t parse_result,
    app_operator_command_t command,
    uint32_t now_ms
);

#endif
