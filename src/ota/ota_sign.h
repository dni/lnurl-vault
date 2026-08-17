#ifndef LNURLVAULT_OTA_SIGN_H
#define LNURLVAULT_OTA_SIGN_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Portable (no ESP-IDF dependency, unit-tested from test/native/) release-
 * signature scheme for OTA images, adapted from forgesworn/heartwood-esp32's
 * `common/src/ota_sign.rs` — same domain-separated-message construction,
 * same verify-only-on-device design, ported to C over vendored Monocypher
 * (monocypher.c/.h, monocypher-ed25519.c/.h — dual BSD-2-Clause/CC0, see
 * their own header comments) instead of the `ed25519-compact` Rust crate.
 * The device only ever verifies; it never holds a signing key. */

#define OTA_SIGNATURE_LEN 64
#define OTA_PUBKEY_LEN 32
#define OTA_SEED_LEN 32
#define OTA_DIGEST_LEN 32

/* Upper bound on ota_signing_message()'s output — big enough for
 * "lnurlvault-ota-v1" || 0x00 || a 32-byte digest, with headroom. Callers
 * pass a buffer at least this big. */
#define OTA_MESSAGE_MAX_LEN 64

/* The exact byte string the release key signs for one firmware image:
 * "lnurlvault-ota-v1" || 0x00 || sha256(image). No board id in the message
 * (unlike heartwood's scheme) — this project targets exactly one board
 * (LilyGo T-Display S3, see README.md); add one here first if that ever
 * changes, the same way heartwood's message binds board::BOARD, so a signed
 * image for a future second board can't be replayed onto this one. */
void ota_signing_message(const uint8_t digest[OTA_DIGEST_LEN], uint8_t out[OTA_MESSAGE_MAX_LEN],
                          size_t *out_len);

/* Verify a release signature over an image digest. Fail-closed: a malformed
 * key or signature (including an all-zero placeholder) returns false. */
bool ota_verify_signature(const uint8_t pubkey[OTA_PUBKEY_LEN], const uint8_t digest[OTA_DIGEST_LEN],
                           const uint8_t signature[OTA_SIGNATURE_LEN]);

#endif
