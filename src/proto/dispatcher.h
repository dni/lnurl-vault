#ifndef LNURLVAULT_DISPATCHER_H
#define LNURLVAULT_DISPATCHER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vault.h"

/* Transport-agnostic command router: both transport/serial_cdc.c and
 * transport/ble_gatt.c read a complete JSON message off the wire (framed
 * differently per transport, see docs/PROTOCOL.md) and hand it to
 * dispatcher_handle() unchanged, then send whatever it writes back over
 * that same transport. Neither transport, nor this module, know anything
 * about NVS or hardware RNG directly — those are injected via
 * dispatcher_deps_t / vault_init(), which is what makes both this module
 * and vault.c runnable from test/native/ without ESP-IDF. */

/* Only CONFIRM_YES is approval. Every caller in dispatcher.c treats anything
 * else as a refusal, deliberately -- see confirm_error_code(). They used to
 * check for CONFIRM_NO and CONFIRM_TIMEOUT and proceed if it was neither,
 * which made the safety of every disclosure depend on this enum never gaining
 * a value. */
typedef enum {
    CONFIRM_YES,
    CONFIRM_NO,
    CONFIRM_TIMEOUT,
    /* The device cannot ask. Today that means the panel never came up, so
     * there is nothing to show the owner and no informed consent to be had --
     * see display.h's display_ready(), which says anything disclosing a
     * secret MUST check it and refuse rather than proceed. Reported as its
     * own error rather than as user_declined, because nobody declined and a
     * client that cannot tell the difference sends its owner hunting for a
     * button they were never shown. */
    CONFIRM_UNAVAILABLE,
} confirm_result_t;

/* Shows note details on-device and waits for a physical confirm/cancel or a
 * timeout, before export_secret is allowed to disclose a plaintext secret.
 * NULL => always allow (used by native tests, where there's no display). */
typedef confirm_result_t (*export_confirm_fn)(const note_meta_t *note);

/* Optional: reports current free heap in bytes, surfaced as get_info's
 * free_heap_bytes field when set. NULL => field omitted (native tests have
 * no heap of their own to report). Added to chase a real, reproducible
 * hardware issue where serial responses get flakier the longer a boot runs
 * — see README.md's Status section — not just for its own sake, but it's
 * cheap and generally useful for a long-running device to report this. */
typedef uint64_t (*free_heap_fn)(void);

/* Optional: reboots the device, surfaced as the `reset` command. NULL =>
 * `reset` responds ok:true but does nothing further (native tests have no
 * device to reboot). Not a fix for the still-unresolved serial flakiness
 * (see README.md's Status section) — a mitigation: the failure pattern
 * documented there gets worse the longer a boot runs and improves after a
 * power cycle, and this gives a remote client (or a human) a way to force
 * that power cycle without physical access to the board. Wired to a
 * *delayed* esp_restart() in main.c, not an immediate one — see that
 * file's comment for why immediate would race the response actually
 * leaving the TX buffer. */
typedef void (*reset_fn)(void);

/* OTA: firmware updates over the same JSON-over-serial wire protocol as
 * everything else (see docs/PROTOCOL.md's `ota_begin`/`ota_chunk`/
 * `ota_finish`), adapted from forgesworn/heartwood-esp32's design —
 * release-signed images (ed25519, verified twice: once at ota_begin over
 * the claimed digest before the owner is even asked, again at ota_finish
 * over the digest actually written to flash), gated by the same physical
 * confirm/cancel pattern export_secret already uses. dispatcher.c owns the
 * portable parts (parsing, base64, signature verification via
 * src/ota/ota_sign.c, session sequencing) and is unit-tested against these
 * dependencies with a fake in-memory "flash"; only the ESP-IDF-specific
 * parts (esp_ota_* calls, the physical approval prompt) are injected here,
 * same pattern as confirm_export/free_heap/reset above. */

/* Physical approval gate for an incoming image — shows its size and waits
 * for a confirm/cancel tap or a timeout, same shape as export_confirm_fn.
 * NULL => always allow (native tests, no display). */
typedef confirm_result_t (*ota_approve_fn)(uint32_t size_bytes, uint32_t timeout_ms);

/* Opens the inactive OTA partition for total_size bytes of sequential
 * writes. False => e.g. the image is too big for the partition. */
typedef bool (*ota_write_begin_fn)(uint32_t total_size);

/* Writes the next sequential chunk (dispatcher.c enforces strict in-order
 * offsets before ever calling this — see handle_ota_chunk). False => a
 * flash write failure. */
typedef bool (*ota_write_chunk_fn)(const uint8_t *data, size_t len);

/* Finalizes a fully-received, re-verified image: esp_ota_end +
 * esp_ota_set_boot_partition, then schedules a reboot into it (see
 * device_reboot.h — same delayed-reboot reasoning as reset_fn above, so
 * ota_finish's "ok" response has a real chance to reach the client first).
 * False => finalization itself failed; the running firmware is unaffected
 * either way since the boot partition is never switched until this
 * succeeds. */
typedef bool (*ota_write_finish_fn)(void);

