/* Seed-recoverable note secrets (LUD-25). The scheme's whole value is that a
 * secret derived on one device reproduces byte-for-byte on another from the
 * seed alone -- so these tests are mostly fixed vectors, and a change that
 * moves any of them silently strands every note derived under the old rule.
 *
 * The BIP-32 vectors below are the published ones from the BIP itself, not
 * values this implementation produced. They are the only check here that
 * catches "self-consistent but not BIP-32". */
#include <stdio.h>
#include <string.h>

#include "derive.h"
#include "hex.h"
#include "sha256.h"
#include "unity_lite.h"

static void from_hex(const char *hex, uint8_t *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned int byte = 0;
        sscanf(hex + i * 2, "%2x", &byte);
        out[i] = (uint8_t)byte;
    }
}

static bool hex_is(const uint8_t *bytes, size_t len, const char *expect) {
    char buf[131];
    if (len * 2 + 1 > sizeof(buf)) {
        return false;
    }
    hex_encode(bytes, len, buf, sizeof(buf));
    return strcmp(buf, expect) == 0;
}

/* BIP-32 test vector 1. */
static const char *TV1_SEED = "000102030405060708090a0b0c0d0e0f";
static const char *TV1_M_KEY = "e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35";
static const char *TV1_M_CHAIN = "873dff81c02f525623fd1fe5167eac3a55a049de3d314bb42ee227ffed37d508";
static const char *TV1_M0H_KEY = "edb2e14f9ee77d26dd93b4ecede8d16ed408ce149b6cd80b0715a2d911a0afea";
static const char *TV1_M0H_CHAIN =
    "47fdacbd0f1097043b78c63c20c34ef4ed9a111d980047ad16282c7ae6236141";

/* BIP-32 test vector 3, whose master key begins with a zero byte. It exists in
 * the BIP precisely because implementations that treat the key as a number and
 * re-serialise it without left-padding to 32 bytes get it wrong. */
static const char *TV3_SEED =
    "4b381541583be4423346c643850da4b320e46a87ae3d2a4e6da11eba819cd4acba45d239319ac14f863b8"
    "d5ab5a0d0c64d2e8a1e7d1457df2e5a3c51c73235be";
static const char *TV3_M_KEY = "00ddb80b067e0d4993197fe10f2657a844a384589847602d56f0c629c81aae32";
static const char *TV3_M0H_KEY =
    "491f7a2eebc7b57028e0d3faa0acda02e75c33b03c48fb288c41e2ea44e1daef";

static void test_master_matches_bip32(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m;
    UL_CHECK(derive_master(seed, sizeof(seed), &m), "master derives");
    UL_CHECK(hex_is(m.key, sizeof(m.key), TV1_M_KEY), "BIP-32 TV1 master key");
    UL_CHECK(hex_is(m.chain_code, sizeof(m.chain_code), TV1_M_CHAIN), "BIP-32 TV1 chain code");
    derive_wipe(&m);
}

static void test_hardened_child_matches_bip32(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m, child;
    derive_master(seed, sizeof(seed), &m);
    UL_CHECK(derive_child_hardened(&m, 0, &child), "m/0' derives");
    UL_CHECK(hex_is(child.key, sizeof(child.key), TV1_M0H_KEY), "BIP-32 TV1 m/0' key");
    UL_CHECK(hex_is(child.chain_code, sizeof(child.chain_code), TV1_M0H_CHAIN),
             "BIP-32 TV1 m/0' chain code");
    derive_wipe(&m);
    derive_wipe(&child);
}

/* A key whose leading byte is zero must serialise as 32 bytes, not 31. */
static void test_a_leading_zero_key_keeps_its_width(void) {
    uint8_t seed[64];
    from_hex(TV3_SEED, seed, sizeof(seed));
    derive_xprv_t m, child;
    UL_CHECK(derive_master(seed, sizeof(seed), &m), "TV3 master derives");
    UL_CHECK(hex_is(m.key, sizeof(m.key), TV3_M_KEY), "BIP-32 TV3 master key keeps its zero byte");
    UL_CHECK(derive_child_hardened(&m, 0, &child), "TV3 m/0' derives");
    UL_CHECK(hex_is(child.key, sizeof(child.key), TV3_M0H_KEY), "BIP-32 TV3 m/0' key");
    derive_wipe(&m);
    derive_wipe(&child);
}

