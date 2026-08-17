#ifndef LNURLVAULT_BLE_FRAME_H
#define LNURLVAULT_BLE_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Reassembles docs/PROTOCOL.md's BLE framing from a stream of GATT writes.
 *
 * The framing is:
 *
 *   [2-byte little-endian payload length][payload bytes...]
 *
 * and a client is free to split that however it likes across writes: the
 * negotiated ATT MTU is not known to the sender until negotiation finishes,
 * writes may be as small as one byte, and several small messages may share a
 * single write. Nothing in the framing promises alignment between message
 * boundaries and write boundaries, so nothing here may assume any.
 *
 * This lives in src/proto (portable C, no ESP-IDF) rather than inside
 * transport/ble_gatt.c for the same reason line_proto.c does for the serial
 * transports: the reassembly is where the fiddly edge cases are, and here it
 * can be driven directly by test/native/test_ble_frame.c one byte at a time.
 * When it lived in the transport it could only be exercised by pushing bytes
 * at a real radio, and four of its edge cases were wrong -- see that test
 * file, which pins each of them.
 */

/* Longest payload accepted. Anything longer is refused rather than
 * truncated; see ble_frame_feed(). */
#ifndef BLE_FRAME_BUF_SIZE
#define BLE_FRAME_BUF_SIZE 4096
#endif

/* Called with one complete payload, NUL-terminated for the benefit of the
 * JSON parser downstream. `len` excludes that NUL. */
typedef void (*ble_frame_cb)(const char *msg, size_t len, void *ctx);

typedef struct {
    char buf[BLE_FRAME_BUF_SIZE];
    uint8_t hdr[2];
    uint8_t hdr_have; /* 0..2 length-header bytes seen; 2 means `want` is set */
    size_t want;      /* declared payload length, once the header is complete */
    size_t have;      /* payload bytes buffered so far */
    /* Payload bytes still to be swallowed from a message whose declared
     * length does not fit. Counting them out is what makes an oversized
     * message a *dropped* message rather than a desynchronised stream: the
     * bytes of its payload must not be re-read as the next message's length
     * header, which is exactly what discarding only the header would do. */
    size_t discard;
} ble_frame_t;

void ble_frame_init(ble_frame_t *f);

/* Abandons any partially received message, and any message still being
 * refused. For a transport that has just dropped bytes it could not take (an
 * oversized single write) or lost the link: continuing to reassemble across
 * the gap would splice two unrelated byte runs into one command. */
void ble_frame_reset(ble_frame_t *f);

/* Feeds `len` received bytes. Invokes `cb` once per complete payload, in
 * order -- several times for one write, or not at all for many writes.
 *
 * A declared length of zero is a complete (empty) message: it delivers
 * nothing and the next byte starts a new header. A declared length larger
 * than BLE_FRAME_BUF_SIZE - 1 is refused, and its payload swallowed, so the
 * stream stays in sync. Neither case ever hands a partial payload to `cb`:
 * half a command that still parses as JSON could name a different note than
 * the sender meant. */
void ble_frame_feed(ble_frame_t *f, const uint8_t *data, size_t len, ble_frame_cb cb, void *ctx);

#endif
