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
esp_err_t vault_nvs_boot(void);

/* Opens the "vault" NVS namespace and wires up the vault_storage_t backend.
 * Returns false on failure (check the log). */
bool vault_nvs_storage_init(void);

/* The backend to pass to vault_init(), valid only after
 * vault_nvs_storage_init() returns true. */
const vault_storage_t *vault_nvs_storage(void);

#endif
