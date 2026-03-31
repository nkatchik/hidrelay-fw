#include "util/sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define UTIL_SHA256_ROTATE_RIGHT32(value, shift) \
    (((uint32_t)(value) >> (uint32_t)(shift)) | ((uint32_t)(value) << (uint32_t)(32U - (shift))))

enum {
    UTIL_SHA256_PAD_THRESHOLD = 56U,
};

static const uint32_t g_util_sha256_k[64] = {
    0x428a2f98U,
    0x71374491U,
    0xb5c0fbcfU,
    0xe9b5dba5U,
    0x3956c25bU,
    0x59f111f1U,
    0x923f82a4U,
    0xab1c5ed5U,
    0xd807aa98U,
    0x12835b01U,
    0x243185beU,
    0x550c7dc3U,
    0x72be5d74U,
    0x80deb1feU,
    0x9bdc06a7U,
    0xc19bf174U,
    0xe49b69c1U,
    0xefbe4786U,
    0x0fc19dc6U,
    0x240ca1ccU,
    0x2de92c6fU,
    0x4a7484aaU,
    0x5cb0a9dcU,
    0x76f988daU,
    0x983e5152U,
    0xa831c66dU,
    0xb00327c8U,
    0xbf597fc7U,
    0xc6e00bf3U,
    0xd5a79147U,
    0x06ca6351U,
    0x14292967U,
    0x27b70a85U,
    0x2e1b2138U,
    0x4d2c6dfcU,
    0x53380d13U,
    0x650a7354U,
    0x766a0abbU,
    0x81c2c92eU,
    0x92722c85U,
    0xa2bfe8a1U,
    0xa81a664bU,
    0xc24b8b70U,
    0xc76c51a3U,
    0xd192e819U,
    0xd6990624U,
    0xf40e3585U,
    0x106aa070U,
    0x19a4c116U,
    0x1e376c08U,
    0x2748774cU,
    0x34b0bcb5U,
    0x391c0cb3U,
    0x4ed8aa4aU,
    0x5b9cca4fU,
    0x682e6ff3U,
    0x748f82eeU,
    0x78a5636fU,
    0x84c87814U,
    0x8cc70208U,
    0x90befffaU,
    0xa4506cebU,
    0xbef9a3f7U,
    0xc67178f2U
};

static uint32_t util_sha256_read_u32_be(const uint8_t * bytes) {
    if (bytes == NULL) {
        return 0U;
    }

    return ((uint32_t)bytes[0] << 24U)
        | ((uint32_t)bytes[1] << 16U)
        | ((uint32_t)bytes[2] << 8U)
        | (uint32_t)bytes[3];
}

static void util_sha256_write_u32_be(
    uint32_t value,
    uint8_t * out_bytes
) {
    if (out_bytes == NULL) {
        return;
    }

    out_bytes[0] = (uint8_t)(value >> 24U);
    out_bytes[1] = (uint8_t)(value >> 16U);
    out_bytes[2] = (uint8_t)(value >> 8U);
    out_bytes[3] = (uint8_t)value;
}

static void util_sha256_write_u64_be(
    uint64_t value,
    uint8_t * out_bytes
) {
    uint8_t index = 0U;

    if (out_bytes == NULL) {
        return;
    }

    for (index = 0U; index < 8U; index++) {
        out_bytes[index] = (uint8_t)(value >> ((7U - index) * 8U));
    }
}

static uint32_t util_sha256_sigma0(uint32_t value) {
    return UTIL_SHA256_ROTATE_RIGHT32(value, 7U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 18U)
        ^ ((uint32_t)value >> 3U);
}

static uint32_t util_sha256_sigma1(uint32_t value) {
    return UTIL_SHA256_ROTATE_RIGHT32(value, 17U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 19U)
        ^ ((uint32_t)value >> 10U);
}

static uint32_t util_sha256_capsigma0(uint32_t value) {
    return UTIL_SHA256_ROTATE_RIGHT32(value, 2U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 13U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 22U);
}

static uint32_t util_sha256_capsigma1(uint32_t value) {
    return UTIL_SHA256_ROTATE_RIGHT32(value, 6U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 11U)
        ^ UTIL_SHA256_ROTATE_RIGHT32(value, 25U);
}

static uint32_t util_sha256_choice(
    uint32_t x,
    uint32_t y,
    uint32_t z
) {
    return (x & y) ^ ((~x) & z);
}

static uint32_t util_sha256_majority(
    uint32_t x,
    uint32_t y,
    uint32_t z
) {
    return (x & y) ^ (x & z) ^ (y & z);
}

