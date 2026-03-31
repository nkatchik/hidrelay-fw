#include "operator_auth.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "operator_command.h"
#include "util/sha256.h"

enum {
    OPERATOR_AUTH_COMMAND_TOKEN_MAX_LEN = 63U,
    OPERATOR_AUTH_LINE_MAX_LEN = 255U,
    OPERATOR_AUTH_DOMAIN_PROOF_LEN = 4U,
    OPERATOR_AUTH_DOMAIN_COMMAND_LEN = 4U,
    OPERATOR_AUTH_DEFAULT_SESSION_TTL_MS = 60000U,
    OPERATOR_AUTH_DEFAULT_LOCKOUT_MS = 30000U,
    OPERATOR_AUTH_DEFAULT_MAX_FAILURES = 5U,
};

static const uint8_t g_operator_auth_domain_proof[OPERATOR_AUTH_DOMAIN_PROOF_LEN] =
    {'H', 'R', 'A', '1'};
static const uint8_t g_operator_auth_domain_command[OPERATOR_AUTH_DOMAIN_COMMAND_LEN] =
    {'H', 'R', 'C', '1'};

static bool operator_auth_whitespace(char ch) {
    return (ch == ' ') || (ch == '\t');
}

static bool operator_auth_time_before(
    uint32_t now_ms,
    uint32_t deadline_ms
) {
    return ((int32_t)(now_ms - deadline_ms)) < 0;
}

static bool operator_auth_lockout_active(
    const operator_auth_state_t * state,
    uint32_t now_ms
) {
    if ((state == NULL) || (state->lockout_until_ms == 0U)) {
        return false;
    }

    return operator_auth_time_before(now_ms, state->lockout_until_ms);
}

static uint8_t operator_auth_hex_nibble(char hex) {
    if ((hex >= '0') && (hex <= '9')) {
        return (uint8_t)(hex - '0');
    }

    if ((hex >= 'a') && (hex <= 'f')) {
        return (uint8_t)(hex - 'a' + 10U);
    }

    if ((hex >= 'A') && (hex <= 'F')) {
        return (uint8_t)(hex - 'A' + 10U);
    }

    return 0xFFU;
}

static bool operator_auth_hex_decode(
    const char * hex,
    size_t hex_len,
    uint8_t * out_bytes,
    size_t out_len
) {
    size_t byte_index = 0U;

    if ((hex == NULL) || (out_bytes == NULL) || (hex_len != (out_len * 2U))) {
        return false;
    }

    for (byte_index = 0U; byte_index < out_len; byte_index++) {
        const uint8_t high = operator_auth_hex_nibble(hex[byte_index * 2U]);
        const uint8_t low = operator_auth_hex_nibble(hex[(byte_index * 2U) + 1U]);

        if ((high > 0x0FU) || (low > 0x0FU)) {
            return false;
        }

        out_bytes[byte_index] = (uint8_t)((high << 4U) | low);
    }

    return true;
}

static void operator_auth_hex_encode(
    const uint8_t * bytes,
    size_t len,
    char * out_hex
) {
    static const char hex_chars[] = "0123456789abcdef";
    size_t index = 0U;

    if ((bytes == NULL) || (out_hex == NULL)) {
        return;
    }

    for (index = 0U; index < len; index++) {
        out_hex[index * 2U] = hex_chars[(bytes[index] >> 4U) & 0x0FU];
        out_hex[(index * 2U) + 1U] = hex_chars[bytes[index] & 0x0FU];
    }

    out_hex[len * 2U] = '\0';
}

static void operator_auth_write_u32_le(
    uint32_t value,
    uint8_t out_bytes[4]
) {
    if (out_bytes == NULL) {
        return;
    }

    out_bytes[0] = (uint8_t)(value & 0xFFU);
    out_bytes[1] = (uint8_t)((value >> 8U) & 0xFFU);
    out_bytes[2] = (uint8_t)((value >> 16U) & 0xFFU);
    out_bytes[3] = (uint8_t)((value >> 24U) & 0xFFU);
}

static void operator_auth_write_u64_le(
    uint64_t value,
    uint8_t out_bytes[8]
) {
    uint8_t index = 0U;

    if (out_bytes == NULL) {
        return;
    }

    for (index = 0U; index < 8U; index++) {
        out_bytes[index] = (uint8_t)((value >> (index * 8U)) & 0xFFU);
    }
}

