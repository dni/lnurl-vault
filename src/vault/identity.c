#include "identity.h"

#include <string.h>

#include "monocypher-ed25519.h"
#include "monocypher.h" /* crypto_wipe */

size_t identity_signing_message(const uint8_t *nonce, size_t nonce_len, uint8_t *out,
                                 size_t outcap) {
    if (!nonce || !out) {
        return 0;
    }
    if (nonce_len < IDENTITY_NONCE_MIN_LEN || nonce_len > IDENTITY_NONCE_MAX_LEN) {
        return 0;
    }
    /* sizeof includes the NUL, which is the 0x00 separator. */
    const size_t prefix_len = sizeof(IDENTITY_DOMAIN);
    if (outcap < prefix_len + nonce_len) {
        return 0;
    }
    memcpy(out, IDENTITY_DOMAIN, prefix_len);
    memcpy(out + prefix_len, nonce, nonce_len);
    return prefix_len + nonce_len;
}

/* crypto_ed25519_key_pair() WIPES the seed it is handed, which is right for a
 * caller that generated one and wrong here -- ours is the device's identity
 * and has to survive the call. Every use goes through this copy. */
static void expand(const uint8_t seed[IDENTITY_SEED_LEN], uint8_t secret_key[64],
                    uint8_t pubkey[IDENTITY_PUBKEY_LEN]) {
    uint8_t scratch[IDENTITY_SEED_LEN];
    memcpy(scratch, seed, sizeof(scratch));
    crypto_ed25519_key_pair(secret_key, pubkey, scratch);
}

void identity_pubkey(const uint8_t seed[IDENTITY_SEED_LEN], uint8_t pubkey[IDENTITY_PUBKEY_LEN]) {
    uint8_t secret_key[64];
    expand(seed, secret_key, pubkey);
    crypto_wipe(secret_key, sizeof(secret_key));
}

bool identity_sign(const uint8_t seed[IDENTITY_SEED_LEN], const uint8_t *nonce, size_t nonce_len,
                    uint8_t sig[IDENTITY_SIG_LEN]) {
    uint8_t message[IDENTITY_MESSAGE_MAX_LEN];
    const size_t len = identity_signing_message(nonce, nonce_len, message, sizeof(message));
    if (len == 0) {
        return false;
    }
    uint8_t secret_key[64];
    uint8_t pubkey[IDENTITY_PUBKEY_LEN];
    expand(seed, secret_key, pubkey);
    crypto_ed25519_sign(sig, secret_key, message, len);
    crypto_wipe(secret_key, sizeof(secret_key));
    return true;
}

bool identity_verify(const uint8_t pubkey[IDENTITY_PUBKEY_LEN], const uint8_t *nonce,
                      size_t nonce_len, const uint8_t sig[IDENTITY_SIG_LEN]) {
    uint8_t message[IDENTITY_MESSAGE_MAX_LEN];
    const size_t len = identity_signing_message(nonce, nonce_len, message, sizeof(message));
    if (len == 0) {
        return false;
    }
    return crypto_ed25519_check(sig, pubkey, message, len) == 0;
}

bool identity_seed_is_blank(const uint8_t seed[IDENTITY_SEED_LEN]) {
    uint8_t seen = 0;
    for (size_t i = 0; i < IDENTITY_SEED_LEN; i++) {
        seen |= seed[i];
    }
    return seen == 0;
}
