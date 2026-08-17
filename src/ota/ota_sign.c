#include "ota_sign.h"

#include <string.h>

#include "monocypher-ed25519.h"

static const char OTA_DOMAIN[] = "lnurlvault-ota-v1";
#define OTA_DOMAIN_LEN (sizeof(OTA_DOMAIN) - 1) /* exclude the NUL string terminator */

void ota_signing_message(const uint8_t digest[OTA_DIGEST_LEN], uint8_t out[OTA_MESSAGE_MAX_LEN],
                          size_t *out_len) {
    size_t off = 0;
    memcpy(out + off, OTA_DOMAIN, OTA_DOMAIN_LEN);
    off += OTA_DOMAIN_LEN;
    out[off++] = 0;
    memcpy(out + off, digest, OTA_DIGEST_LEN);
    off += OTA_DIGEST_LEN;
    *out_len = off;
}

bool ota_verify_signature(const uint8_t pubkey[OTA_PUBKEY_LEN], const uint8_t digest[OTA_DIGEST_LEN],
                           const uint8_t signature[OTA_SIGNATURE_LEN]) {
    uint8_t message[OTA_MESSAGE_MAX_LEN];
    size_t message_len;
    ota_signing_message(digest, message, &message_len);
    return crypto_ed25519_check(signature, pubkey, message, message_len) == 0;
}
