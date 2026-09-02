#ifndef LNURLVAULT_LINE_TX_H
#define LNURLVAULT_LINE_TX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Sends one payload followed by docs/PROTOCOL.md's newline delimiter.
 *
 * Native USB-CDC can accept only part of a response at a time. If that link
 * stops making progress after accepting a prefix, abandoning the write
 * without repairing the delimiter makes the next response continue the same
 * line. The receiver then sees two replies glued together as invalid JSON.
 *
 * line_tx_t remembers that torn-line state. Before any later response it
 * writes a newline on its own, restoring the framing boundary. The transport
 * supplies the write/flush/wait operations, so the state machine has no
 * ESP-IDF dependency and can be tested on the host.
 */

typedef size_t (*line_tx_write_fn)(void *ctx, const uint8_t *data, size_t len);
typedef void (*line_tx_flush_fn)(void *ctx);

/* Called only after write_fn returned zero. Return true to retry, false to
 * abandon this response. An ESP transport normally delays here and returns
 * false when its bounded deadline expires. */
typedef bool (*line_tx_wait_fn)(void *ctx);

typedef struct {
    bool needs_resync;
} line_tx_t;

typedef enum {
    LINE_TX_OK,
    /* No byte of this response was accepted. The existing stream boundary is
     * intact (unless an older torn line still awaits resynchronisation). */
    LINE_TX_DROPPED,
    /* At least one payload byte was accepted, but its terminating newline was
     * not. The next successful send will emit a recovery newline first. */
    LINE_TX_DROPPED_PARTIAL,
} line_tx_result_t;

void line_tx_init(line_tx_t *tx);

line_tx_result_t line_tx_send(line_tx_t *tx, const uint8_t *payload, size_t len,
                              line_tx_write_fn write_fn, line_tx_flush_fn flush_fn,
                              line_tx_wait_fn wait_fn, void *ctx);

#endif