/* An index that already carries the hardening bit is a caller error, not a
 * different child: 0x80000000 | 0 is how this module writes m/0', so accepting
 * it would derive m/0' when the caller wrote m/2147483648'. */
static void test_a_prehardened_index_is_refused(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m, child;
    derive_master(seed, sizeof(seed), &m);
    UL_CHECK(!derive_child_hardened(&m, 0x80000000u, &child), "0x80000000 refused");
    UL_CHECK(!derive_child_hardened(&m, 0xFFFFFFFFu, &child), "0xFFFFFFFF refused");
    uint8_t secret[DERIVE_SECRET_LEN];
    UL_CHECK(!derive_note_secret(&m, (const uint8_t *)"mint.example", 12, 0x80000000u, secret),
             "and refused at the note-secret entry point too");
    derive_wipe(&m);
}

static void test_seed_length_bounds(void) {
    uint8_t seed[DERIVE_SEED_MAX_LEN + 1];
    memset(seed, 0x42, sizeof(seed));
    derive_xprv_t m;
    UL_CHECK(!derive_master(seed, DERIVE_SEED_MIN_LEN - 1, &m), "too short refused");
    UL_CHECK(!derive_master(seed, DERIVE_SEED_MAX_LEN + 1, &m), "too long refused");
    UL_CHECK(derive_master(seed, DERIVE_SEED_MIN_LEN, &m), "minimum accepted");
    derive_wipe(&m);
    UL_CHECK(derive_master(seed, DERIVE_SEED_MAX_LEN, &m), "maximum accepted");
    derive_wipe(&m);
}

/* out may alias parent -- derive_note_secret walks the path in place. */
static void test_child_may_derive_in_place(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m, separate;
    derive_master(seed, sizeof(seed), &m);
    derive_child_hardened(&m, 0, &separate);
    derive_child_hardened(&m, 0, &m); /* same step, in place */
    UL_CHECK(memcmp(m.key, separate.key, DERIVE_KEY_LEN) == 0, "in-place key matches");
    UL_CHECK(memcmp(m.chain_code, separate.chain_code, DERIVE_KEY_LEN) == 0,
             "in-place chain code matches");
    derive_wipe(&m);
    derive_wipe(&separate);
}

/* ---- the LUD-25 path itself ---- */

/* Generated from an independent Python implementation of the same scheme
 * (hmac + sha512 + mod-n addition, no C involved), so a match here is two
 * implementations agreeing rather than one repeating itself. Seed is BIP-32
 * TV1's, for a vector anyone can reproduce. */
static const char *DOM_A = "mint.example";
static const char *DOM_A_SECRET_0 =
    "a5fa7131794dc7a9d076255549961c31b184402832e82c68a10b6ad3a6a1e06a";
static const char *DOM_A_SECRET_1 =
    "588e2ef6c2095b90cdbf68117be2cab71dec61667a3b042e3050cf9c67338bf4";
static const char *DOM_A_SECRET_7 =
    "7e15f95951fdc51ef73597797d9d9b52e2e4607f185e9c85159d96098523672d";
static const char *DOM_A_H_0 = "a4493e2fc8cffe96a191c75dba00ec8b884cbc8a6d30be410eb08005cc2f0d45";

/* The withdraw endpoint's host AND path: a note is the whole URL, so two mints
 * behind one hostname must not share a tree. */
static const char *DOM_B = "mint.example/w";
static const char *DOM_B_SECRET_0 =
    "52dfc5b5edd1c9c424f0807e0e94e8a9c3e6601c7282765be8d5054c7bbce290";

/* A dev host with a port and no dot -- the documented local loop. */
static const char *DOM_C = "localhost:8111";
static const char *DOM_C_SECRET_0 =
    "a3c6b376bced044e98bea405de76508407aa4ecb388a2089b3c19f56cbc7565b";

static void secret_for(const char *domain, uint32_t index, uint8_t out[DERIVE_SECRET_LEN]) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m;
    derive_master(seed, sizeof(seed), &m);
    derive_note_secret(&m, (const uint8_t *)domain, strlen(domain), index, out);
    derive_wipe(&m);
}

