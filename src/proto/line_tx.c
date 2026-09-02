#include "line_tx.h"

static bool write_all(const uint8_t *data, size_t len, line_tx_write_fn write_fn,
                      line_tx_flush_fn flush_fn, line_tx_wait_fn wait_fn, void *ctx,
                      size_t *accepted) {
    size_t sent = 0;
    while (sent < len) {
        size_t queued = write_fn(ctx, data + sent, len - sent);
        /* The callback contract is to return at most len. Fail closed if a
         * driver violates it rather than walking past the caller's buffer. */
        if (queued > len - sent) {
            return false;
        }
        if (queued > 0) {
            sent += queued;
            *accepted += queued;
            flush_fn(ctx);
        } else if (!wait_fn(ctx)) {
            return false;
        }
    }
    return true;
}

void line_tx_init(line_tx_t *tx) {
    tx->needs_resync = false;
}

line_tx_result_t line_tx_send(line_tx_t *tx, const uint8_t *payload, size_t len,
                              line_tx_write_fn write_fn, line_tx_flush_fn flush_fn,
                              line_tx_wait_fn wait_fn, void *ctx) {
    static const uint8_t newline = '\n';
    size_t accepted = 0;

    if (tx->needs_resync) {
        size_t recovery_accepted = 0;
        if (!write_all(&newline, 1, write_fn, flush_fn, wait_fn, ctx,
                       &recovery_accepted)) {
            /* Do not begin a new response until the old torn line has been
             * terminated. Otherwise this call would recreate the corruption
             * the recovery marker exists to prevent. */
            return LINE_TX_DROPPED;
        }
        tx->needs_resync = false;
    }

    if (!write_all(payload, len, write_fn, flush_fn, wait_fn, ctx, &accepted) ||
        !write_all(&newline, 1, write_fn, flush_fn, wait_fn, ctx, &accepted)) {
        if (accepted > 0) {
            tx->needs_resync = true;
            return LINE_TX_DROPPED_PARTIAL;
        }
        return LINE_TX_DROPPED;
    }

    return LINE_TX_OK;
}
