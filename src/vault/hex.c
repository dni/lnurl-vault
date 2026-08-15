#include "hex.h"

static const char HEX_DIGITS[] = "0123456789abcdef";

bool hex_encode(const uint8_t *in, size_t inlen, char *out, size_t outcap) {
    if (outcap < inlen * 2 + 1) {
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
    if (inlen % 2 != 0 || outcap < inlen / 2) {
        return false;
    }
    for (size_t i = 0; i < inlen / 2; i++) {
        int hi = hex_nibble(in[2 * i]);
        int lo = hex_nibble(in[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}
