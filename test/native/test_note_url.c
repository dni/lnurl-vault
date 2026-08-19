/* The lnurlw:// URL that carries a note's secret.
 *
 * This is the string that goes into the QR code someone is handed as money, so
 * it has exactly two acceptable outcomes: complete and correct, or empty. A
 * partial URL is the dangerous case -- it does not look broken, it looks like
 * a valid lnurlw:// carrying a shortened k1, which is a different secret or
 * none, and a caller that forgot to check the return would render it and hand
 * it over.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "note_url.h"
#include "qr_capacity.h"
#include "unity_lite.h"
#include "vault.h" /* VAULT_SECRET_HEX_BUF, VAULT_HOST_BUF */

/* A realistic secret: 64 hex characters, as vault_export_secret produces. */
static const char *K1_64 =
    "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";

static void test_the_expected_shape(void) {
    char out[256];
    UL_CHECK(note_url_build("mint.example/w", "aa11bb22", 21000, out, sizeof(out)),
             "builds a URL from host + secret + amount");
    UL_CHECK(strcmp(out, "lnurlw://mint.example/w?k1=aa11bb22&amount=21000") == 0,
             "URL has the exact expected shape");
}

static void test_rejects_missing_parts(void) {
    char out[256];
    UL_CHECK(!note_url_build("", "aa11bb22", 21000, out, sizeof(out)), "rejects an empty host");
    UL_CHECK(!note_url_build("mint.example/w", "", 21000, out, sizeof(out)),
             "rejects an empty secret");
    UL_CHECK(!note_url_build(NULL, "aa11bb22", 21000, out, sizeof(out)), "rejects a NULL host");
    UL_CHECK(!note_url_build("mint.example/w", NULL, 21000, out, sizeof(out)),
             "rejects a NULL secret");
    UL_CHECK(!note_url_build("mint.example/w", "aa11bb22", 21000, NULL, 16),
             "rejects a NULL buffer rather than writing through it");
    UL_CHECK(!note_url_build("mint.example/w", "aa11bb22", 21000, out, 0),
             "rejects a zero-length buffer");
}

/* THE one that matters. Every failure must leave an empty string, never a
 * partial URL that could be rendered and handed over. */
static void test_every_failure_leaves_an_empty_string(void) {
    char out[64];
    bool always_empty = true;

    /* Walk every buffer size from far too small to just barely too small, so
     * truncation is exercised at every possible cut point -- including cuts
     * that land inside the secret, which is the dangerous one. */
    for (size_t cap = 1; cap < sizeof(out); cap++) {
        memset(out, 'X', sizeof(out));
        if (!note_url_build("mint.example/w", K1_64, 21000, out, cap)) {
            if (out[0] != '\0') {
                always_empty = false;
            }
        }
    }
    UL_CHECK(always_empty, "a buffer too small always yields an empty string, never a fragment");

    /* And the argument-rejection paths, which never reach snprintf. */
    memset(out, 'X', sizeof(out));
    note_url_build("", K1_64, 1, out, sizeof(out));
    UL_CHECK(out[0] == '\0', "an empty host clears the buffer too");

    memset(out, 'X', sizeof(out));
    note_url_build("mint.example/w", "", 1, out, sizeof(out));
    UL_CHECK(out[0] == '\0', "so does an empty secret");
}

/* A truncated URL must never be mistaken for a whole one: if it succeeded, the
 * complete secret is present; if it failed, nothing is. There is no in
 * between, and in particular no prefix of the secret is ever left behind. */
static void test_the_secret_is_whole_or_absent(void) {
    char out[64];
    bool never_partial = true;

    for (size_t cap = 1; cap < sizeof(out); cap++) {
        memset(out, 'X', sizeof(out));
        const bool ok = note_url_build("m/w", K1_64, 1, out, cap);
        if (ok) {
            if (strstr(out, K1_64) == NULL) {
                never_partial = false; /* claimed success without the full secret */
            }
        } else if (out[0] != '\0') {
            never_partial = false; /* failed but left something behind */
        }
    }
    UL_CHECK(never_partial, "success always carries the whole secret; failure carries nothing");
}

/* The buffer ui_task.c actually uses, with the largest host and secret the
 * vault can hold. If this does not fit, unveiling a legitimate note silently
 * shows nothing, which is a bug worth knowing about before a user finds it. */
static void test_the_real_worst_case_fits(void) {
    char host[VAULT_HOST_BUF];
    memset(host, 'h', sizeof(host) - 1);
    host[sizeof(host) - 1] = '\0';

    char out[256]; /* the size unveil() declares */
    const bool ok = note_url_build(host, K1_64, UINT64_MAX, out, sizeof(out));
    UL_CHECK(ok, "the largest host, a full-length secret and the largest amount all fit in 256");
    if (ok) {
        UL_CHECK(strstr(out, K1_64) != NULL, "with the secret intact");
        UL_CHECK(strncmp(out, "lnurlw://", 9) == 0, "and the scheme intact");
    }
}

static void test_amount_edges(void) {
    char out[256];
    UL_CHECK(note_url_build("m/w", "ab", 0, out, sizeof(out)) &&
                 strcmp(out, "lnurlw://m/w?k1=ab&amount=0") == 0,
             "a zero amount is rendered as 0, not omitted");

    UL_CHECK(note_url_build("m/w", "ab", UINT64_MAX, out, sizeof(out)) &&
                 strstr(out, "amount=18446744073709551615") != NULL,
             "the largest amount is rendered without wrapping");
}

