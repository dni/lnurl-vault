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

static vault_storage_state_t g_state = VAULT_STORAGE_UNAVAILABLE;

/* Brings up NVS, and DOES NOT ERASE IT for any reason.
 *
 * This used to be the idiom straight out of every ESP-IDF example: on
 * ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND, erase and try
 * again. That idiom is written for devices whose NVS holds a Wi-Fi
 * credential and a calibration blob, where erasing costs a re-pairing. Here
 * it silently and irreversibly destroys bearer notes -- value, not settings.
 *
 * And it is reachable rather than theoretical. A note blob is
 * sizeof(note_t) == 448 bytes, so a full vault of 128 is around 57KB of a
 * 132KB partition, and every confirm, rename or mark_spent rewrites a blob.
 * Running out of free pages is an ordinary outcome of use, not a sign of
 * corruption -- and the notes are all still there when it happens.
 *
 * So: report it, refuse to proceed silently, and leave the flash alone. The
 * only thing in this firmware that erases is vault_nvs_wipe(), and it is
 * reachable only behind a physical confirmation. */
esp_err_t vault_nvs_boot(void) {
    esp_err_t err = nvs_flash_init();

    switch (err) {
        case ESP_OK:
            g_state = VAULT_STORAGE_OK;
            break;
        case ESP_ERR_NVS_NO_FREE_PAGES:
            g_state = VAULT_STORAGE_FULL;
            ESP_LOGE(TAG,
                     "NVS is out of free pages. NOT erasing: every note is still on flash "
                     "and erasing would destroy them. get_info reports storage=full; "
                     "recovery is a deliberate `wipe` (see docs/PROTOCOL.md), never automatic.");
            break;
        case ESP_ERR_NVS_NEW_VERSION_FOUND:
            g_state = VAULT_STORAGE_VERSION;
            ESP_LOGE(TAG,
                     "NVS was written by a newer format than this firmware understands. "
                     "NOT erasing: a correct firmware could still read these notes.");
            break;
        default:
            g_state = VAULT_STORAGE_UNAVAILABLE;
            ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
            break;
    }
    return err;
}

vault_storage_state_t vault_nvs_state(void) {
    return g_state;
}

const char *vault_nvs_state_name(void) {
    switch (g_state) {
        case VAULT_STORAGE_OK:
            return "ok";
        case VAULT_STORAGE_FULL:
            return "full";
        case VAULT_STORAGE_VERSION:
            return "version_unsupported";
        default:
            return "unavailable";
    }
}

bool vault_nvs_wipe(void) {
    /* Close the handle first: erasing the partition underneath an open handle
     * leaves it pointing at storage that no longer exists. */
    if (g_open) {
        nvs_close(g_handle);
        g_open = false;
    }

    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGE(TAG, "wipe: deinit failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wipe: erase failed: %s", esp_err_to_name(err));
        return false;
    }

    /* VERIFY. An erase that reported success but left the data readable is
     * the failure mode that matters, because the owner acts on the claim. */
    err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wipe: re-init after erase failed: %s", esp_err_to_name(err));
        g_state = VAULT_STORAGE_UNAVAILABLE;
        return false;
    }
    g_state = VAULT_STORAGE_OK;

    nvs_handle_t check;
    err = nvs_open(NAMESPACE, NVS_READONLY, &check);
    if (err == ESP_OK) {
        size_t size = 0;
        esp_err_t found = nvs_get_blob(check, INDEX_KEY, NULL, &size);
        nvs_close(check);
        if (found != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGE(TAG, "wipe: the note index is still readable after erasing; refusing to "
                          "report success");
            return false;
        }
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        /* NOT_FOUND is the expected outcome: the namespace itself is gone. */
        ESP_LOGE(TAG, "wipe: could not verify the namespace is gone: %s", esp_err_to_name(err));
        return false;
    }

    /* Reopen for normal service, so a device that is not rebooted immediately
     * still has working storage. */
    if (!vault_nvs_storage_init()) {
        ESP_LOGE(TAG, "wipe: storage erased and verified, but could not be reopened");
        return false;
    }

    ESP_LOGW(TAG, "storage wiped and verified empty");
    return true;
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
    /* nvs_get_blob's `size` is in/out: it reports ESP_OK for a stored blob
     * SHORTER than the buffer, having written only that many bytes. A note
     * written by a build with a different note_t layout would come back
     * partially filled and look like a successful load, with the untouched
     * tail (state, parent_count, timestamps) carrying whatever the caller's
     * buffer already held. Insist on a whole note: returning false here
     * routes the id to vault_init's unloaded list, which keeps it in the
     * index and leaves the blob on flash for a firmware that understands
     * it, rather than orphaning or half-reading it. */
    size_t size = sizeof(*out);
    if (nvs_get_blob(g_handle, id, out, &size) != ESP_OK) {
        return false;
    }
    if (size != sizeof(*out)) {
        ESP_LOGE(TAG, "load_note %s: stored blob is %u bytes, expected %u; refusing a partial note",
                 id, (unsigned)size, (unsigned)sizeof(*out));
        return false;
    }
    return true;
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
