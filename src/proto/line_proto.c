#include "line_proto.h"

#include <stdbool.h>

void line_proto_init(line_proto_t *lp) {
    lp->len = 0;
    lp->overflowed = false;
}

void line_proto_feed(line_proto_t *lp, const uint8_t *data, size_t len, line_proto_cb cb,
                      void *ctx) {
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];

        if (c == '\n' || c == '\r') {
            if (lp->overflowed) {
                /* Drop the whole line: we hold only its first
                 * LINE_PROTO_BUF_SIZE-1 bytes, and a truncated command that
                 * happens to still parse is more dangerous than no command --
                 * it could name a different note than the sender meant. */
                lp->len = 0;
                lp->overflowed = false;
                continue;
            }
            if (lp->len > 0) {
                lp->buf[lp->len] = '\0';
                cb(lp->buf, ctx);
                lp->len = 0;
            }
            continue;
        }

        if (lp->len + 1 < LINE_PROTO_BUF_SIZE) {
            lp->buf[lp->len++] = c;
        } else {
            lp->overflowed = true;
        }
    }
}
