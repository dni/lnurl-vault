/* Base64, which carries firmware bytes.
 *
 * The only consumer is ota_chunk's `data` field: raw binary does not fit in a
 * JSON string, so an incoming firmware image arrives base64-encoded, 1024 raw
 * bytes at a time, and is decoded into a fixed stack buffer before being
 * written to flash and folded into the digest ota_finish verifies. Every byte
 * of that input is chosen by whoever is offering the image.
 *
 * So the interesting cases are malformed input and buffer discipline, not the
 * round trip. Unlike hex_decode -- which promised "out is left untouched" and
 * did not honour it -- this function validates the whole input before writing
 * any of it. These tests pin that, so it stays true.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "unity_lite.h"

static void test_rfc4648_vectors(void) {
    /* The worked examples from RFC 4648 section 10, which exercise all three
     * padding cases. */
    const char *pairs[][2] = {
        {"", ""},         {"f", "Zg=="},       {"fo", "Zm8="},
        {"foo", "Zm9v"},  {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    bool all_encode = true, all_decode = true;
    for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
        const char *raw = pairs[i][0], *b64 = pairs[i][1];
        char enc[16];
        base64_encode((const unsigned char *)raw, strlen(raw), enc);
        if (strcmp(enc, b64) != 0) {
            all_encode = false;
        }
        unsigned char dec[16];
        size_t dlen = 0;
        if (!base64_decode(b64, strlen(b64), dec, &dlen) || dlen != strlen(raw) ||
            memcmp(dec, raw, dlen) != 0) {
            all_decode = false;
        }
    }
    UL_CHECK(all_encode, "all seven RFC 4648 vectors encode correctly");
    UL_CHECK(all_decode, "and all seven decode back");
}

/* Every length up to a full OTA chunk, so no remainder case is missed: 0, 1
 * and 2 bytes past each multiple of 3 are the three distinct padding paths. */
static void test_round_trip_every_length(void) {
    static unsigned char raw[1024]; /* OTA_CHUNK_MAX_RAW */
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (unsigned char)(i * 37u + 11u);
    }
    static char enc[1400]; /* OTA_CHUNK_B64_BUF */
    static unsigned char dec[1100];

    bool all_ok = true, all_lengths_right = true;
    for (size_t n = 0; n <= sizeof(raw); n++) {
        base64_encode(raw, n, enc);
        if (strlen(enc) != base64_encoded_len(n)) {
            all_lengths_right = false;
        }
        size_t dlen = 0;
        if (!base64_decode(enc, strlen(enc), dec, &dlen) || dlen != n ||
            memcmp(dec, raw, n) != 0) {
            all_ok = false;
        }
    }
    UL_CHECK(all_ok, "every length from 0 to 1024 round-trips exactly");
    UL_CHECK(all_lengths_right, "and base64_encoded_len agrees with what was written");
}

/* The upper bound really must be an upper bound: dispatcher.c sizes its decode
 * buffer from it and enforces the real limit afterwards, precisely because
 * this over-estimates by up to two bytes when padding is present. */
static void test_decoded_len_is_never_an_underestimate(void) {
    static unsigned char raw[300];
    memset(raw, 'z', sizeof(raw));
    static char enc[512];
    static unsigned char dec[512];

    bool never_under = true;
    for (size_t n = 0; n <= sizeof(raw); n++) {
        base64_encode(raw, n, enc);
        const size_t bound = base64_decoded_len(strlen(enc));
        size_t dlen = 0;
        base64_decode(enc, strlen(enc), dec, &dlen);
        if (dlen > bound) {
            never_under = false;
        }
    }
    UL_CHECK(never_under, "the real decoded length never exceeds base64_decoded_len");
}

