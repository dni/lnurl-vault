#include <string.h>

#include "monocypher-ed25519.h"
#include "ota_sign.h"
#include "unity_lite.h"

static void make_keypair(uint8_t seed_byte, uint8_t secret_key[64], uint8_t public_key[32]) {
    uint8_t seed[32];
    memset(seed, seed_byte, sizeof(seed));
    crypto_ed25519_key_pair(secret_key, public_key, seed);
}

void test_ota_sign_run(void) {
    uint8_t secret_key[64], public_key[32];
    make_keypair(7, secret_key, public_key);

    uint8_t digest[OTA_DIGEST_LEN];
    memset(digest, 0xAB, sizeof(digest));

    uint8_t message[OTA_MESSAGE_MAX_LEN];
    size_t message_len = 0;
    ota_signing_message(digest, message, &message_len);
    UL_CHECK(message_len == 17 + 1 + OTA_DIGEST_LEN, "signing message is domain + NUL + digest");
    UL_CHECK(memcmp(message, "lnurlvault-ota-v1", 17) == 0, "signing message starts with the domain label");
    UL_CHECK(message[17] == 0, "domain label is NUL-separated from the digest");

    uint8_t signature[OTA_SIGNATURE_LEN];
    crypto_ed25519_sign(signature, secret_key, message, message_len);

    UL_CHECK(ota_verify_signature(public_key, digest, signature), "a genuine signature verifies");

    uint8_t tampered_digest[OTA_DIGEST_LEN];
    memcpy(tampered_digest, digest, sizeof(digest));
    tampered_digest[0] ^= 1;
    UL_CHECK(!ota_verify_signature(public_key, tampered_digest, signature),
             "a signature does not verify against a different digest");

    uint8_t other_secret[64], other_public[32];
    make_keypair(9, other_secret, other_public);
    UL_CHECK(!ota_verify_signature(other_public, digest, signature),
             "a signature does not verify against the wrong public key");

    uint8_t corrupt_sig[OTA_SIGNATURE_LEN];
    memcpy(corrupt_sig, signature, sizeof(signature));
    corrupt_sig[63] ^= 0x80;
    UL_CHECK(!ota_verify_signature(public_key, digest, corrupt_sig), "a corrupted signature is rejected");

    uint8_t zero_key[OTA_PUBKEY_LEN];
    memset(zero_key, 0, sizeof(zero_key));
    UL_CHECK(!ota_verify_signature(zero_key, digest, signature),
             "an all-zero placeholder public key fails closed, not open");

    /* A NULL argument means "this device has nothing to verify with", which
     * must refuse rather than read address zero. Without this, a caller that
     * skips its own ota_pubkey check dereferences NULL inside monocypher
     * while loading the key — a remote panic on an unsigned image instead of
     * a rejection. dispatcher.c guards both of its call sites; this is the
     * backstop for the next call site somebody adds. */
    UL_CHECK(!ota_verify_signature(NULL, digest, signature),
             "a NULL public key is refused, not dereferenced");
    UL_CHECK(!ota_verify_signature(public_key, NULL, signature),
             "a NULL digest is refused, not dereferenced");
    UL_CHECK(!ota_verify_signature(public_key, digest, NULL),
             "a NULL signature is refused, not dereferenced");

    /* Domain separation is load-bearing, not decoration: a signature the
     * real release key made over the bare digest — the obvious way to get
     * this wrong, and the shape another tool signing "the sha256" would
     * produce — must not verify as a firmware release. */
    uint8_t bare_sig[OTA_SIGNATURE_LEN];
    crypto_ed25519_sign(bare_sig, secret_key, digest, OTA_DIGEST_LEN);
    UL_CHECK(!ota_verify_signature(public_key, digest, bare_sig),
             "a genuine signature over the undomained digest is not a valid release signature");
}
