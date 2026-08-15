/* This file, and everything it wires together, compiles and links into a
 * working firmware.bin against ESP-IDF 6.0.1 — see README.md's "Status"
 * section, including several real bugs found and fixed getting here.
 * dispatcher.c/vault.c and everything under src/vault and src/proto are
 * additionally exercised by test/native/, independent of the ESP32 build,
 * and pass (112/112 assertions, see README.md's Verification section).
 * What none of this proves is real hardware behavior — see the "Status"
 * section for exactly what that does and doesn't cover. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ble_gatt.h"
#include "buttons.h"
#include "dispatcher.h"
#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_storage.h"
#include "serial_cdc.h"
#include "ui_task.h"
#include "vault.h"
#include "vault_lock.h"

static const char *TAG = "main";

static bool rng_fill(uint8_t *out, size_t len) {
    esp_fill_random(out, len);
    return true;
}

static uint32_t now_seconds(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

/* Cheap sanity check, not a statistical test suite: catches a
 * catastrophically stuck RNG (e.g. hardware entropy source not actually
 * wired up) before any secret is ever generated from it. See README.md's
 * security posture section on why esp_fill_random's full-entropy guarantee
 * depends on BLE/Wi-Fi having been active — ble_gatt_start() runs before
 * this in app_main(), so that precondition is already satisfied here. */
static void rng_self_test(void) {
    uint8_t a[16], b[16];
    esp_fill_random(a, sizeof(a));
    esp_fill_random(b, sizeof(b));

    bool all_zero = true;
    bool identical = true;
    for (size_t i = 0; i < sizeof(a); i++) {
        if (a[i] != 0) {
            all_zero = false;
        }
        if (a[i] != b[i]) {
            identical = false;
        }
    }
    if (all_zero || identical) {
        ESP_LOGE(TAG, "RNG self-test failed - refusing to start (see README security posture)");
        abort();
    }
}

/* The physical confirm/cancel gate in front of vault_export_secret, now
 * delegated to ui_task.c — see its header comment for why: it's the single
 * owner of both buttons and the display, arbitrating between this (a
 * remote request) and local on-device note browsing so the two never read
 * the buttons concurrently. */
static confirm_result_t confirm_export_on_device(const note_meta_t *note) {
    return ui_task_request_remote_confirm(note, 30000);
}

void app_main(void) {
    buttons_init();
    display_init();
    vault_lock_init();

    esp_err_t err = vault_nvs_boot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "vault_nvs_boot failed: %s", esp_err_to_name(err));
    }
    bool storage_ok = vault_nvs_storage_init();
    if (!storage_ok) {
        ESP_LOGE(TAG, "NVS storage init failed; vault will run in-RAM only this boot");
    }

    dispatcher_deps_t deps = {
        .rng = rng_fill,
        .confirm_export = confirm_export_on_device,
    };
    dispatcher_init(&deps);

    /* Bring up BLE (and with it, the hardware RNG's documented full-entropy
     * precondition) before touching the RNG at all. */
    ble_gatt_start();
    rng_self_test();

    vault_init(storage_ok ? vault_nvs_storage() : NULL, now_seconds);

    /* ui_task_start() must come after vault_init() (so ui_task never
     * browses a not-yet-populated vault) but can otherwise run any time
     * before the transports start accepting commands. */
    ui_task_start();

    serial_cdc_start();
}
