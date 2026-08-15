/* NOTE: unverified by compilation (see README.md, "Status: unverified by
 * compilation"). This file wires together modules with very different
 * confidence levels: dispatcher.c/vault.c and everything under src/vault
 * and src/proto are exercised by test/native/ and actually pass (93/93
 * assertions, see README.md's Verification section); nvs_storage.c,
 * serial_cdc.c, ble_gatt.c, display.c, and buttons.c are ESP-IDF-specific
 * and have not been compiled here — see each file's own header comment for
 * which parts are most likely to need reconciling against your installed
 * IDF version. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ble_gatt.h"
#include "board_pins.h"
#include "buttons.h"
#include "dispatcher.h"
#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_storage.h"
#include "serial_cdc.h"
#include "vault.h"

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

/* The physical confirm/cancel gate in front of vault_export_secret. Note
 * details aren't shown on-screen yet (see display.c's header comment) but
 * the gate itself — a real button press required, with a timeout — is
 * fully functional regardless. */
static confirm_result_t confirm_export_on_device(const note_meta_t *note) {
    (void)note;
    display_set_state(DISPLAY_STATE_CONFIRM_PENDING);
    confirm_result_t result = buttons_wait_confirm(30000);
    display_set_state(result == CONFIRM_YES ? DISPLAY_STATE_APPROVED : DISPLAY_STATE_DECLINED);
    vTaskDelay(pdMS_TO_TICKS(800));
    display_set_state(DISPLAY_STATE_IDLE);
    return result;
}

void app_main(void) {
    buttons_init();
    display_init();

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

    serial_cdc_start();
}