static bool operator_auth_constant_time_eq(
    const uint8_t * lhs,
    const uint8_t * rhs,
    size_t len
) {
    uint8_t diff = 0U;
    size_t index = 0U;

    if ((lhs == NULL) || (rhs == NULL)) {
        return false;
    }

    for (index = 0U; index < len; index++) {
        diff |= (uint8_t)(lhs[index] ^ rhs[index]);
    }

    return diff == 0U;
}

static void operator_auth_output_reset(operator_auth_output_t * out) {
    if (out == NULL) {
        return;
    }

    (void)memset(out, 0, sizeof(*out));
    out->command = APP_OPERATOR_COMMAND_NONE;
}

static bool operator_auth_take_token(
    const char * text,
    size_t text_len,
    size_t * io_offset,
    const char ** out_token,
    size_t * out_token_len
) {
    size_t offset = 0U;
    size_t end = 0U;

    if ((text == NULL)
        || (io_offset == NULL)
        || (out_token == NULL)
        || (out_token_len == NULL)
        || (*io_offset > text_len)) {
        return false;
    }

    offset = *io_offset;
    while ((offset < text_len) && operator_auth_whitespace(text[offset])) {
        offset++;
    }

    if (offset >= text_len) {
        return false;
    }

    end = offset;
    while ((end < text_len) && !operator_auth_whitespace(text[end])) {
        end++;
    }

    *out_token = &text[offset];
    *out_token_len = end - offset;
    *io_offset = end;
    return true;
}

static bool operator_auth_only_whitespace_remaining(
    const char * text,
    size_t text_len,
    size_t offset
) {
    size_t cursor = offset;

    if (text == NULL) {
        return false;
    }

    while (cursor < text_len) {
        if (!operator_auth_whitespace(text[cursor])) {
            return false;
        }
        cursor++;
    }

    return true;
}

static bool operator_auth_token_equals(
    const char * token,
    size_t token_len,
    const char * expected
) {
    const size_t expected_len = (expected == NULL) ? 0U : strlen(expected);

    if ((token == NULL) || (expected == NULL) || (token_len != expected_len)) {
        return false;
    }

    return memcmp(token, expected, token_len) == 0;
}

static bool operator_auth_token_to_buffer(
    const char * token,
    size_t token_len,
    char * out_buffer,
    size_t out_buffer_len
) {
    if ((token == NULL) || (out_buffer == NULL) || (token_len == 0U) || (out_buffer_len == 0U)) {
        return false;
    }

    if (token_len >= out_buffer_len) {
        return false;
    }

    (void)memcpy(out_buffer, token, token_len);
    out_buffer[token_len] = '\0';
    return true;
}

static bool operator_auth_parse_u32_hex_token(
    const char * token,
    size_t token_len,
    uint32_t * out_value
) {
    uint8_t bytes[4] = {0U};

    if ((out_value == NULL) || (token_len != OPERATOR_AUTH_HEX_SESSION_LEN)) {
        return false;
    }

    if (!operator_auth_hex_decode(token, token_len, bytes, sizeof(bytes))) {
        return false;
    }

    *out_value = ((uint32_t)bytes[0] << 24U)
        | ((uint32_t)bytes[1] << 16U)
        | ((uint32_t)bytes[2] << 8U)
        | (uint32_t)bytes[3];
    return true;
}

static bool operator_auth_parse_u64_hex_token(
    const char * token,
    size_t token_len,
    uint64_t * out_value
) {
    uint8_t bytes[8] = {0U};
    uint8_t index = 0U;
    uint64_t value = 0U;

    if ((out_value == NULL) || (token_len != OPERATOR_AUTH_HEX_NONCE_LEN)) {
        return false;
    }

    if (!operator_auth_hex_decode(token, token_len, bytes, sizeof(bytes))) {
        return false;
    }

    for (index = 0U; index < 8U; index++) {
        value = (value << 8U) | bytes[index];
    }

    *out_value = value;
    return true;
}

static bool operator_auth_parse_u64_dec_token(
    const char * token,
    size_t token_len,
    uint64_t * out_value
) {
    uint64_t value = 0U;
    size_t index = 0U;

    if ((token == NULL) || (out_value == NULL) || (token_len == 0U)) {
        return false;
    }

    for (index = 0U; index < token_len; index++) {
        if ((token[index] < '0') || (token[index] > '9')) {
            return false;
        }
        value = (value * 10U) + (uint64_t)(token[index] - '0');
    }

    *out_value = value;
    return true;
}

