/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build (see README.md's "Status" section for what that does and doesn't
 * prove).
 *
 * vault_nvs_boot() used to hand-roll NVS-encryption key bring-up here
 * (reading/generating keys from a `nvs_keys` partition, then calling
 * nvs_flash_secure_init() directly) — that's an older API path. A real
 * build against ESP-IDF 6.0.1 showed plain nvs_flash_init() now documents
 * doing all of that internally whenever CONFIG_NVS_ENCRYPTION is enabled,
 * dispatching to whichever key-protection scheme is Kconfig-selected (see
 * sdkconfig.defaults — on ESP32-S3 that's the HMAC-peripheral scheme, not
 * the partition-based one this file used to implement by hand, which is
 * why the `nvs_keys` partition was removed from partitions.csv too). The
 * hand-rolled version and the Kconfig-selected scheme were fighting each
 * other; calling plain nvs_flash_init() unconditionally — exactly what
 * ESP-IDF's own bleprph example does — is both simpler and correct. */
#include "nvs_storage.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "nvs_storage";
static const char *NAMESPACE = "vault";
static const char *INDEX_KEY = "index";

static nvs_handle_t g_handle;
static bool g_open = false;
static vault_storage_t g_storage_iface;

esp_err_t vault_nvs_boot(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static bool nvs_load_index(char ids[][VAULT_ID_BUF], size_t max, size_t *count, void *ctx) {
    (void)ctx;
    if (!g_open) {
        return false;
    }
    size_t cap = max * VAULT_ID_BUF;
    size_t actual = cap;
    esp_err_t err = nvs_get_blob(g_handle, INDEX_KEY, ids, &actual);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *count = 0; /* first boot: empty vault, not a failure */
        return true;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_index failed: %s", esp_err_to_name(err));
        return false;
    }
    *count = actual / VAULT_ID_BUF;
    return true;
}

static bool nvs_save_index(const char ids[][VAULT_ID_BUF], size_t count, void *ctx) {
    (void)ctx;
    if (!g_open) {
        return false;
    }
    esp_err_t err = nvs_set_blob(g_handle, INDEX_KEY, ids, count * VAULT_ID_BUF);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_index failed: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(g_handle) == ESP_OK;
}

static bool nvs_load_note(const char *id, note_t *out, void *ctx) {
    (void)ctx;
    if (!g_open) {
        return false;
    }
    size_t size = sizeof(*out);
    return nvs_get_blob(g_handle, id, out, &size) == ESP_OK;
}

static bool nvs_save_note(const note_t *note, void *ctx) {
    (void)ctx;
    if (!g_open) {
        return false;
    }
    esp_err_t err = nvs_set_blob(g_handle, note->id, note, sizeof(*note));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_note failed: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(g_handle) == ESP_OK;
}

static bool nvs_delete_note(const char *id, void *ctx) {
    (void)ctx;
    if (!g_open) {
        return false;
    }
    esp_err_t err = nvs_erase_key(g_handle, id);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "delete_note failed: %s", esp_err_to_name(err));
        return false;
    }
    return nvs_commit(g_handle) == ESP_OK;
}

bool vault_nvs_storage_init(void) {
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &g_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }
    g_open = true;

    g_storage_iface.load_index = nvs_load_index;
    g_storage_iface.save_index = nvs_save_index;
    g_storage_iface.load_note = nvs_load_note;
    g_storage_iface.save_note = nvs_save_note;
    g_storage_iface.delete_note = nvs_delete_note;
    g_storage_iface.ctx = NULL;
    return true;
}

const vault_storage_t *vault_nvs_storage(void) {
    return &g_storage_iface;
}
