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

typedef enum {
    CONFIRM_YES,
    CONFIRM_NO,
    CONFIRM_TIMEOUT,
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
} dispatcher_deps_t;

void dispatcher_init(const dispatcher_deps_t *deps);

/* Parses one complete JSON command object and writes a NUL-terminated JSON
 * response into out (outcap bytes) — always writes something valid, even a
 * bad_request error, never leaves out unwritten. */
void dispatcher_handle(const char *line, char *out, size_t outcap);

#endif
