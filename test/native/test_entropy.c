/* Seed generation neither end can steer. The properties worth pinning are
 * adversarial ones: what a broken vault cannot do to the seed, and what a
 * hostile host cannot do to it either. */
#include <stdio.h>
#include <string.h>

#include "entropy.h"
#include "sha256.h"
#include "unity_lite.h"

/* A deterministic stand-in for esp_fill_random, so a test can pin what the
 * device contributed. */
static uint8_t g_rng_byte = 0xA5;
static bool g_rng_ok = true;
static bool fake_rng(uint8_t *out, size_t len) {
    if (!g_rng_ok) {
        return false;
    }
    memset(out, g_rng_byte, len);
    return true;
}

static const uint8_t HOST[32] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                  0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
                                  0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
                                  0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20};

static void reset(void) {
    g_rng_byte = 0xA5;
    g_rng_ok = true;
}

static void test_a_full_exchange_produces_a_verifiable_seed(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    UL_CHECK(entropy_begin(fake_rng, &s, commitment), "begins");
    UL_CHECK(entropy_finish(&s, HOST, sizeof(HOST), seed, reveal), "finishes");
    UL_CHECK(entropy_verify_commitment(commitment, reveal),
             "the revealed bytes match the commitment");
}

/* The reason for the ordering: a vault that could see the host's bytes before
 * fixing its own could search for a seed it liked. Revealing anything other
 * than what it committed to must fail the host's check. */
static void test_a_vault_cannot_swap_its_contribution(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    entropy_begin(fake_rng, &s, commitment);
    entropy_finish(&s, HOST, sizeof(HOST), seed, reveal);
    reveal[0] ^= 0x01u;
    UL_CHECK(!entropy_verify_commitment(commitment, reveal), "a substituted reveal is caught");
}

/* A host that contributes nothing of value must not be able to make the seed
 * worse than the TRNG alone -- and must not be able to choose it either. */
static void test_a_degenerate_host_cannot_weaken_the_seed(void) {
    reset();
    uint8_t zeros[32] = {0};
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed_zero[ENTROPY_SEED_LEN],
        seed_real[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];

    entropy_begin(fake_rng, &s, commitment);
    UL_CHECK(entropy_finish(&s, zeros, sizeof(zeros), seed_zero, reveal), "all-zero host accepted");

    entropy_begin(fake_rng, &s, commitment);
    entropy_finish(&s, HOST, sizeof(HOST), seed_real, reveal);

    UL_CHECK(memcmp(seed_zero, seed_real, ENTROPY_SEED_LEN) != 0,
             "the host's bytes still move the seed");
    /* And the all-zero result is not itself degenerate. */
    uint8_t blank[ENTROPY_SEED_LEN] = {0};
    UL_CHECK(memcmp(seed_zero, blank, ENTROPY_SEED_LEN) != 0, "the seed is not all-zero");
}

/* The mirror property: two different devices with the same host contribution
 * must not land on the same seed. */
static void test_the_device_contribution_still_matters(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed_a[ENTROPY_SEED_LEN],
        seed_b[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    entropy_begin(fake_rng, &s, commitment);
    entropy_finish(&s, HOST, sizeof(HOST), seed_a, reveal);
    g_rng_byte = 0x5Au;
    entropy_begin(fake_rng, &s, commitment);
    entropy_finish(&s, HOST, sizeof(HOST), seed_b, reveal);
    UL_CHECK(memcmp(seed_a, seed_b, ENTROPY_SEED_LEN) != 0,
             "a different device contribution gives a different seed");
}

/* A rejected contribution must not leave the commitment live for another go,
 * or a host could sample seeds against a pinned device value. */
static void test_a_rejected_contribution_closes_the_session(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    entropy_begin(fake_rng, &s, commitment);
    uint8_t too_short[ENTROPY_HOST_MIN_LEN - 1] = {0};
    UL_CHECK(!entropy_finish(&s, too_short, sizeof(too_short), seed, reveal), "too short refused");
    UL_CHECK(!entropy_finish(&s, HOST, sizeof(HOST), seed, reveal),
             "and the session is closed, not retryable");
}

static void test_host_bounds(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    static uint8_t big[ENTROPY_HOST_MAX_LEN + 1];
    memset(big, 0x77, sizeof(big));

    entropy_begin(fake_rng, &s, commitment);
    UL_CHECK(!entropy_finish(&s, big, ENTROPY_HOST_MAX_LEN + 1, seed, reveal), "too long refused");

    entropy_begin(fake_rng, &s, commitment);
    UL_CHECK(entropy_finish(&s, big, ENTROPY_HOST_MAX_LEN, seed, reveal), "maximum accepted");

    entropy_begin(fake_rng, &s, commitment);
    UL_CHECK(entropy_finish(&s, big, ENTROPY_HOST_MIN_LEN, seed, reveal), "minimum accepted");
}

/* A stuck TRNG is the failure this exchange survives, but an all-zero draw is
 * a broken peripheral: continuing would quietly make the host the sole author
 * of the seed while the commitment still looked well-formed. */
static void test_a_stuck_rng_refuses_to_begin(void) {
    reset();
    g_rng_byte = 0x00;
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN];
    UL_CHECK(!entropy_begin(fake_rng, &s, commitment), "an all-zero draw is refused");
    g_rng_ok = false;
    g_rng_byte = 0xA5;
    UL_CHECK(!entropy_begin(fake_rng, &s, commitment), "an RNG failure is refused");
}

static void test_an_unfinished_session_cannot_finish_twice(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    entropy_begin(fake_rng, &s, commitment);
    UL_CHECK(entropy_finish(&s, HOST, sizeof(HOST), seed, reveal), "first finish works");
    UL_CHECK(!entropy_finish(&s, HOST, sizeof(HOST), seed, reveal), "second is refused");
}

static void test_abort_wipes_the_contribution(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN];
    entropy_begin(fake_rng, &s, commitment);
    entropy_abort(&s);
    uint8_t blank[ENTROPY_LEN] = {0};
    UL_CHECK(memcmp(s.device, blank, ENTROPY_LEN) == 0, "the device bytes are gone");
    uint8_t seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    UL_CHECK(!entropy_finish(&s, HOST, sizeof(HOST), seed, reveal), "and it cannot finish");
}

/* The commitment must not be the seed's own input under another name. */
static void test_commitment_and_seed_are_domain_separated(void) {
    reset();
    entropy_session_t s;
    uint8_t commitment[ENTROPY_COMMITMENT_LEN], seed[ENTROPY_SEED_LEN], reveal[ENTROPY_LEN];
    entropy_begin(fake_rng, &s, commitment);
    entropy_finish(&s, HOST, sizeof(HOST), seed, reveal);
    UL_CHECK(memcmp(commitment, seed, ENTROPY_SEED_LEN) != 0, "commitment is not the seed");
    uint8_t plain[32];
    sha256(reveal, ENTROPY_LEN, plain);
    UL_CHECK(memcmp(commitment, plain, sizeof(plain)) != 0,
             "and not a bare sha256 of the contribution");
}

void test_entropy_run(void) {
    printf("-- entropy --\n");
    test_a_full_exchange_produces_a_verifiable_seed();
    test_a_vault_cannot_swap_its_contribution();
    test_a_degenerate_host_cannot_weaken_the_seed();
    test_the_device_contribution_still_matters();
    test_a_rejected_contribution_closes_the_session();
    test_host_bounds();
    test_a_stuck_rng_refuses_to_begin();
    test_an_unfinished_session_cannot_finish_twice();
    test_abort_wipes_the_contribution();
    test_commitment_and_seed_are_domain_separated();
}
