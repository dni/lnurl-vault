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
#include <string.h>

#include "ble_gatt.h"
#include "board.h"
#include "buttons.h"
#include "cmd_lock.h"
#include "crash_crumb.h"
#include "device_reboot.h"
#include "dispatcher.h"
#include "display.h"
#include "display_selftest.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "identity.h"
#include "input_health.h"
#include "monocypher.h"
#include "nvs_storage.h"
#include "ota.h"
#include "release_key.h"
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

/* dispatcher.c calls this with the command name as each command starts, and
 * with NULL when it returns — see dispatcher.h's trace_cmd_fn, and note the
 * constraint there: the name only, never the arguments. */
static void trace_cmd(const char *cmd) {
    if (cmd) {
        crash_crumb_set_cmd(cmd);
    } else {
        crash_crumb_clear_cmd();
    }
}

/* Surfaced by get_info. On a board with no console (the classic T-Display
 * builds CONFIG_ESP_CONSOLE_NONE, because UART0 carries the protocol), this
 * is the only way to find out why a device in someone's pocket rebooted. */
static bool boot_report(boot_report_t *out) {
    out->reset_reason = crash_crumb_reset_reason();
    out->last_cmd = crash_crumb_last_cmd();
    out->boot_count = crash_crumb_boot_count();
    out->unexpected = crash_crumb_last_boot_was_unexpected();
    return true;
}

/* get_info's `inputs`. Unconditional, so a missing field means "this build
 * cannot observe its buttons", not "they are fine". */
static bool input_report(input_report_t *out) {
    /* Only report buttons this board actually has: a one-button board would
     * otherwise claim a healthy cancel button that is not there, and a client
     * would tell its owner to press it. */
    const uint8_t buttons = board_input_caps().buttons;
    out->confirm = buttons >= 1 ? input_health_name(buttons_input_state(INPUT_CONFIRM)) : NULL;
    out->cancel = buttons >= 2 ? input_health_name(buttons_input_state(INPUT_CANCEL)) : NULL;
    return out->confirm != NULL || out->cancel != NULL;
}

/* get_info's `capabilities`. Display size is zero when the panel did not come
 * up, not its compiled-in dimensions: a client deciding whether a QR handoff
 * fits needs the pixels there ARE. Both transports are unconditional in
 * app_main(), so both are always reported -- if that changes, so must this. */
static bool capability_report(capability_report_t *out) {
    const board_input_caps_t caps = board_input_caps();
    const bool panel = display_ready();
    out->buttons = caps.buttons;
    out->touch = caps.touch;
    out->display_width = panel ? (uint16_t)display_width() : 0;
    out->display_height = panel ? (uint16_t)display_height() : 0;
    out->serial = true;
    out->ble = true;
    return true;
}

/* This device's identity key (#69). Generated once, kept in NVS, never
 * disclosed -- only what it derives. */
static uint8_t g_identity_seed[IDENTITY_SEED_LEN];
static bool g_identity_ready;

static bool identity_seed(uint8_t out[IDENTITY_SEED_LEN]) {
    if (!g_identity_ready) {
        return false;
    }
    memcpy(out, g_identity_seed, IDENTITY_SEED_LEN);
    return true;
}

/* Must run after ble_gatt_start(), which is what satisfies the RNG's
 * documented full-entropy precondition -- see rng_self_test(). */
