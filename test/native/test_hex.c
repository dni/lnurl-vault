/* Hex encode/decode.
 *
 * hex_decode parses the sha256 and signature fields of an ota_begin straight
 * off the wire, into fixed buffers, from bytes a remote party chose. So the
 * cases that matter are not the round trip -- they are what happens when the
 * input is wrong, and whether the caller's buffer is left in a state that
 * could be mistaken for a valid parse.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hex.h"
#include "unity_lite.h"

static void test_round_trip(void) {
    uint8_t bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    char hex[9];
    UL_CHECK(hex_encode(bytes, 4, hex, sizeof(hex)), "encode reports success");
    UL_CHECK(strcmp(hex, "deadbeef") == 0, "encode produces lowercase hex");

    uint8_t decoded[4];
    UL_CHECK(hex_decode(hex, 8, decoded, sizeof(decoded)), "decode reports success");
    UL_CHECK(memcmp(decoded, bytes, 4) == 0, "decode round-trips to original bytes");
}

/* Every byte value, so no nibble is mis-mapped. */
static void test_all_byte_values(void) {
    uint8_t all[256];
    for (int i = 0; i < 256; i++) {
        all[i] = (uint8_t)i;
    }
    char hex[513];
    UL_CHECK(hex_encode(all, sizeof(all), hex, sizeof(hex)), "encodes all 256 byte values");

    uint8_t back[256];
    UL_CHECK(hex_decode(hex, 512, back, sizeof(back)), "and decodes them");
    UL_CHECK(memcmp(all, back, sizeof(all)) == 0, "every byte value round-trips exactly");
}

static void test_case_insensitive_decode(void) {
    uint8_t out[4];
    UL_CHECK(hex_decode("DEADBEEF", 8, out, sizeof(out)), "uppercase decodes");
    UL_CHECK(out[0] == 0xDE && out[3] == 0xEF, "to the same bytes");

    uint8_t mixed[4];
    UL_CHECK(hex_decode("DeAdBeEf", 8, mixed, sizeof(mixed)), "mixed case decodes");
    UL_CHECK(memcmp(out, mixed, 4) == 0, "identically");
}

/* THE one that matters: a rejected input must leave the caller's buffer
 * exactly as it was. It used to decode as it went and fail only on reaching
 * the bad character, so a partial parse was left behind -- and this is the
 * buffer an OTA digest and signature land in. */
static void test_a_rejected_string_writes_nothing(void) {
    const char *bad[] = {
        "xy00",        /* invalid at the very start */
        "00xy",        /* invalid at the very end */
        "aabbxxccdd",  /* invalid in the middle, after valid bytes */
        "aabbccdxdd",  /* invalid in the second nibble of a pair */
        "aabb ccdd",   /* a space is not hex */
        "aabb\ncc",    /* nor a newline */
        "aa:bb",       /* nor punctuation */
        "0x1234",      /* nor a 0x prefix */
        "aabbg0",      /* g is one past f */
        "aabbG0",      /* and so is G */
    };
    bool always_untouched = true;
    bool always_rejected = true;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t out[8];
        memset(out, 0x5A, sizeof(out));
        const size_t len = strlen(bad[i]);
        if (len % 2 != 0) {
            continue; /* covered separately */
        }
        if (hex_decode(bad[i], len, out, sizeof(out))) {
            always_rejected = false;
        }
        for (size_t b = 0; b < sizeof(out); b++) {
            if (out[b] != 0x5A) {
                always_untouched = false;
            }
        }
    }
    UL_CHECK(always_rejected, "every non-hex string is rejected");
    UL_CHECK(always_untouched, "and none of them writes a single byte into the caller's buffer");
}

/* The specific shape an attacker would choose: a long run of valid hex with
 * one bad character near the end, so a decode-as-you-go implementation fills
 * almost the whole buffer before noticing. */
