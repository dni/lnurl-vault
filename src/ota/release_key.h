#ifndef LNURLVAULT_RELEASE_KEY_H
#define LNURLVAULT_RELEASE_KEY_H

#include <stdint.h>

/* The release public key OTA images must be signed against — baked into
 * the firmware at build time, checked at runtime, never the private half
 * (that never touches the device; see tools/ota_push.py and
 * docs/PROTOCOL.md's `ota_begin`). A real keypair has been generated (see
 * release_key.c) — the matching seed is kept offline and, hex-encoded, as
 * this repo's OTA_SIGNING_SEED CI secret (README.md's "OTA firmware
 * updates" section; same custody model as forgesworn/heartwood-esp32's
 * docs/ota-signing.md, which this scheme is adapted from). Rotating this
 * key is a two-release operation — see README.md for why — not just a
 * matter of pasting in a new array here. */
extern const uint8_t OTA_RELEASE_PUBKEY[32];

#endif