static void operator_auth_record_failure(
    operator_auth_state_t * state,
    uint32_t now_ms
) {
    if (state == NULL) {
        return;
    }

    if (state->config.max_auth_failures == 0U) {
        return;
    }

    if (state->auth_failures < UINT8_MAX) {
        state->auth_failures = (uint8_t)(state->auth_failures + 1U);
    }

    if (state->auth_failures >= state->config.max_auth_failures) {
        state->auth_failures = 0U;
        state->lockout_until_ms = now_ms + state->config.lockout_ms;
        state->challenge_active = false;
        state->session_active = false;
    }
}

static bool operator_auth_response_set(
    operator_auth_output_t * out,
    const char * format,
    ...
) {
    bool success = false;
    va_list args;
    int written = 0;

    if ((out == NULL) || (format == NULL)) {
        return false;
    }

    va_start(args, format);
    written = vsnprintf(out->response, sizeof(out->response), format, args);
    va_end(args);

    success = (written > 0) && ((size_t)written < sizeof(out->response));
    out->has_response = success;
    return success;
}

static uint32_t operator_auth_mix_session_id(
    uint32_t now_ms,
    uint64_t entropy
) {
    uint64_t mixed = entropy ^ (((uint64_t)now_ms << 32U) | (uint64_t)now_ms);

    mixed ^= mixed >> 33U;
    mixed *= 0xff51afd7ed558ccdULL;
    mixed ^= mixed >> 33U;
    mixed *= 0xc4ceb9fe1a85ec53ULL;
    mixed ^= mixed >> 33U;

    if ((uint32_t)mixed == 0U) {
        return 1U;
    }

    return (uint32_t)mixed;
}

static uint64_t operator_auth_mix_nonce(
    uint32_t now_ms,
    uint64_t entropy
) {
    uint64_t mixed = entropy ^ ((uint64_t)now_ms * 0x9e3779b97f4a7c15ULL);

    mixed ^= mixed >> 30U;
    mixed *= 0xbf58476d1ce4e5b9ULL;
    mixed ^= mixed >> 27U;
    mixed *= 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;

    if (mixed == 0U) {
        return 1U;
    }

    return mixed;
}

bool operator_auth_key_from_hex(
    const char * key_hex,
    uint8_t out_key[OPERATOR_AUTH_KEY_LEN]
) {
    if ((key_hex == NULL) || (out_key == NULL)) {
        return false;
    }

    return operator_auth_hex_decode(key_hex, strlen(key_hex), out_key, OPERATOR_AUTH_KEY_LEN);
}

bool operator_auth_compute_proof_mac_hex(
    const uint8_t key[OPERATOR_AUTH_KEY_LEN],
    uint32_t session_id,
    uint64_t client_nonce,
    uint64_t device_nonce,
    char out_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U]
) {
    uint8_t message[OPERATOR_AUTH_DOMAIN_PROOF_LEN + 4U + 8U + 8U] = {0U};
    uint8_t digest[OPERATOR_AUTH_MAC_LEN] = {0U};

    if ((key == NULL) || (out_hex == NULL)) {
        return false;
    }

    (void)memcpy(message, g_operator_auth_domain_proof, OPERATOR_AUTH_DOMAIN_PROOF_LEN);
    operator_auth_write_u32_le(session_id, &message[OPERATOR_AUTH_DOMAIN_PROOF_LEN]);
    operator_auth_write_u64_le(client_nonce, &message[OPERATOR_AUTH_DOMAIN_PROOF_LEN + 4U]);
    operator_auth_write_u64_le(device_nonce, &message[OPERATOR_AUTH_DOMAIN_PROOF_LEN + 12U]);

    util_hmac_sha256(key, OPERATOR_AUTH_KEY_LEN, message, sizeof(message), digest);
    operator_auth_hex_encode(digest, sizeof(digest), out_hex);
    return true;
}

