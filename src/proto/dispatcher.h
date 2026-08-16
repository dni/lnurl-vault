#ifndef LNURLVAULT_DISPATCHER_H
#define LNURLVAULT_DISPATCHER_H

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

typedef struct {
    vault_rng_fn rng;
    export_confirm_fn confirm_export;
    /* Board identifier reported by get_info, so a client -- and a bug report
     * -- can say which hardware and which pin map are in play. Injected
     * rather than looked up, because this module stays free of any
     * board/ESP-IDF dependency. NULL omits the field. */
    const char *board;
    free_heap_fn free_heap;
} dispatcher_deps_t;

void dispatcher_init(const dispatcher_deps_t *deps);

/* Parses one complete JSON command object and writes a NUL-terminated JSON
 * response into out (outcap bytes) — always writes something valid, even a
 * bad_request error, never leaves out unwritten. */
void dispatcher_handle(const char *line, char *out, size_t outcap);

#endif
