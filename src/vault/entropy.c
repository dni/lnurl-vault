#include "entropy.h"

#include <string.h>

#include "monocypher.h" /* crypto_wipe */
#include "sha256.h"

/* Domain-separated exactly as identity.c separates its signing message, and
 * for the same reason: the commitment and the seed are both sha256 over the
 * same 32 secret bytes, so without distinct prefixes the commitment the vault
 * publishes would be a hash of the seed's own input, computed with the same
 * construction. Distinct prefixes keep the published value structurally
 * unrelated to the secret one. */
#define ENTROPY_COMMIT_DOMAIN "lnurlvault-entropy-commit-v1"
#define ENTROPY_SEED_DOMAIN "lnurlvault-entropy-seed-v1"

static bool all_zero(const uint8_t *bytes, size_t len) {
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= bytes[i];
    }
    return acc == 0;
}

static void commit(const uint8_t device[ENTROPY_LEN], uint8_t out[ENTROPY_COMMITMENT_LEN]) {
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    /* sizeof includes the NUL, which is the 0x00 separator. */
    sha256_update(&ctx, (const uint8_t *)ENTROPY_COMMIT_DOMAIN, sizeof(ENTROPY_COMMIT_DOMAIN));
    sha256_update(&ctx, device, ENTROPY_LEN);
    sha256_final(&ctx, out);
    crypto_wipe(&ctx, sizeof(ctx));
}

bool entropy_begin(entropy_rng_fn rng, entropy_session_t *session,
                    uint8_t commitment[ENTROPY_COMMITMENT_LEN]) {
    if (!rng || !session || !commitment) {
        return false;
    }
    memset(session, 0, sizeof(*session));
    if (!rng(session->device, ENTROPY_LEN)) {
        return false;
    }
    /* A stuck source is the failure this whole exchange exists to survive, but
     * an all-zero draw is not "poor entropy", it is a broken peripheral, and
     * continuing would silently make the host's contribution the entire seed
     * while the commitment still looked well-formed. */
    if (all_zero(session->device, ENTROPY_LEN)) {
        entropy_abort(session);
        return false;
    }
    commit(session->device, commitment);
    session->open = true;
    return true;
}

bool entropy_finish(entropy_session_t *session, const uint8_t *host, size_t host_len,
                     uint8_t seed_out[ENTROPY_SEED_LEN], uint8_t reveal_out[ENTROPY_LEN]) {
    if (!session || !session->open || !seed_out || !reveal_out) {
        return false;
    }
    if (!host || host_len < ENTROPY_HOST_MIN_LEN || host_len > ENTROPY_HOST_MAX_LEN) {
        /* Closed even on rejection: see entropy.h. A host allowed to retry
         * against a live commitment could sample many seeds while the device
         * stayed pinned to one contribution. */
        entropy_abort(session);
        return false;
    }

    /* The host's bytes are hashed first so an arbitrary-length game transcript
     * enters the mix at a fixed width, and so the seed step's input layout
     * cannot be made ambiguous by a chosen length. */
    uint8_t host_digest[32];
    sha256(host, host_len, host_digest);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)ENTROPY_SEED_DOMAIN, sizeof(ENTROPY_SEED_DOMAIN));
    sha256_update(&ctx, session->device, ENTROPY_LEN);
    sha256_update(&ctx, host_digest, sizeof(host_digest));
    sha256_final(&ctx, seed_out);

    memcpy(reveal_out, session->device, ENTROPY_LEN);

    crypto_wipe(&ctx, sizeof(ctx));
    crypto_wipe(host_digest, sizeof(host_digest));
    entropy_abort(session);
    return true;
}

void entropy_abort(entropy_session_t *session) {
    if (session) {
        crypto_wipe(session, sizeof(*session));
    }
}

bool entropy_verify_commitment(const uint8_t commitment[ENTROPY_COMMITMENT_LEN],
                                const uint8_t reveal[ENTROPY_LEN]) {
    if (!commitment || !reveal) {
        return false;
    }
    uint8_t expect[ENTROPY_COMMITMENT_LEN];
    commit(reveal, expect);
    /* Constant-time: a host checking many commitments must not learn where the
     * first differing byte was. */
    const bool ok = crypto_verify32(expect, commitment) == 0;
    crypto_wipe(expect, sizeof(expect));
    return ok;
}