static void util_sha256_transform(
    util_sha256_context_t * context,
    const uint8_t block[UTIL_SHA256_BLOCK_LEN]
) {
    uint32_t w[64] = {0U};
    uint32_t a = 0U;
    uint32_t b = 0U;
    uint32_t c = 0U;
    uint32_t d = 0U;
    uint32_t e = 0U;
    uint32_t f = 0U;
    uint32_t g = 0U;
    uint32_t h = 0U;
    uint8_t round = 0U;

    if ((context == NULL) || (block == NULL)) {
        return;
    }

    for (round = 0U; round < 16U; round++) {
        w[round] = util_sha256_read_u32_be(&block[(size_t)round * 4U]);
    }

    for (round = 16U; round < 64U; round++) {
        w[round] = util_sha256_sigma1(w[round - 2U])
            + w[round - 7U]
            + util_sha256_sigma0(w[round - 15U])
            + w[round - 16U];
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (round = 0U; round < 64U; round++) {
        const uint32_t t1 = h
            + util_sha256_capsigma1(e)
            + util_sha256_choice(e, f, g)
            + g_util_sha256_k[round]
            + w[round];
        const uint32_t t2 = util_sha256_capsigma0(a) + util_sha256_majority(a, b, c);

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void util_sha256_init(util_sha256_context_t * context) {
    if (context == NULL) {
        return;
    }

    (void)memset(context, 0, sizeof(*context));
    context->state[0] = 0x6a09e667U;
    context->state[1] = 0xbb67ae85U;
    context->state[2] = 0x3c6ef372U;
    context->state[3] = 0xa54ff53aU;
    context->state[4] = 0x510e527fU;
    context->state[5] = 0x9b05688cU;
    context->state[6] = 0x1f83d9abU;
    context->state[7] = 0x5be0cd19U;
}

void util_sha256_update(
    util_sha256_context_t * context,
    const uint8_t * data,
    size_t data_len
) {
    size_t offset = 0U;

    if ((context == NULL) || ((data_len > 0U) && (data == NULL))) {
        return;
    }

    if (data_len == 0U) {
        return;
    }

    context->bit_count += (uint64_t)data_len * 8U;

    while (offset < data_len) {
        size_t copy_len = (size_t)UTIL_SHA256_BLOCK_LEN - context->block_len;
        const size_t remaining = data_len - offset;

        if (copy_len > remaining) {
            copy_len = remaining;
        }

        (void)memcpy(&context->block[context->block_len], &data[offset], copy_len);
        context->block_len = (uint8_t)(context->block_len + copy_len);
        offset += copy_len;

        if (context->block_len == UTIL_SHA256_BLOCK_LEN) {
            util_sha256_transform(context, context->block);
            context->block_len = 0U;
        }
    }
}

void util_sha256_final(
    util_sha256_context_t * context,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
) {
    uint8_t length_bytes[8] = {0U};
    uint8_t digest_index = 0U;

    if ((context == NULL) || (digest == NULL)) {
        return;
    }

    util_sha256_write_u64_be(context->bit_count, length_bytes);

    context->block[context->block_len++] = 0x80U;

    if (context->block_len > UTIL_SHA256_PAD_THRESHOLD) {
        while (context->block_len < UTIL_SHA256_BLOCK_LEN) {
            context->block[context->block_len++] = 0U;
        }
        util_sha256_transform(context, context->block);
        context->block_len = 0U;
    }

    while (context->block_len < UTIL_SHA256_PAD_THRESHOLD) {
        context->block[context->block_len++] = 0U;
    }

    (void)memcpy(&context->block[UTIL_SHA256_PAD_THRESHOLD], length_bytes, sizeof(length_bytes));
    util_sha256_transform(context, context->block);

    for (digest_index = 0U; digest_index < 8U; digest_index++) {
        util_sha256_write_u32_be(context->state[digest_index], &digest[digest_index * 4U]);
    }

    (void)memset(context, 0, sizeof(*context));
    (void)memset(length_bytes, 0, sizeof(length_bytes));
}

void util_sha256(
    const uint8_t * data,
    size_t data_len,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
) {
    util_sha256_context_t context = {0};

    util_sha256_init(&context);
    util_sha256_update(&context, data, data_len);
    util_sha256_final(&context, digest);
}

static void util_sha256_secure_zero(
    uint8_t * bytes,
    size_t len
) {
    volatile uint8_t * cursor = (volatile uint8_t *)bytes;
    size_t index = 0U;

    if (bytes == NULL) {
        return;
    }

    for (index = 0U; index < len; index++) {
        cursor[index] = 0U;
    }
}

void util_hmac_sha256(
    const uint8_t * key,
    size_t key_len,
    const uint8_t * data,
    size_t data_len,
    uint8_t digest[UTIL_SHA256_DIGEST_LEN]
) {
    util_sha256_context_t inner = {0};
    util_sha256_context_t outer = {0};
    uint8_t key_block[UTIL_SHA256_BLOCK_LEN] = {0U};
    uint8_t inner_digest[UTIL_SHA256_DIGEST_LEN] = {0U};
    uint8_t index = 0U;

    if ((key == NULL) || ((data_len > 0U) && (data == NULL)) || (digest == NULL)) {
        return;
    }

    if (key_len > UTIL_SHA256_BLOCK_LEN) {
        util_sha256(key, key_len, key_block);
    } else {
        (void)memcpy(key_block, key, key_len);
    }

    for (index = 0U; index < UTIL_SHA256_BLOCK_LEN; index++) {
        key_block[index] ^= 0x36U;
    }

    util_sha256_init(&inner);
    util_sha256_update(&inner, key_block, UTIL_SHA256_BLOCK_LEN);
    util_sha256_update(&inner, data, data_len);
    util_sha256_final(&inner, inner_digest);

    for (index = 0U; index < UTIL_SHA256_BLOCK_LEN; index++) {
        key_block[index] ^= 0x36U ^ 0x5CU;
    }

    util_sha256_init(&outer);
    util_sha256_update(&outer, key_block, UTIL_SHA256_BLOCK_LEN);
    util_sha256_update(&outer, inner_digest, UTIL_SHA256_DIGEST_LEN);
    util_sha256_final(&outer, digest);

    util_sha256_secure_zero(key_block, sizeof(key_block));
    util_sha256_secure_zero(inner_digest, sizeof(inner_digest));
}
