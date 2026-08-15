#ifndef LNURLVAULT_HEX_H
#define LNURLVAULT_HEX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Encodes inlen bytes as lowercase hex into out, NUL-terminated.
 * outcap must be >= inlen*2 + 1. Returns false (out left untouched) on
 * insufficient capacity. */
bool hex_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap);

/* Decodes exactly inlen hex chars (must be even, lowercase or uppercase)
 * into out. outcap must be >= inlen/2. Returns false on odd length, an
 * invalid character, or insufficient capacity — out is left untouched. */
bool hex_decode(const char *in, size_t inlen, uint8_t *out, size_t outcap);

#endif
