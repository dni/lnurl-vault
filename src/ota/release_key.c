#include "release_key.h"

/* Release public key — see release_key.h. Generated via
 * `python3 tools/ota_push.py keygen`; the matching private seed lives
 * offline and (hex-encoded) as this repo's OTA_SIGNING_SEED CI secret, see
 * README.md's "OTA firmware updates" section. Not the all-zero placeholder
 * anymore, so ota_sign.c's ota_verify_signature() now accepts images signed
 * with the corresponding seed instead of failing closed against everything. */
const uint8_t OTA_RELEASE_PUBKEY[32] = {
    0xfb, 0xdf, 0x53, 0xc4, 0xd1, 0xbf, 0xcc, 0x6a, 0xfb, 0x32, 0x30, 0xc2,
    0x19, 0x42, 0xd4, 0x3f, 0x83, 0x17, 0xef, 0xc0, 0xa6, 0xcc, 0xe1, 0x7d,
    0x20, 0xc8, 0xe9, 0x78, 0x4e, 0x08, 0x9e, 0x7b,
};
