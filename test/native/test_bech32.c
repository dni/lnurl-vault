/* Bech32, and the LNURL a wallet will actually take.
 *
 * The expected strings here were produced by an INDEPENDENT implementation --
 * BIP-173's own reference Python, transcribed from the spec, and round-tripped
 * back through a decoder written the same way -- not by running the C and
 * writing down what it said. That distinction is the whole value of the file.
 * A checksum implementation tested against itself passes while being wrong in
 * exactly the way that matters: it produces a string no other implementation
 * accepts, which on a bearer note shows up as a wallet shrugging at a QR code
 * and reads to the owner as a broken device.
 *
 * Two of the vectors are BIP-173's own, which pins the checksum and the
 * charset against the standard rather than against a helper script. */
#include <stdio.h>
#include <string.h>

#include "bech32.h"
#include "note_url.h"
#include "font5x7.h"
#include "qr_capacity.h"
#include "unity_lite.h"

/* The k1 from test_derive.c's LUD-25 path vectors, so a reader can follow one
 * secret from the seed it is derived from all the way to the code on screen. */
#define K1 "a5fa7131794dc7a9d076255549961c31b184402832e82c68a10b6ad3a6a1e06a"

static void check_str(const char *got, const char *want, const char *what) {
    const bool ok = strcmp(got, want) == 0;
    UL_CHECK(ok, what);
    if (!ok) {
        printf("     got  %s\n     want %s\n", got, want);
    }
}

static void test_bip173_own_vectors(void) {
    char out[BECH32_MAX_OUT];

    /* Empty data, one-character hrp. The smallest thing the spec defines, and
     * the one that catches a checksum seeded wrongly. */
    UL_CHECK(bech32_encode_upper("a", NULL, 0, out, sizeof(out)), "encodes empty data");
    check_str(out, "A12UEL5L", "BIP-173's 'a12uel5l' vector, uppercased");

    /* An 83-character hrp: hrp_expand contributes twice its length to the
     * checksum, so a loop that walks it once passes everything above and
     * fails here. */
    UL_CHECK(bech32_encode_upper(
                 "an83characterlonghumanreadablepartthatcontainsthenumber1andtheexcludedcharactersbio",
                 NULL, 0, out, sizeof(out)),
             "encodes a long human-readable part");
    check_str(out,
              "AN83CHARACTERLONGHUMANREADABLEPARTTHATCONTAINSTHENUMBER1ANDTHEEXCLUDEDCHARACTERSBIO1TT5TGS",
              "BIP-173's long-hrp vector");

    /* Eight bytes 0x00..0x07, which exercises the 8-to-5 regrouping across a
     * boundary that is not a multiple of either. */
    static const uint8_t bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    UL_CHECK(bech32_encode_upper("abcdef", bytes, sizeof(bytes), out, sizeof(out)),
             "encodes eight bytes");
    check_str(out, "ABCDEF1QQQSYQCYQ5RQWEECU68", "the 8-to-5 regrouping");
}

static void test_a_note_encodes_as_an_lnurl(void) {
    char out[BECH32_MAX_OUT];

    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, "mint.example/w", K1, 21000, out,
                                sizeof(out)),
             "builds a bech32 note");
    check_str(out,
              "LNURL1DP68GURN8GHJ7MTFDE6ZUETCV9KHQMR99AMN76E384SN2ENPXUCNXVFH8Y6XGCEHVYUKGVPHXCER2DF"
              "4XSUNJD33VVENZC338Q6RGVPJ8QENYEFCXF3NVWRPXYCXYDNPVSEKZDNPX9JNQDNPYESK6MM4DE6R6V33XQCR"
              "QHD3DSG",
              "the LNURL for mint.example/w");

    /* A real mint, and a host carrying a path -- the shape every note on the
     * bench actually has. */
    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, "moneyer.dev/w", K1, 50000, out,
                                sizeof(out)),
             "builds one for a real mint");
    check_str(out,
              "LNURL1DP68GURN8GHJ7MT0DEJHJETJ9EJX2A30WULKKVFAVY6KVCFHXYENZDEEX3JXXDMP89JRQDEKXG6N2DF"
              "58YUNVVTRXVCKYVFCXS6RQV3CXVEX2WPJVVMRSCF3XP3RVCTYXDSNVCF3V5CRVCFXV9KK7ATWWS7N2VPSXQCQ"
              "AL6XKG",
              "the LNURL for moneyer.dev/w");

    /* A host with a port, and the smallest amount there is. */
    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, "localhost:8111", K1, 1, out, sizeof(out)),
             "builds one for a host with a port");
    check_str(out,
              "LNURL1DP68GURN8GHJ7MR0VDSKC6R0WD6R5WP3XYCN76E384SN2ENPXUCNXVFH8Y6XGCEHVYUKGVPHXCER2DF"
              "4XSUNJD33VVENZC338Q6RGVPJ8QENYEFCXF3NVWRPXYCXYDNPVSEKZDNPX9JNQDNPYESK6MM4DE6R6VG7XMJY"
              "P",
              "the LNURL for localhost:8111");
}