bool operator_auth_compute_command_mac_hex(
    const uint8_t key[OPERATOR_AUTH_KEY_LEN],
    uint32_t session_id,
    uint64_t sequence,
    const char * command,
    char out_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U]
) {
    uint8_t
        message[OPERATOR_AUTH_DOMAIN_COMMAND_LEN + 4U + 8U + OPERATOR_AUTH_COMMAND_TOKEN_MAX_LEN] =
            {0U};
    uint8_t digest[OPERATOR_AUTH_MAC_LEN] = {0U};
    size_t message_len = 0U;
    size_t command_len = 0U;

    if ((key == NULL) || (command == NULL) || (out_hex == NULL)) {
        return false;
    }

    command_len = strlen(command);
    if ((command_len == 0U) || (command_len > OPERATOR_AUTH_COMMAND_TOKEN_MAX_LEN)) {
        return false;
    }

    (void)memcpy(message, g_operator_auth_domain_command, OPERATOR_AUTH_DOMAIN_COMMAND_LEN);
    operator_auth_write_u32_le(session_id, &message[OPERATOR_AUTH_DOMAIN_COMMAND_LEN]);
    operator_auth_write_u64_le(sequence, &message[OPERATOR_AUTH_DOMAIN_COMMAND_LEN + 4U]);
    (void)memcpy(&message[OPERATOR_AUTH_DOMAIN_COMMAND_LEN + 12U], command, command_len);

    message_len = OPERATOR_AUTH_DOMAIN_COMMAND_LEN + 12U + command_len;
    util_hmac_sha256(key, OPERATOR_AUTH_KEY_LEN, message, message_len, digest);
    operator_auth_hex_encode(digest, sizeof(digest), out_hex);
    return true;
}

bool operator_auth_state_init(
    operator_auth_state_t * state,
    const operator_auth_config_t * config,
    const char * key_hex
) {
    operator_auth_config_t effective_config = {
        .session_ttl_ms = OPERATOR_AUTH_DEFAULT_SESSION_TTL_MS,
        .lockout_ms = OPERATOR_AUTH_DEFAULT_LOCKOUT_MS,
        .max_auth_failures = OPERATOR_AUTH_DEFAULT_MAX_FAILURES,
    };

    if ((state == NULL) || (key_hex == NULL)) {
        return false;
    }

    (void)memset(state, 0, sizeof(*state));

    if (config != NULL) {
        effective_config = *config;
    }

    if (effective_config.session_ttl_ms == 0U) {
        effective_config.session_ttl_ms = OPERATOR_AUTH_DEFAULT_SESSION_TTL_MS;
    }

    if ((effective_config.max_auth_failures > 0U) && (effective_config.lockout_ms == 0U)) {
        effective_config.lockout_ms = OPERATOR_AUTH_DEFAULT_LOCKOUT_MS;
    }

    state->config = effective_config;
    state->key_valid = operator_auth_key_from_hex(key_hex, state->root_key);
    return state->key_valid;
}

static bool operator_auth_handle_hello(
    operator_auth_state_t * state,
    const char * nonce_token,
    size_t nonce_len,
    uint32_t now_ms,
    uint64_t entropy,
    operator_auth_output_t * out
) {
    uint64_t client_nonce = 0U;

    if ((state == NULL) || (nonce_token == NULL) || (out == NULL)) {
        return false;
    }

    if (!operator_auth_parse_u64_hex_token(nonce_token, nonce_len, &client_nonce)) {
        return operator_auth_response_set(out, "ERR AUTH_HELLO_NONCE");
    }

    state->session_id = operator_auth_mix_session_id(now_ms, entropy);
    state->client_nonce = client_nonce;
    state->device_nonce = operator_auth_mix_nonce(now_ms, entropy ^ 0xA5A5A5A5A5A5A5A5ULL);
    state->challenge_active = true;
    state->session_active = false;

    return operator_auth_response_set(
        out,
        "AUTH CHALLENGE %08" PRIx32 " %016" PRIx64 " %" PRIu32,
        state->session_id,
        state->device_nonce,
        state->config.session_ttl_ms
    );
}

