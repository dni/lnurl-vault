/* Seed-recoverable note secrets (LUD-25). The scheme's whole value is that a
 * secret derived on one device reproduces byte-for-byte on another from the
 * seed alone -- so these tests are mostly fixed vectors, and a change that
 * moves any of them silently strands every note derived under the old rule.
 *
 * The BIP-32 vectors below are the published ones from the BIP itself, and the
 * LUD-25 ones come from lnurlcash-conformance's cash-derivation.json. Neither
 * set is a value this implementation produced, which is the only thing that
 * catches "self-consistent but not the scheme everyone else implements". */
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

/* Straight out of lnurlcash-conformance 0.7.0's cash-derivation.json, which
 * derives them from the BIP39 seed for "abandon abandon ... about" through
 * m/139' and the four domain levels. This device never walks that part -- see
 * derive.h -- so what it is given is the domain node those levels end at, and
 * what it has to reproduce is every secret beneath it. A mismatch here means
 * this vault and every conforming wallet disagree about where the money is.
 *
 * The `h` values are the vector's own noteId, i.e. what the mint is told. */
static const char *NODE_A =
    "72056e5cde21458b13689c3950904dfd327415a506d064290e5e5f4296a40543"
    "dd5e9504ddb6eefbafa4ad3b00ad421858fafc0ed9ea4abd9cb68793f845cfc1";
static const char *NODE_A_SECRET_0 =
    "de5b81405a12e1297b350d80e2ad85043ed5b9436a0c5592d3302778de330499";
static const char *NODE_A_SECRET_1 =
    "267570df5ba8098d728e839a698f729c2df6fa7b8b7ae7c9c7ffa7dda3417e1d";
/* One past a 20-index gap limit: the rung a wallet reaches only by keeping its
 * own counter, since no scan can see the burned ones below it. */
static const char *NODE_A_SECRET_20 =
    "1f2b8080d4c9431b1dec780abb4eee3c4aaf7ff03f7dd126a7e005ec7af372e0";
static const char *NODE_A_H_0 =
    "7db9da2845cd45c1c3c2e302d6135da46823e245f756b830ef59ac324b769e02";

/* A second mint, on a host carrying a port -- the documented local loop, and
 * the case where two implementations most easily disagree about the host
 * string. That disagreement cannot reach this device: the string is hashed on
 * the provisioning side and arrives here already resolved to a node. */
static const char *NODE_B =
    "80bdd2d71f0235bd9e22d5838a4a4f346cf5415309ada559942a409a630e102c"
    "e214425b89b36d03d4835fdf6592d2619c74d37b746711b4ed85759ea1d358a6";
static const char *NODE_B_SECRET_0 =
    "d7bcc5c9e7015ca2688ed10e24db3f82163d5b59fc887e5dd346abf2426b1270";

static void secret_at(const char *node_hex, uint32_t index, uint8_t out[DERIVE_SECRET_LEN]) {
    uint8_t bytes[DERIVE_NODE_LEN];
    from_hex(node_hex, bytes, sizeof(bytes));
    derive_xprv_t node;
    UL_CHECK(derive_node_import(bytes, &node), "node imports");
    UL_CHECK(derive_note_secret(&node, index, out), "secret derives");
    derive_wipe(&node);
}

static void test_note_secrets_match_the_reference(void) {
    uint8_t s[DERIVE_SECRET_LEN];
    secret_at(NODE_A, 0, s);
    UL_CHECK(hex_is(s, sizeof(s), NODE_A_SECRET_0), "mint.example secret_0");
    secret_at(NODE_A, 1, s);
    UL_CHECK(hex_is(s, sizeof(s), NODE_A_SECRET_1), "mint.example secret_1");
    secret_at(NODE_A, 20, s);
    UL_CHECK(hex_is(s, sizeof(s), NODE_A_SECRET_20), "mint.example secret_20");
    secret_at(NODE_B, 0, s);
    UL_CHECK(hex_is(s, sizeof(s), NODE_B_SECRET_0), "127.0.0.1:8899 secret_0");
}

/* What the mint is told is sha256 of the secret, exactly as it would be for a
 * random one -- the derivation is invisible on the wire. */
static void test_the_disclosed_hash_is_plain_sha256(void) {
    uint8_t s[DERIVE_SECRET_LEN], h[32];
    secret_at(NODE_A, 0, s);
    sha256(s, sizeof(s), h);
    UL_CHECK(hex_is(h, sizeof(h), NODE_A_H_0), "h = sha256(secret_0)");
}

/* The point of the whole exercise: the same node reproduces the same secret,
 * and a different index or a different mint does not collide. */
static void test_derivation_is_reproducible_and_separated(void) {
    uint8_t a[DERIVE_SECRET_LEN], b[DERIVE_SECRET_LEN];
    secret_at(NODE_A, 3, a);
    secret_at(NODE_A, 3, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) == 0, "same node and index reproduces");
    secret_at(NODE_A, 4, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) != 0, "a different index differs");
    secret_at(NODE_B, 3, b);
    UL_CHECK(memcmp(a, b, sizeof(a)) != 0, "a different mint differs");
}

/* A node is provisioning material for one mint's whole subtree, so a corrupt
 * or fabricated blob must be refused rather than quietly derived from: every
 * secret beneath a wrong node is money the provisioning wallet cannot find. */
static void test_a_malformed_node_is_refused(void) {
    derive_xprv_t node;
    uint8_t bytes[DERIVE_NODE_LEN];

    memset(bytes, 0, sizeof(bytes));
    UL_CHECK(!derive_node_import(bytes, &node), "an all-zero key is refused");

    /* n itself, and n + 1: the first values a key may not take. */
    from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141", bytes, 32);
    UL_CHECK(!derive_node_import(bytes, &node), "a key at the group order is refused");
    from_hex("fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364142", bytes, 32);
    UL_CHECK(!derive_node_import(bytes, &node), "a key above the group order is refused");

    UL_CHECK(!derive_node_import(NULL, &node), "a NULL blob is refused");
}

/* Every level this device walks is hardened, by the spec's own i'. An index
 * that already carries the bit is a caller naming a child it did not mean. */
static void test_a_prehardened_note_index_is_refused(void) {
    uint8_t bytes[DERIVE_NODE_LEN], out[DERIVE_SECRET_LEN];
    from_hex(NODE_A, bytes, sizeof(bytes));
    derive_xprv_t node;
    UL_CHECK(derive_node_import(bytes, &node), "node imports");
    UL_CHECK(!derive_note_secret(&node, 0x80000000u, out), "a pre-hardened index is refused");
    UL_CHECK(!derive_note_secret(&node, 0xffffffffu, out), "so is the top of the range");
    UL_CHECK(derive_note_secret(&node, 0x7fffffffu, out), "the last legal index still derives");
    derive_wipe(&node);
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
    test_a_malformed_node_is_refused();
    test_a_prehardened_note_index_is_refused();
}