static void test_note_secrets_match_the_reference(void) {
    uint8_t s[DERIVE_SECRET_LEN];
    secret_for(DOM_A, 0, s);
    UL_CHECK(hex_is(s, sizeof(s), DOM_A_SECRET_0), "mint.example secret_0");
    secret_for(DOM_A, 1, s);
    UL_CHECK(hex_is(s, sizeof(s), DOM_A_SECRET_1), "mint.example secret_1");
    secret_for(DOM_A, 7, s);
    UL_CHECK(hex_is(s, sizeof(s), DOM_A_SECRET_7), "mint.example secret_7");
    secret_for(DOM_B, 0, s);
    UL_CHECK(hex_is(s, sizeof(s), DOM_B_SECRET_0), "mint.example/w secret_0");
    secret_for(DOM_C, 0, s);
    UL_CHECK(hex_is(s, sizeof(s), DOM_C_SECRET_0), "localhost:8111 secret_0");
}

/* What the mint is told is sha256 of the secret, exactly as it would be for a
 * random one -- the derivation is invisible on the wire. */
static void test_the_disclosed_hash_is_plain_sha256(void) {
    uint8_t s[DERIVE_SECRET_LEN], h[32];
    secret_for(DOM_A, 0, s);
    sha256(s, sizeof(s), h);
    UL_CHECK(hex_is(h, sizeof(h), DOM_A_H_0), "h = sha256(secret_0)");
}

/* The point of the whole exercise: the same seed reproduces the same secret,
 * and a different index or a different mint does not collide. */
static void test_derivation_is_reproducible_and_separated(void) {
    uint8_t a[DERIVE_SECRET_LEN], b[DERIVE_SECRET_LEN];
    secret_for(DOM_A, 3, a);
    secret_for(DOM_A, 3, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) == 0, "same seed, same domain, same index reproduces");
    secret_for(DOM_A, 4, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) != 0, "a different index differs");
    secret_for(DOM_B, 3, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) != 0, "a different endpoint differs");
}

static void test_a_different_seed_gives_a_different_tree(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    seed[15] ^= 0x01u;
    derive_xprv_t m;
    derive_master(seed, sizeof(seed), &m);
    uint8_t other[DERIVE_SECRET_LEN], ours[DERIVE_SECRET_LEN];
    derive_note_secret(&m, (const uint8_t *)DOM_A, strlen(DOM_A), 0, other);
    derive_wipe(&m);
    secret_for(DOM_A, 0, ours);
    UL_CHECK(memcmp(other, ours, sizeof(ours)) != 0, "one flipped seed bit changes the secret");
}

/* Domain material is what separates one mint's tree from another's; the four
 * levels must be in hardened range because every level of this path is
 * hardened. */
static void test_domain_indices_are_in_hardened_range(void) {
    uint8_t seed[16];
    from_hex(TV1_SEED, seed, sizeof(seed));
    derive_xprv_t m;
    derive_master(seed, sizeof(seed), &m);
    const char *domains[] = {DOM_A, DOM_B, DOM_C, "a", "xn--bcher-kva.example"};
    for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++) {
        derive_domain_t d;
        UL_CHECK(derive_domain(&m, (const uint8_t *)domains[i], strlen(domains[i]), &d),
                 "domain derives");
        for (size_t j = 0; j < 4; j++) {
            UL_CHECK(d.d[j] < 0x80000000u, "index fits a hardened level");
        }
    }
    derive_domain_t d;
    UL_CHECK(!derive_domain(&m, (const uint8_t *)"", 0, &d), "an empty domain is refused");
    derive_wipe(&m);
}

void test_derive_run(void) {
    printf("-- derive --\n");
    test_master_matches_bip32();
    test_hardened_child_matches_bip32();
    test_a_leading_zero_key_keeps_its_width();
    test_a_prehardened_index_is_refused();
    test_seed_length_bounds();
    test_child_may_derive_in_place();
    test_note_secrets_match_the_reference();
    test_the_disclosed_hash_is_plain_sha256();
    test_derivation_is_reproducible_and_separated();
    test_a_different_seed_gives_a_different_tree();
    test_domain_indices_are_in_hardened_range();
}
