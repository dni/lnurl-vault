#ifndef LNURLVAULT_NVS_STORAGE_H
#define LNURLVAULT_NVS_STORAGE_H

#include <stdbool.h>

#include "esp_err.h"
#include "vault.h"

/* ESP-IDF-specific vault_storage_t backend (encrypted NVS). Everything in
 * vault.c/vault.h is storage-agnostic and portable; this is the one piece
 * that actually talks to flash, which is why it lives outside src/vault/
 * (excluded from the native test build — see platformio.ini's
 * `[env:native]` build_src_filter and test/native/Makefile). */

/* Brings up NVS. If CONFIG_NVS_ENCRYPTION is set (see sdkconfig.defaults),
 * plain nvs_flash_init() transparently encrypts the default partition using
 * whichever key-protection scheme is Kconfig-selected (HMAC-peripheral on
 * ESP32-S3) — nothing extra to do here; see nvs_storage.c's header comment
 * for how that was confirmed against a real build. Call once, before
 * vault_nvs_storage_init(). */
/* NEVER erases, whatever nvs_flash_init() reports. See vault_nvs_boot(). */
esp_err_t vault_nvs_boot(void);

/* What state storage came up in, for get_info to report. A vault that cannot
 * read its own notes must say so out loud rather than present as an empty
 * working device: an owner who believes their notes are gone behaves very
 * differently from one who knows the device needs attention, and only one of
 * those two beliefs can be corrected later. */
typedef enum {
    VAULT_STORAGE_OK,
    /* The partition is out of free pages. Every note is still physically on
     * flash and none of it is readable until the partition is either grown
     * or deliberately wiped. Ordinary churn reaches this: a note blob is 448
     * bytes and every confirm, rename or mark_spent rewrites one. */
    VAULT_STORAGE_FULL,
    /* Written by a newer NVS format than this firmware understands -- a
     * downgrade, not a corruption. Erasing would destroy notes that a
     * correct firmware could still read. */
    VAULT_STORAGE_VERSION,
    VAULT_STORAGE_UNAVAILABLE,
} vault_storage_state_t;

vault_storage_state_t vault_nvs_state(void);

/* Short stable name for the state above, for the wire. Never NULL. */
const char *vault_nvs_state_name(void);

/* Erases the whole NVS partition, then VERIFIES it is actually gone by
 * re-initialising and re-reading. Returns false if either step fails, and in
 * particular returns false rather than true when the erase reported success
 * but the data is still there -- a wipe that claims success it cannot
 * demonstrate is worse than one that admits failure, because the owner acts
 * on the claim (sells the device, hands it on).
 *
 * Only ever called behind a physical confirmation; see dispatcher.h's
 * wipe_storage_fn and docs/PROTOCOL.md's `wipe`. Callers must not reboot or
 * report success when this returns false. */
bool vault_nvs_wipe(void);

/* Opens the "vault" NVS namespace and wires up the vault_storage_t backend.
 * Returns false on failure (check the log). */
bool vault_nvs_storage_init(void);

/* The backend to pass to vault_init(), valid only after
 * vault_nvs_storage_init() returns true. */
const vault_storage_t *vault_nvs_storage(void);

/* This device's identity seed (#69, src/vault/identity.h). Lives in the same
 * namespace as the notes, so `wipe` destroys it with everything else -- a
 * wiped vault becomes a different vault to any wallet that pinned it, which
 * is right for a device that has been handed on.
 *
 * Load returns false when there is none yet, or it could not be read. */
bool vault_nvs_identity_load(uint8_t seed[32]);
bool vault_nvs_identity_save(const uint8_t seed[32]);

#endif
