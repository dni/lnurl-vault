/* The device key behind trust-on-first-use (#69). It proves one thing: the
 * vault answering now holds the same key as the vault that answered before. */
#include <stdio.h>
#include <string.h>

#include "identity.h"
#include "unity_lite.h"

static const uint8_t SEED_A[IDENTITY_SEED_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
static const uint8_t SEED_B[IDENTITY_SEED_LEN] = {
    0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88, 0x77, 0x66, 0x55,
    0x44, 0x33, 0x22, 0x11, 0x00, 0x10, 0x0f, 0x0e, 0x0d, 0x0c, 0x0b,
    0x0a, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};

static const uint8_t NONCE[IDENTITY_NONCE_MAX_LEN] = {
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5,
    0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe, 0xbf};

static void test_a_challenge_verifies_against_the_pubkey(void) {
    uint8_t pubkey[IDENTITY_PUBKEY_LEN], sig[IDENTITY_SIG_LEN];
    identity_pubkey(SEED_A, pubkey);
    UL_CHECK(identity_sign(SEED_A, NONCE, 32, sig), "signs");
    UL_CHECK(identity_verify(pubkey, NONCE, 32, sig), "and verifies");
}

/* The whole point: a different device answering the same challenge must not
 * pass as the pinned one. */
static void test_another_device_cannot_pass_as_this_one(void) {
    uint8_t pubkey_a[IDENTITY_PUBKEY_LEN], sig_b[IDENTITY_SIG_LEN];
    identity_pubkey(SEED_A, pubkey_a);
    UL_CHECK(identity_sign(SEED_B, NONCE, 32, sig_b), "B signs");
    UL_CHECK(!identity_verify(pubkey_a, NONCE, 32, sig_b),
             "B's answer does not verify against A's key");
}

/* A signature is bound to the nonce that was asked for, so a recording of a
 * previous session cannot be replayed at a fresh challenge. */
static void test_an_old_answer_does_not_answer_a_new_challenge(void) {
    uint8_t pubkey[IDENTITY_PUBKEY_LEN], sig[IDENTITY_SIG_LEN];
    uint8_t other[IDENTITY_NONCE_MAX_LEN];
    memcpy(other, NONCE, sizeof(other));
    other[0] ^= 0x01;

    identity_pubkey(SEED_A, pubkey);
    UL_CHECK(identity_sign(SEED_A, NONCE, 32, sig), "signs one nonce");
    UL_CHECK(!identity_verify(pubkey, other, 32, sig), "and does not verify another");
}

/* crypto_ed25519_key_pair() wipes the seed it is handed. Ours is the device's
 * identity and has to survive: if signing destroyed it, the vault would have
 * a different identity after every challenge. */
static void test_signing_does_not_destroy_the_seed(void) {
    uint8_t seed[IDENTITY_SEED_LEN], sig[IDENTITY_SIG_LEN];
    memcpy(seed, SEED_A, sizeof(seed));
    UL_CHECK(identity_sign(seed, NONCE, 32, sig), "signs");
    UL_CHECK(memcmp(seed, SEED_A, sizeof(seed)) == 0, "the seed is untouched");

    uint8_t first[IDENTITY_PUBKEY_LEN], second[IDENTITY_PUBKEY_LEN];
    identity_pubkey(seed, first);
    identity_pubkey(seed, second);
    UL_CHECK(memcmp(first, second, sizeof(first)) == 0,
             "and the same seed still gives the same identity");
}

/* Domain separation. Without the prefix, a nonce equal to an OTA signing
 * message would let an identity challenge be replayed as a firmware
 * approval. */
static void test_the_message_is_domain_separated(void) {
    uint8_t message[IDENTITY_MESSAGE_MAX_LEN];
    size_t len = identity_signing_message(NONCE, 32, message, sizeof(message));
    UL_CHECK(len == sizeof(IDENTITY_DOMAIN) + 32, "prefix, separator and nonce");
    UL_CHECK(memcmp(message, IDENTITY_DOMAIN, sizeof(IDENTITY_DOMAIN) - 1) == 0, "the domain");
    UL_CHECK(message[sizeof(IDENTITY_DOMAIN) - 1] == 0x00, "then a 0x00 separator");
    UL_CHECK(memcmp(message + sizeof(IDENTITY_DOMAIN), NONCE, 32) == 0, "then the nonce");
}

/* A nonce the host did not choose freely, or one long enough to be an oracle
 * for signing something else, is refused rather than signed. */
static void test_nonce_bounds_are_enforced(void) {
    uint8_t sig[IDENTITY_SIG_LEN];
    UL_CHECK(!identity_sign(SEED_A, NONCE, IDENTITY_NONCE_MIN_LEN - 1, sig), "too short");
    UL_CHECK(!identity_sign(SEED_A, NONCE, IDENTITY_NONCE_MAX_LEN + 1, sig), "too long");
    UL_CHECK(identity_sign(SEED_A, NONCE, IDENTITY_NONCE_MIN_LEN, sig), "the minimum is allowed");
    UL_CHECK(identity_sign(SEED_A, NONCE, IDENTITY_NONCE_MAX_LEN, sig), "and the maximum");
}

/* An all-zero seed is a perfectly valid ed25519 key, so nothing downstream
 * would notice -- and every device that failed to generate one would share an
 * identity, which is worse than having none at all. */
static void test_a_blank_seed_is_recognised(void) {
    uint8_t blank[IDENTITY_SEED_LEN] = {0};
    UL_CHECK(identity_seed_is_blank(blank), "all zero is blank");
    UL_CHECK(!identity_seed_is_blank(SEED_A), "a real seed is not");
    blank[IDENTITY_SEED_LEN - 1] = 1;
    UL_CHECK(!identity_seed_is_blank(blank), "one set byte anywhere is enough");
}

void test_identity_run(void) {
    printf("-- identity --\n");
    test_a_challenge_verifies_against_the_pubkey();
    test_another_device_cannot_pass_as_this_one();
    test_an_old_answer_does_not_answer_a_new_challenge();
    test_signing_does_not_destroy_the_seed();
    test_the_message_is_domain_separated();
    test_nonce_bounds_are_enforced();
    test_a_blank_seed_is_recognised();
}
