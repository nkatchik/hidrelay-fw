#include "operator_command.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

enum {
    OPERATOR_COMMAND_DEFAULT_MIN_INTERVAL_MS = 500U,
    OPERATOR_COMMAND_DEFAULT_AUTH_LOCKOUT_MS = 30000U,
    OPERATOR_COMMAND_DEFAULT_AUTH_MAX_FAILURES = 5U,
};

static bool operator_command_whitespace(char ch) {
    return (ch == ' ') || (ch == '\t');
}

static bool operator_command_trim_view(
    const char * text,
    size_t * out_begin,
    size_t * out_end
) {
    size_t begin = 0U;
    size_t end = 0U;

    if ((text == NULL) || (out_begin == NULL) || (out_end == NULL)) {
        return false;
    }

    end = strlen(text);

    while ((begin < end) && operator_command_whitespace(text[begin])) {
        begin++;
    }

    while ((end > begin) && operator_command_whitespace(text[end - 1U])) {
        end--;
    }

    if (begin == end) {
        return false;
    }

    *out_begin = begin;
    *out_end = end;
    return true;
}

static bool operator_command_extract_token(
    const char * text,
    size_t begin,
    size_t end,
    const char ** out_token_begin,
    size_t * out_token_len,
    size_t * out_next
) {
    size_t token_end = 0U;

    if ((text == NULL)
        || (out_token_begin == NULL)
        || (out_token_len == NULL)
        || (out_next == NULL)
        || (begin >= end)) {
        return false;
    }

    token_end = begin;

    while ((token_end < end) && !operator_command_whitespace(text[token_end])) {
        token_end++;
    }

    if (token_end == begin) {
        return false;
    }

    *out_token_begin = &text[begin];
    *out_token_len = token_end - begin;

    while ((token_end < end) && operator_command_whitespace(text[token_end])) {
        token_end++;
    }

    *out_next = token_end;
    return true;
}

static bool operator_command_token_match(
    const char * token_begin,
    size_t token_len,
    const char * expected
) {
    size_t expected_len = 0U;

    if ((token_begin == NULL) || (expected == NULL)) {
        return false;
    }

    expected_len = strlen(expected);
    return (token_len == expected_len) && (memcmp(token_begin, expected, token_len) == 0);
}

operator_command_parse_result_t operator_command_parse_line_result(
    const char * line,
    const char * token,
    app_operator_command_t * out_command
) {
    size_t begin = 0U;
    size_t end = 0U;
    const char * parsed_token = NULL;
    size_t parsed_token_len = 0U;
    size_t next = 0U;

    if (out_command == NULL) {
        return OPERATOR_COMMAND_PARSE_RESULT_INVALID;
    }

    *out_command = APP_OPERATOR_COMMAND_NONE;

    if (line == NULL) {
        return OPERATOR_COMMAND_PARSE_RESULT_INVALID;
    }

    if (!operator_command_trim_view(line, &begin, &end)) {
        return OPERATOR_COMMAND_PARSE_RESULT_EMPTY;
    }

    if ((token != NULL) && (token[0] != '\0')) {
        if (!operator_command_extract_token(
                line,
                begin,
                end,
                &parsed_token,
                &parsed_token_len,
                &next
            )) {
            return OPERATOR_COMMAND_PARSE_RESULT_EMPTY;
        }

        if (!operator_command_token_match(parsed_token, parsed_token_len, token)) {
            return OPERATOR_COMMAND_PARSE_RESULT_TOKEN_MISMATCH;
        }

        begin = next;
    }

    if (!operator_command_extract_token(
            line,
            begin,
            end,
            &parsed_token,
            &parsed_token_len,
            &next
        )) {
        return OPERATOR_COMMAND_PARSE_RESULT_EMPTY;
    }

    if (next != end) {
        return OPERATOR_COMMAND_PARSE_RESULT_TRAILING_DATA;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "LOCKOUT_CLEAR_ALL")) {
        *out_command = APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL;
        return OPERATOR_COMMAND_PARSE_RESULT_OK;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "LOCKOUT_CLEAR_LAST")) {
        *out_command = APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_LAST;
        return OPERATOR_COMMAND_PARSE_RESULT_OK;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "ROTATE_LAST")) {
        *out_command = APP_OPERATOR_COMMAND_ROTATE_SECURITY_LAST;
        return OPERATOR_COMMAND_PARSE_RESULT_OK;
    }

    return OPERATOR_COMMAND_PARSE_RESULT_UNKNOWN_COMMAND;
}

