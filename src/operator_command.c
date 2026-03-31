#include "operator_command.h"

#include <stddef.h>
#include <string.h>

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

bool operator_command_parse_line(
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
        return false;
    }

    *out_command = APP_OPERATOR_COMMAND_NONE;

    if (!operator_command_trim_view(line, &begin, &end)) {
        return false;
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
            return false;
        }

        if (!operator_command_token_match(parsed_token, parsed_token_len, token)) {
            return false;
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
        return false;
    }

    if (next != end) {
        return false;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "LOCKOUT_CLEAR_ALL")) {
        *out_command = APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_ALL;
        return true;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "LOCKOUT_CLEAR_LAST")) {
        *out_command = APP_OPERATOR_COMMAND_CLEAR_LOCKOUT_LAST;
        return true;
    }

    if (operator_command_token_match(parsed_token, parsed_token_len, "ROTATE_LAST")) {
        *out_command = APP_OPERATOR_COMMAND_ROTATE_SECURITY_LAST;
        return true;
    }

    return false;
}