static bool operator_auth_handle_prove(
    operator_auth_state_t * state,
    const char * session_token,
    size_t session_len,
    const char * mac_token,
    size_t mac_len,
    uint32_t now_ms,
    operator_auth_output_t * out
) {
    uint32_t session_id = 0U;
    uint8_t provided_mac[OPERATOR_AUTH_MAC_LEN] = {0U};
    uint8_t expected_mac[OPERATOR_AUTH_MAC_LEN] = {0U};
    char expected_mac_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U] = {0};

    if ((state == NULL) || (session_token == NULL) || (mac_token == NULL) || (out == NULL)) {
        return false;
    }

    if (!state->challenge_active) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "AUTH FAIL NO_CHALLENGE");
    }

    if (!operator_auth_parse_u32_hex_token(session_token, session_len, &session_id)
        || (session_id != state->session_id)) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "AUTH FAIL SESSION");
    }

    if ((mac_len != OPERATOR_AUTH_HEX_MAC_LEN)
        || !operator_auth_hex_decode(mac_token, mac_len, provided_mac, sizeof(provided_mac))
        || !operator_auth_compute_proof_mac_hex(
            state->root_key,
            state->session_id,
            state->client_nonce,
            state->device_nonce,
            expected_mac_hex
        )
        || !operator_auth_hex_decode(
            expected_mac_hex,
            OPERATOR_AUTH_HEX_MAC_LEN,
            expected_mac,
            sizeof(expected_mac)
        )) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "AUTH FAIL FORMAT");
    }

    if (!operator_auth_constant_time_eq(provided_mac, expected_mac, sizeof(expected_mac))) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "AUTH FAIL MAC");
    }

    state->challenge_active = false;
    state->session_active = true;
    state->session_expiry_ms = now_ms + state->config.session_ttl_ms;
    state->next_sequence = 1U;
    state->auth_failures = 0U;
    state->lockout_until_ms = 0U;
    return operator_auth_response_set(out, "AUTH OK %08" PRIx32, state->session_id);
}

static bool operator_auth_handle_command(
    operator_auth_state_t * state,
    const char * session_token,
    size_t session_len,
    const char * seq_token,
    size_t seq_len,
    const char * command_token,
    size_t command_len,
    const char * mac_token,
    size_t mac_len,
    uint32_t now_ms,
    operator_auth_output_t * out
) {
    char command_buffer[OPERATOR_AUTH_COMMAND_TOKEN_MAX_LEN + 1U] = {0};
    char expected_mac_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U] = {0};
    uint8_t expected_mac[OPERATOR_AUTH_MAC_LEN] = {0U};
    uint8_t provided_mac[OPERATOR_AUTH_MAC_LEN] = {0U};
    uint32_t session_id = 0U;
    uint64_t sequence = 0U;
    operator_command_parse_result_t parse_result = OPERATOR_COMMAND_PARSE_RESULT_INVALID;

    if ((state == NULL) || (out == NULL)) {
        return false;
    }

    if (!state->session_active) {
        return operator_auth_response_set(out, "ERR NO_SESSION");
    }

    if (!operator_auth_time_before(now_ms, state->session_expiry_ms)) {
        state->session_active = false;
        return operator_auth_response_set(out, "ERR SESSION_EXPIRED");
    }

    if (!operator_auth_parse_u32_hex_token(session_token, session_len, &session_id)
        || (session_id != state->session_id)) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "CMD FAIL SESSION");
    }

    if (!operator_auth_parse_u64_dec_token(seq_token, seq_len, &sequence)
        || (sequence != state->next_sequence)) {
        return operator_auth_response_set(out, "CMD FAIL SEQ");
    }

    if (!operator_auth_token_to_buffer(
            command_token,
            command_len,
            command_buffer,
            sizeof(command_buffer)
        )) {
        return operator_auth_response_set(out, "CMD FAIL COMMAND");
    }

    if ((mac_len != OPERATOR_AUTH_HEX_MAC_LEN)
        || !operator_auth_hex_decode(mac_token, mac_len, provided_mac, sizeof(provided_mac))
        || !operator_auth_compute_command_mac_hex(
            state->root_key,
            state->session_id,
            sequence,
            command_buffer,
            expected_mac_hex
        )
        || !operator_auth_hex_decode(
            expected_mac_hex,
            OPERATOR_AUTH_HEX_MAC_LEN,
            expected_mac,
            sizeof(expected_mac)
        )) {
        return operator_auth_response_set(out, "CMD FAIL FORMAT");
    }

    if (!operator_auth_constant_time_eq(provided_mac, expected_mac, sizeof(expected_mac))) {
        operator_auth_record_failure(state, now_ms);
        return operator_auth_response_set(out, "CMD FAIL MAC");
    }

    parse_result = operator_command_parse_line_result(command_buffer, NULL, &out->command);
    if ((parse_result != OPERATOR_COMMAND_PARSE_RESULT_OK)
        || (out->command == APP_OPERATOR_COMMAND_NONE)) {
        return operator_auth_response_set(out, "CMD FAIL COMMAND");
    }

    out->has_command = true;
    state->next_sequence = state->next_sequence + 1U;
    state->auth_failures = 0U;
    return operator_auth_response_set(out, "CMD OK %" PRIu64, sequence);
}