bool operator_command_parse_line(
    const char * line,
    const char * token,
    app_operator_command_t * out_command
) {
    return operator_command_parse_line_result(line, token, out_command)
        == OPERATOR_COMMAND_PARSE_RESULT_OK;
}

void operator_command_policy_init(
    operator_command_policy_t * policy,
    const operator_command_policy_config_t * config
) {
    if (policy == NULL) {
        return;
    }

    (void)memset(policy, 0, sizeof(*policy));

    if (config == NULL) {
        policy->config.min_interval_ms = OPERATOR_COMMAND_DEFAULT_MIN_INTERVAL_MS;
        policy->config.auth_lockout_ms = OPERATOR_COMMAND_DEFAULT_AUTH_LOCKOUT_MS;
        policy->config.auth_max_failures = OPERATOR_COMMAND_DEFAULT_AUTH_MAX_FAILURES;
        return;
    }

    policy->config = *config;
}

static bool operator_command_policy_time_before(
    uint32_t now_ms,
    uint32_t deadline_ms
) {
    return ((int32_t)(now_ms - deadline_ms)) < 0;
}

static bool operator_command_policy_lockout_active(
    const operator_command_policy_t * policy,
    uint32_t now_ms
) {
    if ((policy == NULL) || (policy->lockout_until_ms == 0U)) {
        return false;
    }

    return operator_command_policy_time_before(now_ms, policy->lockout_until_ms);
}

static void operator_command_policy_refresh_lockout(
    operator_command_policy_t * policy,
    uint32_t now_ms
) {
    if (operator_command_policy_lockout_active(policy, now_ms)
        || (policy->lockout_until_ms == 0U)) {
        return;
    }

    policy->lockout_until_ms = 0U;
}

static void operator_command_policy_record_auth_failure(
    operator_command_policy_t * policy,
    uint32_t now_ms
) {
    if ((policy->config.auth_max_failures == 0U) || (policy->config.auth_lockout_ms == 0U)) {
        return;
    }

    if (policy->auth_failures < UINT8_MAX) {
        policy->auth_failures = (uint8_t)(policy->auth_failures + 1U);
    }

    if (policy->auth_failures >= policy->config.auth_max_failures) {
        policy->lockout_until_ms = now_ms + policy->config.auth_lockout_ms;
        policy->auth_failures = 0U;
    }
}

bool operator_command_policy_accept(
    operator_command_policy_t * policy,
    operator_command_parse_result_t parse_result,
    app_operator_command_t command,
    uint32_t now_ms
) {
    if (policy == NULL) {
        return false;
    }

    operator_command_policy_refresh_lockout(policy, now_ms);

    if (parse_result == OPERATOR_COMMAND_PARSE_RESULT_TOKEN_MISMATCH) {
        if (!operator_command_policy_lockout_active(policy, now_ms)) {
            operator_command_policy_record_auth_failure(policy, now_ms);
        }
        return false;
    }

    if ((parse_result != OPERATOR_COMMAND_PARSE_RESULT_OK)
        || (command == APP_OPERATOR_COMMAND_NONE)) {
        return false;
    }

    if (operator_command_policy_lockout_active(policy, now_ms)) {
        return false;
    }

    if (policy->has_last_accept
        && ((uint32_t)(now_ms - policy->last_accept_ms) < policy->config.min_interval_ms)) {
        return false;
    }

    policy->auth_failures = 0U;
    policy->last_accept_ms = now_ms;
    policy->has_last_accept = true;
    return true;
}