static void identity_boot(void) {
    if (vault_nvs_identity_load(g_identity_seed) && !identity_seed_is_blank(g_identity_seed)) {
        g_identity_ready = true;
        return;
    }
    if (!rng_fill(g_identity_seed, sizeof(g_identity_seed)) ||
        identity_seed_is_blank(g_identity_seed)) {
        ESP_LOGE(TAG, "identity: could not generate a key; identify will report unsupported");
        crypto_wipe(g_identity_seed, sizeof(g_identity_seed));
        return;
    }
    /* Refuse to serve an identity that cannot be remembered. A host that
     * pinned a key the device forgets at the next boot would warn about a
     * swapped vault every time it reconnected, which trains people to
     * dismiss exactly the warning this exists to raise. */
    if (!vault_nvs_identity_save(g_identity_seed)) {
        ESP_LOGE(TAG, "identity: generated a key but could not store it; not serving it");
        crypto_wipe(g_identity_seed, sizeof(g_identity_seed));
        return;
    }
    g_identity_ready = true;
    ESP_LOGI(TAG, "identity: generated and stored a new device key");
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
 * This used to hold vault_lock for the whole approval wait, and said so:
 * the reason it could not simply release it was that vault_lock was then
 * the ONLY thing serializing dispatcher_handle() across transports, so
 * releasing it would have let a second transport's ota_begin/ota_chunk race
 * this one's g_ota mutation, which has no re-validation of its own the way
 * vault.c's notes do. Holding it, though, is the deadlock in issue #4: the
 * task on the other side of this wait is ui_task, and ui_task takes
 * vault_lock for its own note browsing — so it can be blocked on the very
 * lock this call is holding, never reach the queue it is being waited on,
 * and wedge both tasks until the vault is power-cycled.
 *
 * cmd_lock (see cmd_lock.h) resolves it by giving those two jobs separate
 * locks. The transports now hold cmd_lock across the whole command,
 * including this wait, so g_ota keeps exactly the protection it had; and
 * vault_lock, which ui_task actually contends for, is released around the
 * wait here just as confirm_export_on_device does above. */
static confirm_result_t ota_approve_on_device(uint32_t size_bytes, uint32_t timeout_ms) {
    (void)size_bytes;
    vault_lock_release();
    confirm_result_t result = ui_task_request_ota_confirm(timeout_ms);
    vault_lock_acquire();
    return result;
}

/* Same gate again, for `wipe`. Releases vault_lock across the wait for
 * exactly the reasons the other two do -- ui_task is the task on the far
 * side of it and takes vault_lock for its own browsing, so holding it here
 * would wedge both. cmd_lock stays held throughout; see cmd_lock.h. */
static confirm_result_t wipe_approve_on_device(uint32_t timeout_ms) {
    vault_lock_release();
    confirm_result_t result = ui_task_request_wipe_confirm(timeout_ms);
    vault_lock_acquire();
    return result;
}

/* The gate in front of mark_spent, delete, discard and rename -- see
 * dispatcher.h's action_confirm_fn. Releases vault_lock across the wait for
 * the same reason the other three do: ui_task is on the far side of it and
 * takes vault_lock for its own browsing, so holding it here would wedge both.
 * cmd_lock stays held throughout; see cmd_lock.h. */
static confirm_result_t confirm_action_on_device(const char *action, const note_meta_t *note) {
    vault_lock_release();
    confirm_result_t result = ui_task_request_action_confirm(action, note, 30000);
    vault_lock_acquire();
    return result;
}

void app_main(void) {
    /* First: reads what the previous boot left in RTC memory before anything
     * this boot can overwrite or crash into it. */
    crash_crumb_boot();

    buttons_init();
    display_init();
    vault_lock_init();
    cmd_lock_init();

    /* A failure here is NOT recovered by erasing -- see vault_nvs_boot(). The
     * notes are still on flash; the device comes up able to say so
     * (get_info's `storage` field) and nothing else. */
    esp_err_t err = vault_nvs_boot();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage unavailable (%s): reporting storage=%s, NOT erasing",
                 esp_err_to_name(err), vault_nvs_state_name());
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
        .wipe_approve = wipe_approve_on_device,
        .wipe_storage = vault_nvs_wipe,
        .storage_state = vault_nvs_state_name,
        .trace_cmd = trace_cmd,
        .boot_report = boot_report,
        .confirm_action = confirm_action_on_device,
        .input_report = input_report,
        .capabilities = capability_report,
        .identity_seed = identity_seed,
    };
    dispatcher_init(&deps);

    /* Create ui_task's request queue before any transport starts, so a gated
     * command arriving during boot (BLE comes up next, and vault_init below
     * loads NVS) is queued for the task rather than sent to a NULL handle. The
     * task itself still starts last, see ui_task_start() below. */
    ui_task_init();

    /* Bring up BLE (and with it, the hardware RNG's documented full-entropy
     * precondition) before touching the RNG at all. */
    ble_gatt_start();
    rng_self_test();

    /* After ble_gatt_start()/rng_self_test() above, and after storage is up:
     * the key is generated from the same RNG the notes use and has to be
     * stored before it is served. */
    identity_boot();

    if (storage_ok) {
        vault_init(vault_nvs_storage(), now_seconds);
    } else {
        /* Storage was expected but could not be brought up. Fail closed --
         * refuse to mint notes that would vanish at the next reset -- rather
         * than run in RAM, which is vault_init(NULL)'s test-only mode. */
        vault_init_storage_unavailable(now_seconds);
    }

    board_serial_start();

    /* Diagnostics sit between the transport and ui_task, which is the only
     * window where they get both properties they need. Run before the
     * transport, the QR ladder's per-code button wait left the device
     * unreachable over the wire for minutes. Run after ui_task, that task
     * polls the same buttons and owns the same display, so every press
     * advanced the ladder AND made ui_task repaint over the code being
     * examined. Both were observed on hardware. No-ops unless their build
     * flags are set.
     *
     * A gated command arriving during this window (or during BLE bring-up and
     * vault_init above) is queued -- ui_task_init() created the queue before
     * any transport started -- and serviced once ui_task_start() runs below;
     * the calling transport simply waits for it. */
    display_selftest_run();
    qr_selftest_run();

    /* Last: after vault_init() so it never browses a not-yet-populated
     * vault, and after the diagnostics so it never fights them for the
     * display. */
    ui_task_start();
}
