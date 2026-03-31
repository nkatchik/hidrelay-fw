#ifndef HIDRELAY_OPERATOR_AUTH_H
#define HIDRELAY_OPERATOR_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app.h"

enum {
    OPERATOR_AUTH_KEY_LEN = 32U,
    OPERATOR_AUTH_MAC_LEN = 32U,
    OPERATOR_AUTH_HEX_MAC_LEN = OPERATOR_AUTH_MAC_LEN * 2U,
    OPERATOR_AUTH_HEX_SESSION_LEN = 8U,
    OPERATOR_AUTH_HEX_NONCE_LEN = 16U,
    OPERATOR_AUTH_RESPONSE_MAX_LEN = 128U,
};

typedef struct operator_auth_config {
    uint32_t session_ttl_ms;
    uint32_t lockout_ms;
    uint8_t max_auth_failures;
} operator_auth_config_t;

typedef struct operator_auth_state {
    uint8_t root_key[OPERATOR_AUTH_KEY_LEN];
    uint32_t session_id;
    uint64_t client_nonce;
    uint64_t device_nonce;
    uint64_t next_sequence;
    uint32_t session_expiry_ms;
    uint32_t lockout_until_ms;
    uint8_t auth_failures;
    bool key_valid;
    bool challenge_active;
    bool session_active;
    operator_auth_config_t config;
} operator_auth_state_t;

typedef struct operator_auth_output {
    bool has_command;
    app_operator_command_t command;
    bool has_response;
    char response[OPERATOR_AUTH_RESPONSE_MAX_LEN];
} operator_auth_output_t;

bool operator_auth_key_from_hex(
    const char * key_hex,
    uint8_t out_key[OPERATOR_AUTH_KEY_LEN]
);

bool operator_auth_compute_proof_mac_hex(
    const uint8_t key[OPERATOR_AUTH_KEY_LEN],
    uint32_t session_id,
    uint64_t client_nonce,
    uint64_t device_nonce,
    char out_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U]
);

bool operator_auth_compute_command_mac_hex(
    const uint8_t key[OPERATOR_AUTH_KEY_LEN],
    uint32_t session_id,
    uint64_t sequence,
    const char * command,
    char out_hex[OPERATOR_AUTH_HEX_MAC_LEN + 1U]
);

bool operator_auth_state_init(
    operator_auth_state_t * state,
    const operator_auth_config_t * config,
    const char * key_hex
);

bool operator_auth_process_line(
    operator_auth_state_t * state,
    const char * line,
    uint32_t now_ms,
    uint64_t entropy,
    operator_auth_output_t * out
);

#endif
