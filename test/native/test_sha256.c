#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "hex.h"
#include "sha256.h"
#include "unity_lite.h"

static void check_vector(const char *input, const char *expected_hex) {
    uint8_t out[32];
    sha256((const uint8_t *)input, strlen(input), out);
    char hex[65];
    hex_encode(out, sizeof(out), hex, sizeof(hex));
    UL_CHECK(strcmp(hex, expected_hex) == 0, "sha256 known-answer vector mismatch");
}

/* The streaming API, which is what actually matters here.
 *
 * The one-shot sha256() above is not what the OTA path uses. dispatcher.c
 * hashes an incoming image incrementally -- sha256_init at ota_begin, one
 * sha256_update per 1024-byte chunk, sha256_final at ota_finish -- and that
 * digest is what ota_finish re-verifies the signature against. It is the check
 * that carries the whole OTA security guarantee: the claimed digest is checked
 * before the owner is asked, and THIS digest, over the bytes actually written
 * to flash, is checked before the boot partition is switched.
 *
 * None of that streaming path was tested. A bug in the 64-byte block buffering
 * -- the boundary between one update() and the next -- would produce a digest
 * that disagrees with the signature and refuse every legitimate image, or
 * worse, agree when it should not. */

/* Hashing the same bytes in ANY chunking must equal hashing them at once.
 * That is the property the OTA path depends on, and chunk boundaries are
 * exactly where a block-buffering bug hides. */
static void test_streaming_matches_one_shot(void) {
    /* Deterministic pseudo-random data, so a failure is reproducible. */
    static uint8_t data[4096];
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < sizeof(data); i++) {
        x = x * 1103515245u + 12345u;
        data[i] = (uint8_t)(x >> 16);
    }

    uint8_t once[32];
    sha256(data, sizeof(data), once);

    /* Chunk sizes chosen to straddle the 64-byte block in every awkward way:
     * smaller than a block, exactly a block, one either side, and sizes that
     * never align with it. 1024 is what ota_chunk actually sends. */
    const size_t chunks[] = {1, 2, 3, 7, 31, 32, 63, 64, 65, 100, 127, 128, 1000, 1024, 4095};
    bool all_match = true;
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        const size_t step = chunks[c];
        sha256_ctx_t ctx;
        sha256_init(&ctx);
        for (size_t off = 0; off < sizeof(data); off += step) {
            size_t n = sizeof(data) - off;
            if (n > step) {
                n = step;
            }
            sha256_update(&ctx, data + off, n);
        }
        uint8_t streamed[32];
        sha256_final(&ctx, streamed);
        if (memcmp(once, streamed, 32) != 0) {
            all_match = false;
        }
    }
    UL_CHECK(all_match, "every chunking of 4096 bytes gives the same digest as one shot");
}

/* Uneven chunking, because a real transfer's last chunk is short and a retry
 * can resend an odd-sized piece. */
static void test_ragged_chunking(void) {
    static uint8_t data[1543]; /* deliberately not a multiple of anything */
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)(i * 31u + 7u);
    }
    uint8_t once[32];
    sha256(data, sizeof(data), once);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    size_t off = 0, step = 1;
    while (off < sizeof(data)) {
        size_t n = sizeof(data) - off;
        if (n > step) {
            n = step;
        }
        sha256_update(&ctx, data + off, n);
        off += n;
        step = step * 2 + 1; /* 1, 3, 7, 15, 31, ... */
    }
    uint8_t streamed[32];
    sha256_final(&ctx, streamed);
    UL_CHECK(memcmp(once, streamed, 32) == 0, "ever-growing ragged chunks still match");
}

/* A zero-length update must change nothing -- a transfer can legitimately
 * deliver an empty piece, and it must not disturb the buffer. */
static void test_empty_updates_are_harmless(void) {
    const char *msg = "abc";
    uint8_t expected[32];
    sha256((const uint8_t *)msg, 3, expected);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)"", 0);
    sha256_update(&ctx, (const uint8_t *)msg, 1);
    sha256_update(&ctx, (const uint8_t *)"", 0);
    sha256_update(&ctx, (const uint8_t *)msg + 1, 2);
    sha256_update(&ctx, (const uint8_t *)"", 0);
    uint8_t out[32];
    sha256_final(&ctx, out);
    UL_CHECK(memcmp(expected, out, 32) == 0, "zero-length updates do not disturb the hash");

    sha256_ctx_t empty;
    sha256_init(&empty);
    sha256_final(&empty, out);
    char hex[65];
    hex_encode(out, sizeof(out), hex, sizeof(hex));
    UL_CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
             "streaming nothing at all gives the empty-string digest");
}

/* The padding boundaries, where a length-encoding bug lives: a message of
 * exactly 55 bytes still fits one block with its padding, 56 does not, and 64
 * is a whole block with none to spare. */
static void test_padding_boundaries(void) {
    static uint8_t buf[200];
    memset(buf, 'a', sizeof(buf));
    const size_t lens[] = {0, 1, 54, 55, 56, 57, 63, 64, 65, 119, 120, 128, 129, 200};
    bool all_match = true;
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        uint8_t once[32], streamed[32];
        sha256(buf, lens[i], once);

        sha256_ctx_t ctx;
        sha256_init(&ctx);
        for (size_t off = 0; off < lens[i]; off++) {
            sha256_update(&ctx, buf + off, 1); /* one byte at a time */
        }
        sha256_final(&ctx, streamed);
        if (memcmp(once, streamed, 32) != 0) {
            all_match = false;
        }
    }
    UL_CHECK(all_match, "every length across the padding boundaries hashes identically");
}

/* An OTA-sized image, hashed the way ota_chunk actually delivers it. Smaller
 * than a real 578KB firmware but past enough block boundaries to be
 * meaningful, and it keeps the suite fast. */
static void test_an_ota_sized_transfer(void) {
    const size_t total = 64 * 1024;
    static uint8_t image[64 * 1024];
    for (size_t i = 0; i < total; i++) {
        image[i] = (uint8_t)((i * 2654435761u) >> 13);
    }
    uint8_t once[32];
    sha256(image, total, once);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    for (size_t off = 0; off < total; off += 1024) { /* OTA_CHUNK_MAX_RAW */
        sha256_update(&ctx, image + off, 1024);
    }
    uint8_t streamed[32];
    sha256_final(&ctx, streamed);
    UL_CHECK(memcmp(once, streamed, 32) == 0,
             "64KB delivered in 1024-byte chunks matches the one-shot digest");
}

void test_sha256_run(void) {
    /* FIPS 180-4 / NIST standard test vectors. */
    check_vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56-byte input: crosses the single-block padding boundary (needs two
     * 64-byte blocks once the 0x80 pad + 8-byte length are appended), the
     * standard NIST multi-block vector. */
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    /* 112-byte input: the other standard NIST multi-block vector, two full
     * blocks before padding. */
    check_vector("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                  "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu",
                  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");

    test_streaming_matches_one_shot();
    test_ragged_chunking();
    test_empty_updates_are_harmless();
    test_padding_boundaries();
    test_an_ota_sized_transfer();
}