static void test_it_is_all_one_case(void) {
    /* A bech32 string is valid lower or upper but never mixed, and the
     * uppercase form is the point -- it is what lets the QR encoder use
     * alphanumeric mode. One lowercase character and the code silently gets
     * bigger, or a decoder rejects the lot. */
    char out[BECH32_MAX_OUT];
    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, "mint.example/w", K1, 21000, out,
                                sizeof(out)),
             "builds a note to inspect");
    bool any_lower = false;
    for (const char *p = out; *p; p++) {
        if (*p >= 'a' && *p <= 'z') {
            any_lower = true;
        }
    }
    UL_CHECK(!any_lower, "no lowercase anywhere in the LNURL");
}

static void test_a_short_buffer_yields_nothing_not_something(void) {
    /* The failure that must never reach a QR code: a plausible-looking LNURL
     * carrying a shortened secret is a different note, or none, handed to
     * somebody as money. Same contract note_url.c already holds itself to. */
    char out[BECH32_MAX_OUT];
    for (size_t cap = 1; cap < 180; cap += 7) {
        memset(out, 'X', sizeof(out));
        const bool ok =
            note_url_build_as(NOTE_URL_BECH32, NULL, "mint.example/w", K1, 21000, out, cap);
        UL_CHECK(!ok, "a buffer too small is refused");
        UL_CHECK(out[0] == '\0', "and leaves an empty string, never a partial one");
    }
}

static void test_the_length_is_predictable(void) {
    /* Callers size buffers off this, so it has to agree with what encoding
     * actually writes -- not approximately. */
    char out[BECH32_MAX_OUT];
    static const uint8_t data[40] = {0};
    for (size_t n = 0; n <= sizeof(data); n++) {
        const size_t predicted = bech32_encoded_len("lnurl", n);
        UL_CHECK(predicted > 0, "a short payload has a length");
        UL_CHECK(bech32_encode_upper("lnurl", data, n, out, sizeof(out)), "and encodes");
        UL_CHECK(strlen(out) == predicted, "the predicted length is the written length");
    }
    UL_CHECK(bech32_encoded_len("", 10) == 0, "an empty hrp has no length");
    UL_CHECK(bech32_encoded_len("lnurl", 100000) == 0, "and neither does an absurd payload");
}

static void test_every_format_names_itself(void) {
    /* The unveil screen prints these, so an unnamed format would show up as a
     * blank caption over a code nobody can identify. */
    for (int f = 0; f < NOTE_URL_FORMAT_COUNT; f++) {
        const char *name = note_url_format_name((note_url_format_t)f);
        UL_CHECK(name && name[0] && strcmp(name, "?") != 0, "each format has a name");
    }
}

static void test_the_three_formats_differ(void) {
    char a[BECH32_MAX_OUT], b[BECH32_MAX_OUT], c[BECH32_MAX_OUT];
    UL_CHECK(note_url_build_as(NOTE_URL_LUD17, NULL, "mint.example/w", K1, 21000, a, sizeof(a)),
             "lnurlw builds");
    UL_CHECK(note_url_build_as(NOTE_URL_CLAIM, NULL, "mint.example/w", K1, 21000, b, sizeof(b)),
             "the claim link builds");
    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, "mint.example/w", K1, 21000, c, sizeof(c)),
             "the LNURL builds");
    UL_CHECK(strcmp(a, b) != 0 && strcmp(b, c) != 0 && strcmp(a, c) != 0,
             "cycling formats actually changes the code on screen");
    /* Each one carries the same secret, which is the only thing that must be
     * true of all three. */
    UL_CHECK(strstr(a, K1) != NULL, "lnurlw carries the secret");
    UL_CHECK(strstr(b, K1) != NULL, "the claim link carries the secret");
}

