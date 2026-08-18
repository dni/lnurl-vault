#include "hex.h"

static const char HEX_DIGITS[] = "0123456789abcdef";

bool hex_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
    if (!out || (inlen > 0 && !in) || outcap < inlen * 2 + 1) {
        return false;
    }
    for (size_t i = 0; i < inlen; i++) {
        out[2 * i] = HEX_DIGITS[in[i] >> 4];
        out[2 * i + 1] = HEX_DIGITS[in[i] & 0x0f];
    }
    out[inlen * 2] = '\0';
    return true;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool hex_decode(const char *in, size_t inlen, uint8_t *out, size_t outcap) {
    if (!in || !out || inlen % 2 != 0 || outcap < inlen / 2) {
        return false;
    }

    /* Validate the whole string BEFORE writing any of it, so the documented
     * "out is left untouched" on failure is actually true.
     *
     * It was not. The loop below used to decode and store as it went, failing
     * only when it reached a bad character -- so "aabbxx" returned false with
     * two decoded bytes already in the caller's buffer. The header has always
     * promised otherwise, and the promise is the useful part: this parses the
     * sha256 and signature fields of an ota_begin straight off the wire, so a
     * caller that trusted the contract instead of the return value would be
     * verifying a signature against a half-filled digest whose tail is
     * whatever was in that buffer before. */
    for (size_t i = 0; i < inlen; i++) {
        if (hex_nibble(in[i]) < 0) {
            return false;
        }
    }

    for (size_t i = 0; i < inlen / 2; i++) {
        const int hi = hex_nibble(in[2 * i]);
        const int lo = hex_nibble(in[2 * i + 1]);
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
