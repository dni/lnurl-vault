#include "bech32.h"

#include <string.h>

/* BIP-173's charset, in its canonical order: the index IS the 5-bit value.
 * Held uppercase because that is the only case this encoder emits (see
 * bech32.h); the spec writes it lowercase, and the two differ by case alone.
 * Note what it excludes -- 1, b, i and o -- so that no two characters in it
 * can be confused by a person reading one off a screen. */
/* Sized by the initialiser rather than pinned to [32]: the NUL is never
 * indexed, but gcc rightly refuses a 33-byte literal in a 32-byte array. */
static const char CHARSET_UPPER[] = "QPZRY9X8GF2TVDW0S3JN54KHCE6MUA7L";

/* The BCH generator, straight from BIP-173's reference implementation. */
static const uint32_t GEN[5] = {0x3b6a57b2u, 0x26508e6du, 0x1ea119fau, 0x3d4233ddu, 0x2a1462b3u};

static uint32_t polymod_step(uint32_t chk, uint8_t value) {
    const uint32_t top = chk >> 25;
    chk = ((chk & 0x1ffffffu) << 5) ^ value;
    for (int i = 0; i < 5; i++) {
        if ((top >> i) & 1u) {
            chk ^= GEN[i];
        }
    }
    return chk;
}

/* Streams `data` as 5-bit groups, most significant first, with the final
 * partial group zero-padded on the right -- BIP-173's convertbits(8, 5, pad).
 *
 * A cursor rather than an output array so the caller can walk the data twice
 * (once to build the checksum, once to emit characters) without holding
 * len * 8 / 5 bytes of it. On this device that is the difference between a
 * few dozen bytes of stack and four hundred. */
typedef struct {
    const uint8_t *data;
    size_t len;
    size_t i;
    uint32_t acc;
    int bits;
    bool flushed;
} groups_t;

static void groups_init(groups_t *g, const uint8_t *data, size_t len) {
    g->data = data;
    g->len = len;
    g->i = 0;
    g->acc = 0;
    g->bits = 0;
    g->flushed = false;
}

static bool groups_next(groups_t *g, uint8_t *out) {
    while (g->bits < 5 && g->i < g->len) {
        g->acc = (g->acc << 8) | g->data[g->i++];
        g->bits += 8;
    }
    if (g->bits >= 5) {
        g->bits -= 5;
        *out = (uint8_t)((g->acc >> g->bits) & 31u);
        return true;
    }
    if (g->bits > 0 && !g->flushed) {
        /* The tail, padded right with zeroes. */
        *out = (uint8_t)((g->acc << (5 - g->bits)) & 31u);
        g->bits = 0;
        g->flushed = true;
        return true;
    }
    return false;
}

static size_t group_count(size_t data_len) {
    return (data_len * 8u + 4u) / 5u;
}

static bool hrp_ok(const char *hrp, size_t *out_len) {
    if (!hrp || !hrp[0]) {
        return false;
    }
    const size_t n = strlen(hrp);
    for (size_t i = 0; i < n; i++) {
        const unsigned char c = (unsigned char)hrp[i];
        /* BIP-173: the human-readable part is US-ASCII 33..126. */
        if (c < 33 || c > 126) {
            return false;
        }
    }
    *out_len = n;
    return true;
}

size_t bech32_encoded_len(const char *hrp, size_t data_len) {
    size_t hrp_len = 0;
    if (!hrp_ok(hrp, &hrp_len)) {
        return 0;
    }
    /* hrp + '1' + data groups + 6 checksum characters. */
    const size_t total = hrp_len + 1 + group_count(data_len) + 6;
    return total > BECH32_MAX_OUT ? 0 : total;
}

/* Lowercase for the checksum, whatever case goes out.
 *
 * BIP-173 defines the checksum over the LOWERCASE form, and a string is
 * valid in either case as long as it is not mixed. Computing it over the
 * uppercase HRP would produce something no decoder accepts -- and it would
 * only show up as a wallet rejecting the code, which on a bearer note reads
 * as "this device is broken" rather than as an encoding bug. */
static uint8_t hrp_lower(char c) {
    return (uint8_t)((c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c);
}

bool bech32_encode_upper(const char *hrp, const uint8_t *data, size_t data_len, char *out,
                          size_t outcap) {
    if (!out || outcap == 0) {
        return false;
    }
    /* Emptied first, so no failure path can leave a partial string behind for
     * a caller that forgot to check. */
    out[0] = '\0';
    if (!data && data_len > 0) {
        return false;
    }

    size_t hrp_len = 0;
    if (!hrp_ok(hrp, &hrp_len)) {
        return false;
    }
    const size_t needed = bech32_encoded_len(hrp, data_len);
    if (needed == 0 || needed + 1 > outcap) {
        return false;
    }

    /* Checksum first, over hrp_expand(hrp) + data + six zeroes. */
    uint32_t chk = 1;
    for (size_t i = 0; i < hrp_len; i++) {
        chk = polymod_step(chk, (uint8_t)(hrp_lower(hrp[i]) >> 5));
    }
    chk = polymod_step(chk, 0);
    for (size_t i = 0; i < hrp_len; i++) {
        chk = polymod_step(chk, (uint8_t)(hrp_lower(hrp[i]) & 31u));
    }
    groups_t g;
    groups_init(&g, data, data_len);
    uint8_t v;
    while (groups_next(&g, &v)) {
        chk = polymod_step(chk, v);
    }
    for (int i = 0; i < 6; i++) {
        chk = polymod_step(chk, 0);
    }
    chk ^= 1u;

    /* Then emit, walking the data a second time rather than having stored it. */
    size_t n = 0;
    for (size_t i = 0; i < hrp_len; i++) {
        const char c = hrp[i];
        out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    out[n++] = '1';
    groups_init(&g, data, data_len);
    while (groups_next(&g, &v)) {
        out[n++] = CHARSET_UPPER[v];
    }
    for (int i = 0; i < 6; i++) {
        out[n++] = CHARSET_UPPER[(chk >> (5 * (5 - i))) & 31u];
    }
    out[n] = '\0';
    return true;
}
