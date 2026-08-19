#ifndef LNURLVAULT_IDENTITY_H
#define LNURLVAULT_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A key that says which vault this is.
 *
 * The protocol has had nothing to pin to. get_info reports fw_version and
 * board -- class identifiers every unit of a build shares -- so a swapped or
 * hostile vault answering the same commands is indistinguishable from the one
 * that was paired yesterday, and the wallet documents that it deliberately
 * does not try (issue #69). A challenge-response over a per-device key is
 * what makes trust-on-first-use possible at all.
 *
 * It proves ONE thing: the device answering now holds the same key as the
 * device that answered before. It says nothing about what that device is, and
 * it is not a defence against someone with the vault in their hands. Physical
 * possession is still the model.
 *
 * NOT a note secret. It never signs a spend, never leaves the device, and
 * lives outside the note namespace. A wipe destroys it, so a wiped vault
 * becomes a DIFFERENT vault to any wallet that had pinned it -- which is the
 * right answer for a device that has been sold on or handed over.
 *
 * Portable: no ESP-IDF, no storage. The seed is passed in, so the whole
 * thing is exercised natively (test/native/test_identity.c). */

#define IDENTITY_SEED_LEN 32
#define IDENTITY_PUBKEY_LEN 32
#define IDENTITY_SIG_LEN 64

/* The host picks the nonce, so it must be big enough that the device cannot
 * usefully precompute answers, and bounded so a challenge cannot be turned
 * into an oracle for signing arbitrary long messages. */
#define IDENTITY_NONCE_MIN_LEN 16
#define IDENTITY_NONCE_MAX_LEN 32

/* "lnurlvault-id-v1" || 0x00 || nonce -- same domain-separated construction
 * as ota_sign.c, and for the same reason: a signature made for one purpose
 * must not verify for another. Without the prefix, a nonce that happened to
 * equal an OTA signing message would let an identity challenge be replayed as
 * a firmware approval. */
#define IDENTITY_DOMAIN "lnurlvault-id-v1"
#define IDENTITY_MESSAGE_MAX_LEN (sizeof(IDENTITY_DOMAIN) + IDENTITY_NONCE_MAX_LEN)

/* Returns the message length, or 0 if the nonce is out of bounds. */
size_t identity_signing_message(const uint8_t *nonce, size_t nonce_len, uint8_t *out,
                                 size_t outcap);

void identity_pubkey(const uint8_t seed[IDENTITY_SEED_LEN], uint8_t pubkey[IDENTITY_PUBKEY_LEN]);

/* False if the nonce is out of bounds. */
bool identity_sign(const uint8_t seed[IDENTITY_SEED_LEN], const uint8_t *nonce, size_t nonce_len,
                    uint8_t sig[IDENTITY_SIG_LEN]);

/* The host's side of the check, here so both ends are tested against one
 * implementation of the message construction. */
bool identity_verify(const uint8_t pubkey[IDENTITY_PUBKEY_LEN], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t sig[IDENTITY_SIG_LEN]);

/* True if every byte is zero -- an unprovisioned seed, which must never be
 * used to sign. An all-zero seed is a perfectly valid ed25519 key, so nothing
 * downstream would notice; every device that failed to generate one would
 * share an identity and TOFU would pin the wrong thing everywhere. */
bool identity_seed_is_blank(const uint8_t seed[IDENTITY_SEED_LEN]);

#endif
