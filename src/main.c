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
#include "board.h"
#include "buttons.h"
#include "device_reboot.h"
#include "dispatcher.h"
#include "display.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_storage.h"
#include "ota.h"
#include "release_key.h"
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

static uint64_t free_heap_bytes(void) {
    return (uint64_t)esp_get_free_heap_size();
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
 * the buttons concurrently.
 *
 * dispatcher_handle() — which calls straight into this — runs from inside
 * vault_lock_acquire()/release() in both transports (serial_cdc.c's
 * handle_rx(), itself nested inside TinyUSB's own tud_task(), and
 * ble_gatt.c's NimBLE host callback), so without the release/acquire pair
 * below, a client that sends export_secret and never taps a button would
 * hold vault_lock — and stall the entire USB or BLE transport task, not
 * just this one response — for up to 30s. That directly contradicts
 * vault_lock.h's own header comment ("never something like ... the 30s
 * remote-confirm wait") and serial_cdc.c's own point #1 (never block
 * inside handle_rx()); this was a real gap between what those two files
 * document and what main.c actually wired up, not a hypothetical. Releasing
 * the lock around just the wait is safe: vault_export_secret() (called by
 * dispatcher.c right after this returns) independently re-checks the note
 * is still CONFIRMED, so a concurrent discard/mark_spent racing the confirm
 * window fails closed with invalid_state instead of exporting stale state —
 * the same re-validate-after-reacquire pattern ui_task.c's own unveil()
 * already uses for the identical local-browsing race. */
static confirm_result_t confirm_export_on_device(const note_meta_t *note) {
    vault_lock_release();
    confirm_result_t result = ui_task_request_remote_confirm(note, 30000);
    vault_lock_acquire();
    return result;
}

/* Same gate, for an incoming OTA image instead of a note export — see
 * dispatcher.h's ota_approve_fn comment. size_bytes isn't shown on-screen
 * yet either, same limitation as confirm_export_on_device above.
 *
 * Deliberately NOT given the same vault_lock_release()/acquire() treatment
 * as confirm_export_on_device above, even though it has the exact same
 * up-to-30s blocking-while-locked problem: dispatcher.c's OTA session state
 * (g_ota in dispatcher.c) is plain static data with no re-validation or
 * locking of its own — unlike vault.c's notes, which vault_export_secret()
 * re-checks after the lock is reacquired. Today, vault_lock is the ONLY
 * thing serializing dispatcher_handle() calls across transports at all, so
 * releasing it here would let a second transport's ota_begin/ota_chunk
 * race this one's g_ota mutation with nothing to catch it — a new, less
 * understood bug in an already real-hardware-unverified path (see
 * README.md's "OTA firmware updates" section), trading a known, bounded
 * problem for an unknown one. Fixing this for real needs g_ota to get its
 * own lock (or dispatcher.c to stop being lock-free internally) — a bigger
 * change than this session made, left as a follow-up. */
static confirm_result_t ota_approve_on_device(uint32_t size_bytes, uint32_t timeout_ms) {
    (void)size_bytes;
    return ui_task_request_ota_confirm(timeout_ms);
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
        .board = BOARD_NAME,
        .free_heap = free_heap_bytes,
        .reset = device_reboot_delayed,
        .ota_approve = ota_approve_on_device,
        .ota_write_begin = ota_write_begin,
        .ota_write_chunk = ota_write_chunk,
        .ota_write_finish = ota_write_finish,
        .ota_write_abort = ota_write_abort,
        .ota_pubkey = OTA_RELEASE_PUBKEY,
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
