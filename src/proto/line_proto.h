#ifndef LNURLVAULT_LINE_PROTO_H
#define LNURLVAULT_LINE_PROTO_H

#include <stddef.h>
#include <stdint.h>

/* Reassembles newline-delimited JSON commands from a byte stream.
 *
 * Both serial transports carry docs/PROTOCOL.md's line protocol -- native
 * USB-CDC on chips with a USB-OTG peripheral, plain UART behind an external
 * USB bridge on chips without one. Only the bytes arrive differently; the
 * framing, the buffer discipline and the overlong-line behaviour are the same,
 * so they live here rather than being duplicated (and drifting) per transport.
 *
 * No ESP-IDF dependency, so this is exercised by test/native/ alongside the
 * rest of src/proto.
 */

#ifndef LINE_PROTO_BUF_SIZE
#define LINE_PROTO_BUF_SIZE 2048
#endif

/* Called with one complete, NUL-terminated command line. */
typedef void (*line_proto_cb)(const char *line, void *ctx);

typedef struct {
    char buf[LINE_PROTO_BUF_SIZE];
    size_t len;
    /* Set once a line has overflowed the buffer, so the truncated remainder
     * is discarded rather than being handed over as if it were a whole
     * command. A half-command that still parses is far worse than none: it
     * could name a different note than the sender meant. Cleared at the next
     * newline, which resynchronises the stream. */
    bool overflowed;
} line_proto_t;

void line_proto_init(line_proto_t *lp);

/* Feeds `len` received bytes. Invokes `cb` once per complete line, in order.
 * Empty lines are ignored. Both CR and LF terminate a line, so a host sending
 * CRLF produces one command, not one command and one empty line. */
void line_proto_feed(line_proto_t *lp, const uint8_t *data, size_t len, line_proto_cb cb,
                      void *ctx);

#endif