static void test_a_nearly_valid_digest_writes_nothing(void) {
    char almost[65];
    memset(almost, 'a', 64);
    almost[63] = 'z'; /* one bad character, at the last position */
    almost[64] = '\0';

    uint8_t digest[32];
    memset(digest, 0xC3, sizeof(digest));

    UL_CHECK(!hex_decode(almost, 64, digest, sizeof(digest)),
             "63 good characters and one bad one is still rejected");

    bool untouched = true;
    for (size_t i = 0; i < sizeof(digest); i++) {
        if (digest[i] != 0xC3) {
            untouched = false;
        }
    }
    UL_CHECK(untouched, "and leaves all 32 bytes of the digest buffer untouched");
}

static void test_length_and_capacity(void) {
    uint8_t out[4];
    UL_CHECK(!hex_decode("abc", 3, out, sizeof(out)), "odd length is rejected");
    UL_CHECK(!hex_decode("a", 1, out, sizeof(out)), "a single character is rejected");
    UL_CHECK(hex_decode("", 0, out, sizeof(out)), "an empty string is a valid empty decode");

    uint8_t small[2];
    UL_CHECK(!hex_decode("deadbeef", 8, small, sizeof(small)),
             "a buffer too small is rejected rather than overrun");
    UL_CHECK(hex_decode("dead", 4, small, sizeof(small)), "and exactly big enough is accepted");

    char hex[9];
    uint8_t bytes[4] = {1, 2, 3, 4};
    UL_CHECK(!hex_encode(bytes, 4, hex, 8), "encode rejects a buffer with no room for the NUL");
    UL_CHECK(hex_encode(bytes, 4, hex, 9), "and accepts one with exactly enough");
    UL_CHECK(hex[8] == '\0', "terminating the result");
}

static void test_null_arguments(void) {
    uint8_t out[4];
    char hex[9];
    uint8_t bytes[4] = {1, 2, 3, 4};
    UL_CHECK(!hex_decode(NULL, 8, out, sizeof(out)), "a NULL input is rejected");
    UL_CHECK(!hex_decode("dead", 4, NULL, 4), "a NULL output is rejected, not written through");
    UL_CHECK(!hex_encode(bytes, 4, NULL, 9), "encode rejects a NULL output");
    UL_CHECK(!hex_encode(NULL, 4, hex, 9), "and a NULL input with a non-zero length");
}

/* The exact sizes the OTA path uses, since those are the only ones that
 * actually occur: a 32-byte digest as 64 characters, a 64-byte signature as
 * 128. */
static void test_the_ota_sizes(void) {
    uint8_t digest[32], sig[64];
    for (size_t i = 0; i < sizeof(digest); i++) {
        digest[i] = (uint8_t)(i * 7 + 1);
    }
    for (size_t i = 0; i < sizeof(sig); i++) {
        sig[i] = (uint8_t)(i * 11 + 3);
    }
    char dhex[65], shex[129];
    UL_CHECK(hex_encode(digest, sizeof(digest), dhex, sizeof(dhex)) && strlen(dhex) == 64,
             "a 32-byte digest encodes to exactly 64 characters");
    UL_CHECK(hex_encode(sig, sizeof(sig), shex, sizeof(shex)) && strlen(shex) == 128,
             "a 64-byte signature encodes to exactly 128");

    uint8_t d2[32], s2[64];
    UL_CHECK(hex_decode(dhex, 64, d2, sizeof(d2)) && memcmp(digest, d2, sizeof(digest)) == 0,
             "and the digest decodes back exactly");
    UL_CHECK(hex_decode(shex, 128, s2, sizeof(s2)) && memcmp(sig, s2, sizeof(sig)) == 0,
             "and so does the signature");
}

void test_hex_run(void) {
    printf("-- hex --\n");
    test_round_trip();
    test_all_byte_values();
    test_case_insensitive_decode();
    test_a_rejected_string_writes_nothing();
    test_a_nearly_valid_digest_writes_nothing();
    test_length_and_capacity();
    test_null_arguments();
    test_the_ota_sizes();
}
