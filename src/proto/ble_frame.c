#include "ble_frame.h"

#include <string.h>

void ble_frame_init(ble_frame_t *f) {
    ble_frame_reset(f);
}

void ble_frame_reset(ble_frame_t *f) {
    f->hdr_have = 0;
    f->want = 0;
    f->have = 0;
    /* Including a part-drained discard: reset is only called once the byte
     * stream itself is known to be broken (bytes dropped, or the link gone),
     * and a byte count taken from before the break cannot be trusted to land
     * on a message boundary after it. */
    f->discard = 0;
}

void ble_frame_feed(ble_frame_t *f, const uint8_t *data, size_t len, ble_frame_cb cb, void *ctx) {
    size_t i = 0;
    while (i < len) {
        /* Swallowing the payload of a refused (over-long) message. */
        if (f->discard > 0) {
            size_t n = len - i;
            if (n > f->discard) {
                n = f->discard;
            }
            f->discard -= n;
            i += n;
            continue;
        }

        /* Collecting the 2-byte length header, one byte at a time because a
         * write may carry only the first of them. */
        if (f->hdr_have < 2) {
            f->hdr[f->hdr_have++] = data[i++];
            if (f->hdr_have < 2) {
                continue;
            }
            size_t want = (size_t)f->hdr[0] | ((size_t)f->hdr[1] << 8);
            if (want == 0) {
                /* A complete message with no payload. There is nothing to
                 * dispatch, but it is not a framing error either, so resync
                 * on the next byte rather than stalling here forever waiting
                 * for a payload that is never coming. */
                f->hdr_have = 0;
                continue;
            }
            if (want > BLE_FRAME_BUF_SIZE - 1) {
                f->discard = want;
                f->hdr_have = 0;
                continue;
            }
            f->want = want;
            f->have = 0;
            continue;
        }

        /* Payload. */
        size_t n = len - i;
        size_t room = f->want - f->have;
        if (n > room) {
            n = room;
        }
        memcpy(f->buf + f->have, data + i, n);
        f->have += n;
        i += n;

        if (f->have == f->want) {
            f->buf[f->have] = '\0';
            cb(f->buf, f->have, ctx);
            /* Reset before returning to the loop: any bytes left in this
             * write belong to the next message's header, not to this one. */
            f->hdr_have = 0;
            f->want = 0;
            f->have = 0;
        }
    }
}
