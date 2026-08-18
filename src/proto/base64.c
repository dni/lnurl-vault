#include "base64.h"

static const char ENCODE_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encoded_len(size_t in_len) {
    return ((in_len + 2) / 3) * 4;
}

void base64_encode(const unsigned char *in, size_t in_len, char *out) {
    size_t i = 0, o = 0;
    while (i + 3 <= in_len) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8) | in[i + 2];
        out[o++] = ENCODE_TABLE[(v >> 18) & 0x3F];
        out[o++] = ENCODE_TABLE[(v >> 12) & 0x3F];
        out[o++] = ENCODE_TABLE[(v >> 6) & 0x3F];
        out[o++] = ENCODE_TABLE[v & 0x3F];
        i += 3;
    }
    size_t rem = in_len - i;
    if (rem == 1) {
        unsigned int v = (unsigned int)in[i] << 16;
        out[o++] = ENCODE_TABLE[(v >> 18) & 0x3F];
        out[o++] = ENCODE_TABLE[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        unsigned int v = ((unsigned int)in[i] << 16) | ((unsigned int)in[i + 1] << 8);
        out[o++] = ENCODE_TABLE[(v >> 18) & 0x3F];
        out[o++] = ENCODE_TABLE[(v >> 12) & 0x3F];
        out[o++] = ENCODE_TABLE[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
}

static int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

size_t base64_decoded_len(size_t in_len) {
    return (in_len / 4) * 3;
}

bool base64_decode(const char *in, size_t in_len, unsigned char *out, size_t *out_len) {
    /* Guarded rather than assumed: this parses ota_chunk's `data` field, so
     * every argument reaching it is shaped by a remote party even when the
     * pointers are the caller's. The rest of this function already validates
     * the whole input before writing any of it -- which is what makes the
     * header's "without writing partial output" true, unlike hex_decode's
     * identical promise, which was not. */
    if (!in || !out || !out_len) {
        return false;
    }
    if (in_len % 4 != 0) {
        return false;
    }
    if (in_len == 0) {
        *out_len = 0;
        return true;
    }
    size_t pad = 0;
    if (in[in_len - 1] == '=') pad++;
    if (in[in_len - 2] == '=') pad++;
    /* Padding, if present, may only occupy the final group's last 1-2
     * chars — reject '=' appearing anywhere else in the input. */
    for (size_t i = 0; i < in_len - pad; i++) {
        if (in[i] == '=' || decode_char(in[i]) < 0) {
            return false;
        }
    }

    size_t o = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int c0 = decode_char(in[i]);
        int c1 = decode_char(in[i + 1]);
        int c2 = (in[i + 2] == '=') ? 0 : decode_char(in[i + 2]);
        int c3 = (in[i + 3] == '=') ? 0 : decode_char(in[i + 3]);
        unsigned int v = ((unsigned int)c0 << 18) | ((unsigned int)c1 << 12) |
                          ((unsigned int)c2 << 6) | (unsigned int)c3;
        out[o++] = (unsigned char)((v >> 16) & 0xFF);
        if (in[i + 2] != '=') {
            out[o++] = (unsigned char)((v >> 8) & 0xFF);
        }
        if (in[i + 3] != '=') {
            out[o++] = (unsigned char)(v & 0xFF);
        }
    }
    *out_len = o;
    return true;
}