/* Discards an in-progress session (declined approval, a size/signature
 * mismatch caught at ota_finish, or any other abandoned session) without
 * ever touching the boot partition. */
typedef void (*ota_write_abort_fn)(void);

/* Optional: the physical confirmation in front of `wipe`. Same shape as
 * ota_approve_fn, deliberately its own dependency rather than a shared one,
 * because what the screen should say for "erase everything you own" is not
 * what it should say for "accept a firmware image", and sharing the hook
 * would quietly rule that out. NULL => `wipe` refuses outright rather than
 * proceeding ungated: an unconfirmable wipe is not one to grant. */
typedef confirm_result_t (*wipe_approve_fn)(uint32_t timeout_ms);

/* Optional: erases persistent storage AND verifies it is gone, returning
 * false if either the erase or the verification failed. NULL => `wipe` is
 * reported unsupported.
 *
 * A false return must never be reported to the client as success and must
 * never be followed by a reboot -- see nvs_storage.h's vault_nvs_wipe, and
 * heartwood-esp32's persistent_wipe.rs, whose header makes the same demand
 * of its callers. */
typedef bool (*wipe_storage_fn)(void);

/* Optional: how storage came up this boot ("ok", "full",
 * "version_unsupported", "unavailable" -- see nvs_storage.h), reported by
 * get_info. NULL omits the field.
 *
 * This exists because the alternative to erasing a full partition is
 * refusing to, and a device that refuses must be able to say so. Otherwise
 * it presents as an empty working vault while holding every note on flash,
 * unread -- and the owner concludes their notes are gone. */
typedef const char *(*storage_state_fn)(void);
/* Optional: called with the command NAME as each command starts and ends,
 * so the firmware can leave a breadcrumb naming what was in flight if the
 * device goes down mid-command (see src/crash_crumb.h). NULL => not traced,
 * which is what native tests use.
 *
 * The name only, never the line it came from: import_secret carries a raw
 * secret in its arguments, and the breadcrumb survives a reset. Anything
 * wired here inherits that constraint. */
typedef void (*trace_cmd_fn)(const char *cmd);

/* Optional: describes the previous boot, surfaced by get_info as
 * last_reset_reason / boot_count / last_cmd_in_flight. NULL => the fields
 * are omitted. last_cmd may be NULL even when this is set, meaning the
 * device went down cleanly or not at all. */
typedef struct {
    const char *reset_reason;
    const char *last_cmd; /* NULL if nothing was in flight */
    uint32_t boot_count;
    bool unexpected;
} boot_report_t;

typedef bool (*boot_report_fn)(boot_report_t *out);

/* Optional: the physical gate in front of a command that changes or destroys
 * a note the owner already has -- mark_spent, delete, discard, rename.
 *
 * These were ungated. BLE has no bonding and no passkey, so any central in
 * radio range is already a client, and the README's reasoning for accepting
 * that was that a secret cannot be extracted without a physical gesture. True,
 * and beside the point: an attacker who cannot read a single secret could
 * still mark every live note spent and then delete them, destroying real value
 * without ever learning one. Issue #16.
 *
 * `action` is the command name, so the screen can say which of them is being
 * approved -- approving a delete while believing you are approving an export
 * would be worse than not asking at all. `note` is what it will happen to, and
 * may be NULL if the note could not be read.
 *
 * NULL => these commands run ungated, which is the old behaviour and what the
 * native tests use. */
typedef confirm_result_t (*action_confirm_fn)(const char *action, const note_meta_t *note);

typedef struct {
    vault_rng_fn rng;
    export_confirm_fn confirm_export;
    /* Board identifier reported by get_info, so a client -- and a bug report
     * -- can say which hardware and which pin map are in play. Injected
     * rather than looked up, because this module stays free of any
     * board/ESP-IDF dependency. NULL omits the field. */
    const char *board;
    free_heap_fn free_heap;
    reset_fn reset;
    ota_approve_fn ota_approve;
    ota_write_begin_fn ota_write_begin;
    ota_write_chunk_fn ota_write_chunk;
    ota_write_finish_fn ota_write_finish;
    ota_write_abort_fn ota_write_abort;
    /* Release public key (32 bytes) OTA images must be signed against.
     * NULL => `ota_begin` always fails closed with bad_request, the same
     * "no key configured yet" fail-closed default
     * forgesworn/heartwood-esp32's placeholder all-zero key gives —
     * see docs/ota-signing.md-equivalent notes in README.md once a real
     * release key exists. */
    const uint8_t *ota_pubkey;
    wipe_approve_fn wipe_approve;
    wipe_storage_fn wipe_storage;
    storage_state_fn storage_state;
    trace_cmd_fn trace_cmd;
    boot_report_fn boot_report;
    action_confirm_fn confirm_action;
} dispatcher_deps_t;

void dispatcher_init(const dispatcher_deps_t *deps);

/* Parses one complete JSON command object and writes a NUL-terminated JSON
 * response into out (outcap bytes) — always writes something valid, even a
 * bad_request error, never leaves out unwritten. */
void dispatcher_handle(const char *line, char *out, size_t outcap);

#endif
