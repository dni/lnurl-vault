#ifndef LNURLVAULT_SHA256_H
#define LNURLVAULT_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* Portable FIPS 180-4 SHA-256. No ESP-IDF/mbedtls dependency, so the exact
 * same implementation compiles into the firmware and into the native test
 * binary (see test/native/test_sha256.c for the known-answer vectors). */

typedef struct {
    uint32_t h[8];
    uint64_t total_len; /* total input bytes processed so far */
    uint8_t buf[64];
    size_t buf_len; /* bytes currently buffered, 0..63 */
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len);
void sha256_final(sha256_ctx_t *ctx, uint8_t out[32]);

/* One-shot convenience wrapper. */
void sha256(const uint8_t *data, size_t len, uint8_t out[32]);

#endif