/* Malformed input, and the buffer discipline that goes with it. */
static void test_malformed_input_is_rejected(void) {
    unsigned char out[64];
    size_t olen = 0;

    UL_CHECK(!base64_decode("Zm9vY", 5, out, &olen), "a length that is not a multiple of 4");
    UL_CHECK(!base64_decode("Zm9", 3, out, &olen), "nor is three characters");
    UL_CHECK(!base64_decode("Zm9v!", 5, out, &olen), "nor five");

    UL_CHECK(!base64_decode("Zm9 v", 5, out, &olen), "a space is not base64");
    UL_CHECK(!base64_decode("Zm-v", 4, out, &olen), "nor is '-' (that is base64url, not this)");
    UL_CHECK(!base64_decode("Zm_v", 4, out, &olen), "nor '_'");
    UL_CHECK(!base64_decode("Zm\nv", 4, out, &olen), "nor a newline");
    UL_CHECK(!base64_decode("Zm9\x80", 4, out, &olen), "nor a high byte");

    /* Padding may only occupy the last one or two characters of the input. */
    UL_CHECK(!base64_decode("Z=9v", 4, out, &olen), "'=' in the second position is rejected");
    UL_CHECK(!base64_decode("=m9v", 4, out, &olen), "and in the first");
    UL_CHECK(!base64_decode("Zm==Zm9v", 8, out, &olen),
             "padding in a non-final group is rejected");
    UL_CHECK(!base64_decode("====", 4, out, &olen), "all padding is rejected");
}

/* THE discipline that matters, and the one hex_decode got wrong: a rejected
 * input must not leave anything in the caller's buffer. */
static void test_a_rejected_chunk_writes_nothing(void) {
    /* Valid base64 with one bad character near the END, so a
     * decode-as-you-go implementation would fill most of the buffer first. */
    static char nearly[1024];
    memset(nearly, 'A', sizeof(nearly));
    nearly[1000] = '!'; /* not a base64 character */

    static unsigned char out[1024];
    memset(out, 0x7E, sizeof(out));
    size_t olen = 12345;

    UL_CHECK(!base64_decode(nearly, sizeof(nearly), out, &olen),
             "a chunk with one bad character near the end is rejected");

    bool untouched = true;
    for (size_t i = 0; i < sizeof(out); i++) {
        if (out[i] != 0x7E) {
            untouched = false;
        }
    }
    UL_CHECK(untouched, "and not one byte of the caller's buffer is written");
    UL_CHECK(olen == 12345, "nor is the caller's length overwritten");
}

static void test_empty_and_null(void) {
    unsigned char out[8];
    size_t olen = 99;
    UL_CHECK(base64_decode("", 0, out, &olen) && olen == 0,
             "an empty string decodes to zero bytes");

    UL_CHECK(!base64_decode(NULL, 4, out, &olen), "a NULL input is rejected");
    UL_CHECK(!base64_decode("Zm9v", 4, NULL, &olen), "a NULL output is rejected");
    UL_CHECK(!base64_decode("Zm9v", 4, out, NULL), "a NULL length pointer is rejected");

    char enc[8];
    base64_encode((const unsigned char *)"", 0, enc);
    UL_CHECK(enc[0] == '\0', "encoding nothing produces an empty, terminated string");
}

/* The exact shape ota_chunk sends: a full 1024-byte chunk, and the buffer
 * sizes dispatcher.c declares for it. */
static void test_a_full_ota_chunk(void) {
    static unsigned char raw[1024];
    for (size_t i = 0; i < sizeof(raw); i++) {
        raw[i] = (unsigned char)(i ^ 0xA5);
    }
    static char enc[1400]; /* OTA_CHUNK_B64_BUF */
    base64_encode(raw, sizeof(raw), enc);
    UL_CHECK(strlen(enc) + 1 <= sizeof(enc),
             "a full 1024-byte chunk encodes within OTA_CHUNK_B64_BUF");

    static unsigned char dec[1050]; /* OTA_CHUNK_DECODE_BUF */
    size_t dlen = 0;
    UL_CHECK(base64_decode(enc, strlen(enc), dec, &dlen), "and decodes");
    UL_CHECK(dlen == sizeof(raw) && memcmp(dec, raw, dlen) == 0,
             "back to exactly the 1024 bytes that went in");
    UL_CHECK(base64_decoded_len(strlen(enc)) <= sizeof(dec),
             "and the bound dispatcher.c sizes its buffer from fits that buffer");
}

void test_base64_run(void) {
    printf("-- base64 --\n");
    test_rfc4648_vectors();
    test_round_trip_every_length();
    test_decoded_len_is_never_an_underestimate();
    test_malformed_input_is_rejected();
    test_a_rejected_chunk_writes_nothing();
    test_empty_and_null();
    test_a_full_ota_chunk();
}