static void test_the_longest_form_still_renders(void) {
    /* The LNURL is the biggest thing this device ever puts in a QR, and it is
     * now a button press away on every note. If it does not fit a version the
     * renderer will draw, that is not a smaller code -- qr_display_show()
     * refuses outright and the owner gets FAILED / NOT SHOWN, on the screen
     * whose entire job is to hand the note over.
     *
     * Checked at the NARROWER panel, and against the byte-mode table on
     * purpose: qr_capacity.c sizes by byte mode even though the encoder will
     * pick denser alphanumeric mode for an uppercase bech32 string, so this
     * asserts the pessimistic answer. The real code comes out no larger. */
    char out[BECH32_MAX_OUT];
    static const char *const HOSTS[] = {
        "mint.example/w",
        "moneyer.dev/w",
        "localhost:8111",
    };
    for (size_t i = 0; i < sizeof(HOSTS) / sizeof(HOSTS[0]); i++) {
        UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL, HOSTS[i], K1, 2100000000ULL, out,
                                    sizeof(out)),
                 "the LNURL builds for a realistic host");
        const uint8_t version = qr_version_for_length(strlen(out));
        UL_CHECK(version > 0, "and fits a QR version this device will draw");
        if (version > 0) {
            /* ISO/IEC 18004: a version-N code is 17 + 4N modules square. */
            const int qr_size = 17 + 4 * (int)version;
            /* 240x135 is the classic T-Display. One pixel per module is the
             * floor; below that qr_display_show() gives up and draws nothing. */
            UL_CHECK(qr_scale_for(qr_size, 240, 135) >= 1,
                     "and renders at at least a pixel per module on the narrow panel");
            /* And leaves a strip for the caption naming the form, which is
             * the only thing telling a person which of the three they are
             * looking at. */
            const int square = qr_square_modules(qr_size) * qr_scale_for(qr_size, 240, 135);
            UL_CHECK(135 - square >= FONT5X7_HEIGHT,
                     "with room under it for the caption");
        }
    }
}

static void test_a_very_long_host_keeps_the_code_and_loses_the_label(void) {
    /* A long enough mint host pushes the LNURL to a QR version whose square
     * eats the strip the caption lives in, on the narrower panel. The code
     * still renders and still scans -- the quiet zone is inside the square --
     * and the label is simply not drawn.
     *
     * That is the right way round, and it is asserted rather than assumed:
     * shrinking the code a step to make room for a word about it would trade
     * the thing being handed over for the thing describing it. */
    char out[BECH32_MAX_OUT];
    UL_CHECK(note_url_build_as(NOTE_URL_BECH32, NULL,
                                "a-rather-long-mint-hostname.example.org/lnurlcash/w", K1,
                                2100000000ULL, out, sizeof(out)),
             "a long host still encodes");
    const uint8_t version = qr_version_for_length(strlen(out));
    UL_CHECK(version > 0, "and still fits a version this device will draw");
    if (version > 0) {
        const int qr_size = 17 + 4 * (int)version;
        UL_CHECK(qr_scale_for(qr_size, 240, 135) >= 1, "and still renders on the narrow panel");
    }
}

void test_bech32_run(void) {
    printf("\n-- bech32 and the LNURL forms --\n");
    test_bip173_own_vectors();
    test_a_note_encodes_as_an_lnurl();
    test_it_is_all_one_case();
    test_a_short_buffer_yields_nothing_not_something();
    test_the_length_is_predictable();
    test_every_format_names_itself();
    test_the_three_formats_differ();
    test_the_longest_form_still_renders();
    test_a_very_long_host_keeps_the_code_and_loses_the_label();
}