bool operator_auth_process_line(
    operator_auth_state_t * state,
    const char * line,
    uint32_t now_ms,
    uint64_t entropy,
    operator_auth_output_t * out
) {
    const char * token = NULL;
    const char * token2 = NULL;
    const char * token3 = NULL;
    const char * token4 = NULL;
    const char * token5 = NULL;
    size_t token_len = 0U;
    size_t token2_len = 0U;
    size_t token3_len = 0U;
    size_t token4_len = 0U;
    size_t token5_len = 0U;
    size_t offset = 0U;
    size_t line_len = 0U;

    if ((state == NULL) || (line == NULL) || (out == NULL)) {
        return false;
    }

    operator_auth_output_reset(out);

    if (!state->key_valid) {
        return operator_auth_response_set(out, "ERR AUTH_KEY");
    }

    line_len = strlen(line);
    if ((line_len == 0U) || (line_len > OPERATOR_AUTH_LINE_MAX_LEN)) {
        return false;
    }

    if (!operator_auth_take_token(line, line_len, &offset, &token, &token_len)) {
        return false;
    }

    if (!operator_auth_lockout_active(state, now_ms) && (state->lockout_until_ms != 0U)) {
        state->lockout_until_ms = 0U;
    }

    if (operator_auth_token_equals(token, token_len, "AUTH")) {
        if (operator_auth_lockout_active(state, now_ms)) {
            return operator_auth_response_set(out, "ERR AUTH_LOCKED");
        }

        if (!operator_auth_take_token(line, line_len, &offset, &token2, &token2_len)) {
            return operator_auth_response_set(out, "ERR AUTH_COMMAND");
        }

        if (operator_auth_token_equals(token2, token2_len, "HELLO")) {
            if (!operator_auth_take_token(line, line_len, &offset, &token3, &token3_len)
                || !operator_auth_only_whitespace_remaining(line, line_len, offset)) {
                return operator_auth_response_set(out, "ERR AUTH_HELLO");
            }

            return operator_auth_handle_hello(state, token3, token3_len, now_ms, entropy, out);
        }

        if (operator_auth_token_equals(token2, token2_len, "PROVE")) {
            if (!operator_auth_take_token(line, line_len, &offset, &token3, &token3_len)
                || !operator_auth_take_token(line, line_len, &offset, &token4, &token4_len)
                || !operator_auth_only_whitespace_remaining(line, line_len, offset)) {
                return operator_auth_response_set(out, "ERR AUTH_PROVE");
            }

            return operator_auth_handle_prove(
                state,
                token3,
                token3_len,
                token4,
                token4_len,
                now_ms,
                out
            );
        }

        return operator_auth_response_set(out, "ERR AUTH_COMMAND");
    }

    if (operator_auth_token_equals(token, token_len, "CMD")) {
        if (operator_auth_lockout_active(state, now_ms)) {
            return operator_auth_response_set(out, "ERR AUTH_LOCKED");
        }

        if (!operator_auth_take_token(line, line_len, &offset, &token2, &token2_len)
            || !operator_auth_take_token(line, line_len, &offset, &token3, &token3_len)
            || !operator_auth_take_token(line, line_len, &offset, &token4, &token4_len)
            || !operator_auth_take_token(line, line_len, &offset, &token5, &token5_len)
            || !operator_auth_only_whitespace_remaining(line, line_len, offset)) {
            return operator_auth_response_set(out, "ERR CMD_FORMAT");
        }

        return operator_auth_handle_command(
            state,
            token2,
            token2_len,
            token3,
            token3_len,
            token4,
            token4_len,
            token5,
            token5_len,
            now_ms,
            out
        );
    }

    return operator_auth_response_set(out, "ERR UNKNOWN");
}
