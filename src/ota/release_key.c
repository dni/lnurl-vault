#include "release_key.h"

/* Release public key — see release_key.h. Generated via
 * `python3 tools/ota_push.py keygen`; the matching private seed lives
 * offline and (hex-encoded) as this repo's OTA_SIGNING_SEED CI secret, see
 * README.md's "OTA firmware updates" section. Not the all-zero placeholder
 * anymore, so ota_sign.c's ota_verify_signature() now accepts images signed
 * with the corresponding seed instead of failing closed against everything.
 *
 * 283761ec5824b4a64746e03430e28610038344a0df2de6daa22b1905f3a462ee
 *
 * Rotating this is a two-release operation, because a device only trusts the
 * key the firmware it is currently running was built with: ship one release
 * carrying the new key but still signed with the OLD seed, then switch
 * OTA_SIGNING_SEED so everything after is signed with the new one. A device
 * that skips the intermediate release has to be re-flashed over USB, since
 * the flasher path does not check OTA signatures. */
const uint8_t OTA_RELEASE_PUBKEY[32] = {
    0x28, 0x37, 0x61, 0xec, 0x58, 0x24, 0xb4, 0xa6, 0x47, 0x46, 0xe0, 0x34,
    0x30, 0xe2, 0x86, 0x10, 0x03, 0x83, 0x44, 0xa0, 0xdf, 0x2d, 0xe6, 0xda,
    0xa2, 0x2b, 0x19, 0x05, 0xf3, 0xa4, 0x62, 0xee,
};
