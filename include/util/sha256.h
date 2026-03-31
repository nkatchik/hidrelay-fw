#ifndef HIDRELAY_UTIL_SHA256_H
#define HIDRELAY_UTIL_SHA256_H

#include <stddef.h>
#include <stdint.h>

enum {
    UTIL_SHA256_BLOCK_LEN = 64U,
    UTIL_SHA256_DIGEST_LEN = 32U,
};

typedef struct util_sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[UTIL_SHA256_BLOCK_LEN];
    uint8_t block_len;
} util_sha256_context_t;

void util_sha256_init(util_sha256_context_t * context);
void util_sha256_update(
    util_sha256_context_t * context,
    const uint8_t * data,
    size_t data_len
);
void util_sha256_final(
    util_sha256_context_t * context,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
);

void util_sha256(
    const uint8_t * data,
    size_t data_len,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
);

void util_hmac_sha256(
    const uint8_t * key,
    size_t key_len,
    const uint8_t * data,
    size_t data_len,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
);

#endif