/* The parts must not run together: a decoder splitting on ? and & has to find
 * exactly the fields it expects, and a missing separator would fold the secret
 * into the host or the amount into the secret. */
static void test_the_separators_are_present_exactly_once(void) {
    char out[256];
    note_url_build("mint.example/w", K1_64, 21000, out, sizeof(out));

    int q = 0, amp = 0;
    for (const char *p = out; *p; p++) {
        if (*p == '?') {
            q++;
        }
        if (*p == '&') {
            amp++;
        }
    }
    UL_CHECK(q == 1, "exactly one '?' separates the endpoint from the query");
    UL_CHECK(amp == 1, "exactly one '&' separates k1 from amount");
    UL_CHECK(strstr(out, "?k1=") != NULL, "k1 is the first query parameter");
    UL_CHECK(strstr(out, "&amount=") != NULL, "amount is the second");
}

/* ---- the QR a stranger has to be able to use (issue #26) --------------- */

/* lnurlw:// codes render and phone cameras decode them, and nothing opens
 * them: no handler exists on a stock phone. A bearer note that cannot be
 * handed to someone with a phone is not a bearer note. */
static void test_claim_format_is_an_ordinary_https_link(void) {
    char out[256];
    bool ok =
        note_url_build_as(NOTE_URL_CLAIM, NULL, "mint.example", K1_64, 21000, out, sizeof(out));
    UL_CHECK(ok, "the claim URL builds");
    UL_CHECK(strncmp(out, "https://", 8) == 0, "and is a link a camera will open");
    UL_CHECK(strstr(out, "u=mint.example") != NULL, "carrying the mint it came from");
    UL_CHECK(strstr(out, K1_64) != NULL, "and the secret");
}

/* The property that matters. Everything after the first '#' is a fragment,
 * and a fragment is not sent in the request line: no server log, no referrer,
 * no proxy sees it. The secret must land on that side of the '#'. */
static void test_the_secret_never_precedes_the_fragment(void) {
    char out[256];
    UL_CHECK(
        note_url_build_as(NOTE_URL_CLAIM, NULL, "mint.example", K1_64, 21000, out, sizeof(out)),
        "built");
    const char *hash = strchr(out, '#');
    const char *secret = strstr(out, K1_64);
    UL_CHECK(hash != NULL, "there is a fragment");
    UL_CHECK(secret != NULL && hash != NULL && secret > hash, "and the secret is inside it");
}

/* LUD-17 is still the smaller code and what LNURL-native wallets want.
 * Changing the default must not remove the option. */
static void test_lud17_format_is_unchanged(void) {
    char via_as[256], direct[256];
    UL_CHECK(note_url_build_as(NOTE_URL_LUD17, NULL, "mint.example", K1_64, 21000, via_as,
                                sizeof(via_as)),
              "built via note_url_build_as");
    UL_CHECK(note_url_build("mint.example", K1_64, 21000, direct, sizeof(direct)), "built direct");
    UL_CHECK(strcmp(via_as, direct) == 0, "the LUD-17 path is byte-identical");
}

/* Same contract as note_url_build: empty, never partial. A truncated claim
 * URL is a plausible link carrying a shortened k1. */
static void test_a_claim_url_that_does_not_fit_is_emptied(void) {
    char out[32];
    UL_CHECK(
        !note_url_build_as(NOTE_URL_CLAIM, NULL, "mint.example", K1_64, 21000, out, sizeof(out)),
        "refused");
    UL_CHECK(out[0] == '\0', "and left empty, never partially written");
}

/* The format has to fit on the glass, not just in the buffer. A code the
 * panel cannot render is not a handoff, so tie the payload length to the
 * smallest screen with a bench record: the classic T-Display, 135px short. */
static void test_the_claim_url_still_fits_the_screens_we_have(void) {
    char out[256];
    UL_CHECK(note_url_build_as(NOTE_URL_CLAIM, NULL, "mint.lnurlcash.com", K1_64, 2100000000ULL,
                                out, sizeof(out)),
              "built at a realistic host and amount");

    uint8_t version = qr_version_for_length(strlen(out));
    UL_CHECK(version != 0, "it fits a QR code at all");

    const int qr_size = 17 + 4 * (int)version;
    UL_CHECK(qr_square_modules(qr_size) > 0, "with a sane module count");
    UL_CHECK(qr_scale_for(qr_size, 240, 135) >= 2,
              "and renders at two pixels per module on a 135px panel");
}

void test_note_url_run(void) {
    printf("-- note_url --\n");
    test_the_expected_shape();
    test_rejects_missing_parts();
    test_every_failure_leaves_an_empty_string();
    test_the_secret_is_whole_or_absent();
    test_the_real_worst_case_fits();
    test_amount_edges();
    test_the_separators_are_present_exactly_once();
    test_claim_format_is_an_ordinary_https_link();
    test_the_secret_never_precedes_the_fragment();
    test_lud17_format_is_unchanged();
    test_a_claim_url_that_does_not_fit_is_emptied();
    test_the_claim_url_still_fits_the_screens_we_have();
}
