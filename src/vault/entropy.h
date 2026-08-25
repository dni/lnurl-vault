#ifndef LNURLVAULT_ENTROPY_H
#define LNURLVAULT_ENTROPY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Seed generation neither side can steer.
 *
 * The note seed (derive.h) is the one secret whose quality every note depends
 * on for the life of the device, and the TRNG behind it comes with a caveat
 * this repo already documents: Espressif's full-entropy guarantee for
 * esp_fill_random() is conditional on Wi-Fi or BT having been active, and the
 * startup self-test catches only a catastrophically stuck source, not a merely
 * poor one (README.md, Security posture). Trusting it alone for the seed is a
 * bet on firmware ordering staying correct forever.
 *
 * So the seed is drawn from BOTH ends and neither can choose it:
 *
 *   1. entropy_begin  -- vault draws 32 bytes from the TRNG and publishes only
 *                        sha256 of them. It is now committed.
 *   2. host           -- the wallet gathers real human randomness (an offline
 *                        game: pointer paths, inter-event timings, whatever
 *                        the player does) and sends it over.
 *   3. entropy_finish -- vault mixes the two into the seed and reveals its own
 *                        contribution, so the wallet can check it against the
 *                        commitment from step 1.
 *
 * The commitment is the whole point of the ordering. Without it a vault with a
 * broken or backdoored TRNG could wait to see the host's bytes and then search
 * its own for a seed it likes; with it, the vault is bound before the host
 * says anything, and the host is the only one who moves second. Equally, a
 * malicious HOST cannot choose the seed either, because the vault's committed
 * bytes are already fixed and mixed in.
 *
 * The mix is a hash, so neither contribution can cancel the other: a host that
 * sends all zeroes, or the same bytes every time, leaves the result exactly as
 * good as the TRNG alone, and a dead TRNG leaves it exactly as good as the
 * host's game. It takes both being bad to produce a bad seed, which is the
 * property worth having.
 *
 * Portable: no ESP-IDF, no storage. The RNG is injected, so this runs
 * identically in test/native/test_entropy.c and in firmware. */

#define ENTROPY_LEN 32
#define ENTROPY_COMMITMENT_LEN 32
#define ENTROPY_SEED_LEN 32

/* Enough that a host cannot pass off a token gesture as a contribution. A
 * game that has collected less than this has not collected anything. */
#define ENTROPY_HOST_MIN_LEN 16
#define ENTROPY_HOST_MAX_LEN 1024

/* Fills `len` bytes with cryptographically random data; false on failure.
 * Firmware wires this to esp_fill_random, as vault.h's vault_rng_fn does. */
typedef bool (*entropy_rng_fn)(uint8_t *out, size_t len);

/* Holds the device's committed contribution between the two steps. Secret
 * until revealed -- entropy_finish wipes it, and an abandoned session must be
 * dropped with entropy_abort(). */
typedef struct {
    uint8_t device[ENTROPY_LEN];
    bool open;
} entropy_session_t;

/* Step 1. Draws the device's contribution and returns only its commitment.
 * False if the RNG fails, or if the draw came back all-zero -- the same
 * stuck-source check identity_seed_is_blank makes, applied where it matters
 * most. */
bool entropy_begin(entropy_rng_fn rng, entropy_session_t *session,
                    uint8_t commitment[ENTROPY_COMMITMENT_LEN]);

/* Step 3. Mixes the host's contribution into the committed device bytes,
 * writes the seed, and reveals the device bytes so the host can verify the
 * commitment it was given in step 1.
 *
 * False if the session is not open, or the host contribution is outside
 * ENTROPY_HOST_MIN_LEN..ENTROPY_HOST_MAX_LEN. The session is closed either
 * way, so a rejected contribution cannot be retried against the same
 * commitment -- otherwise a host could probe repeatedly while the device
 * stayed bound to one value, which is the grinding the commitment exists to
 * stop, just pointed the other way. */
bool entropy_finish(entropy_session_t *session, const uint8_t *host, size_t host_len,
                     uint8_t seed_out[ENTROPY_SEED_LEN],
                     uint8_t reveal_out[ENTROPY_LEN]);

/* Drops an unfinished session and wipes its secret. */
void entropy_abort(entropy_session_t *session);

/* The host's side of the check, here so both ends are tested against one
 * implementation of the commitment. */
bool entropy_verify_commitment(const uint8_t commitment[ENTROPY_COMMITMENT_LEN],
                                const uint8_t reveal[ENTROPY_LEN]);

#endif
