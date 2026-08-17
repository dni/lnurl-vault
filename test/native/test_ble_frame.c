/* Pins the reassembly of docs/PROTOCOL.md's BLE framing.
 *
 * Every case below except the first was wrong in the version of this logic
 * that lived inline in src/transport/ble_gatt.c, and none of them could be
 * reached from a test while it lived there -- they needed a real radio and a
 * client willing to write bytes in an awkward shape. Extracting the
 * reassembly into src/proto/ble_frame.c is what makes them ordinary unit
 * tests; each one below names the specific behaviour that used to be wrong,
 * so a future rewrite has to keep it. */
#include <stdio.h>
#include <string.h>

#include "ble_frame.h"
#include "unity_lite.h"

#define MAX_CAPTURED 8

typedef struct {
    int count;
    char msg[MAX_CAPTURED][128];
    size_t len[MAX_CAPTURED];
    size_t total_bytes; /* so the big-payload case can assert without a 4KB copy */
} capture_t;

static void on_msg(const char *msg, size_t len, void *ctx) {
    capture_t *c = (capture_t *)ctx;
    c->total_bytes += len;
    if (c->count < MAX_CAPTURED) {
        size_t copy = len < sizeof(c->msg[0]) - 1 ? len : sizeof(c->msg[0]) - 1;
        memcpy(c->msg[c->count], msg, copy);
        c->msg[c->count][copy] = '\0';
        c->len[c->count] = len;
    }
    c->count++;
}

/* Builds [2-byte LE length][payload] into `out`, returning its total size. */
static size_t frame(uint8_t *out, const char *payload) {
    size_t n = strlen(payload);
    out[0] = (uint8_t)(n & 0xFF);
    out[1] = (uint8_t)((n >> 8) & 0xFF);
    memcpy(out + 2, payload, n);
    return n + 2;
}

static void test_single_write(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    uint8_t buf[64];
    size_t n = frame(buf, "{\"cmd\":\"get_info\"}");
    ble_frame_feed(&f, buf, n, on_msg, &c);

    UL_CHECK(c.count == 1, "one whole message in one write delivers once");
    UL_CHECK(strcmp(c.msg[0], "{\"cmd\":\"get_info\"}") == 0, "payload survives intact");
    UL_CHECK(c.len[0] == 18, "reported length excludes the framing and the NUL");
}

/* A client that has not yet negotiated an MTU may write a single byte, and
 * the first write of a message may therefore carry only half the length
 * header. The old inline version bailed out of any write shorter than two
 * bytes and lost the message that followed. */
static void test_header_split_across_writes(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    uint8_t buf[64];
    size_t n = frame(buf, "{\"cmd\":\"get_info\"}");
    for (size_t i = 0; i < n; i++) {
        ble_frame_feed(&f, buf + i, 1, on_msg, &c);
    }

    UL_CHECK(c.count == 1, "a message written one byte at a time still arrives");
    UL_CHECK(strcmp(c.msg[0], "{\"cmd\":\"get_info\"}") == 0, "and arrives intact");
}

/* Nothing in the framing aligns message boundaries to write boundaries, so a
 * client may pack several into one write. The old inline version delivered
 * the first and silently discarded everything after it. */
static void test_two_messages_in_one_write(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    uint8_t buf[128];
    size_t n = frame(buf, "first");
    n += frame(buf + n, "second");
    ble_frame_feed(&f, buf, n, on_msg, &c);

    UL_CHECK(c.count == 2, "both messages in one write are delivered");
    UL_CHECK(strcmp(c.msg[0], "first") == 0, "first message intact");
    UL_CHECK(strcmp(c.msg[1], "second") == 0, "second message intact");
}

/* The one that matters most, and the reason the writes below are sized so
 * deliberately: refusing an over-long message must also consume its payload.
 *
 * The old inline version zeroed the declared length and returned having
 * consumed none of the payload, so the payload's own bytes were then read as
 * though they were the next message's length header. A sender controls its
 * own write boundaries, so it can arrange for the first write of that
 * payload to be a complete, well-formed frame -- and the device dispatches
 * the command inside it, byte for byte, having never been sent it as a
 * message at all. Checked against a transcription of the old code: it
 * dispatches exactly {"cmd":"reset"} for the writes below.
 *
 * So this is a security property, not tidiness. A refused message must
 * consume exactly its declared length and deliver nothing. */
static void test_oversized_message_cannot_smuggle_a_command(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    /* Write 1: a header declaring more than can ever be accepted. */
    const size_t declared = 5000;
    uint8_t hdr[2] = {(uint8_t)(declared & 0xFF), (uint8_t)(declared >> 8)};
    ble_frame_feed(&f, hdr, sizeof(hdr), on_msg, &c);

    /* Write 2: the first bytes of that refused payload, which the sender has
     * arranged to be a complete frame in their own right. */
    uint8_t smuggled[64];
    size_t smuggled_len = frame(smuggled, "{\"cmd\":\"reset\"}");
    ble_frame_feed(&f, smuggled, smuggled_len, on_msg, &c);

    UL_CHECK(c.count == 0, "a command buried in a refused payload is not dispatched");

    /* Writes 3..n: the rest of the refused payload. */
    static uint8_t filler[512];
    memset(filler, 'A', sizeof(filler));
    for (size_t sent = smuggled_len; sent < declared;) {
        size_t n = declared - sent;
        if (n > sizeof(filler)) {
            n = sizeof(filler);
        }
        ble_frame_feed(&f, filler, n, on_msg, &c);
        sent += n;
    }
    UL_CHECK(c.count == 0, "nor is anything else in it");

    /* And the stream is still in sync for whatever legitimately follows. */
    uint8_t buf[64];
    size_t n = frame(buf, "after");
    ble_frame_feed(&f, buf, n, on_msg, &c);

    UL_CHECK(c.count == 1, "the stream resynchronises on the next real message");
    UL_CHECK(c.count == 1 && strcmp(c.msg[0], "after") == 0, "and that message is intact");
}

/* Same refusal, with the payload split across writes the way it would
 * actually arrive over the air -- the drain has to survive being interrupted
 * at an arbitrary byte, not just at a write boundary that happens to line up
 * with the declared length. */
static void test_oversized_payload_split_across_writes(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    static uint8_t payload[5000];
    memset(payload, 'B', sizeof(payload));
    frame(payload, "{\"cmd\":\"reset\"}");

    uint8_t hdr[2] = {(uint8_t)(sizeof(payload) & 0xFF), (uint8_t)(sizeof(payload) >> 8)};
    ble_frame_feed(&f, hdr, sizeof(hdr), on_msg, &c);

    /* 5000 does not divide by 143, so the last write is a short one and the
     * drain has to end mid-write. */
    for (size_t off = 0; off < sizeof(payload); off += 143) {
        size_t n = sizeof(payload) - off;
        ble_frame_feed(&f, payload + off, n < 143 ? n : 143, on_msg, &c);
    }
    UL_CHECK(c.count == 0, "nothing delivered while the refused payload drains");

    uint8_t buf[64];
    size_t n = frame(buf, "after");
    ble_frame_feed(&f, buf, n, on_msg, &c);
    UL_CHECK(c.count == 1 && strcmp(c.msg[0], "after") == 0,
             "resyncs exactly where the refused payload ended");
}

/* A zero-length message is well-formed, just empty.
 *
 * It has to be packed into the same write as the message behind it to show
 * the bug: on its own the old inline version happened to recover, because it
 * left its buffer empty and so re-read the next header correctly. Packed, it
 * consumed the following message as though it were that empty message's
 * payload, and since it only ever dispatched on `want > 0`, nothing was ever
 * delivered again on that connection -- every later byte piled into the same
 * buffer behind a length of zero. */
static void test_zero_length_message_does_not_wedge(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    uint8_t buf[64];
    buf[0] = 0; /* an empty message... */
    buf[1] = 0;
    size_t n = 2 + frame(buf + 2, "after"); /* ...immediately followed by a real one */
    ble_frame_feed(&f, buf, n, on_msg, &c);

    UL_CHECK(c.count == 1, "an empty message dispatches nothing and consumes nothing extra");
    UL_CHECK(c.count == 1 && strcmp(c.msg[0], "after") == 0,
             "the message packed behind it still arrives");
}

/* The largest payload that fits, to prove the boundary is off-by-none: the
 * buffer must still have room for the terminating NUL. */
static void test_largest_accepted_payload(void) {
    static ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    const size_t biggest = BLE_FRAME_BUF_SIZE - 1;
    uint8_t hdr[2] = {(uint8_t)(biggest & 0xFF), (uint8_t)(biggest >> 8)};
    ble_frame_feed(&f, hdr, sizeof(hdr), on_msg, &c);

    static uint8_t payload[BLE_FRAME_BUF_SIZE];
    memset(payload, 'x', biggest);
    ble_frame_feed(&f, payload, biggest, on_msg, &c);

    UL_CHECK(c.count == 1, "a payload of exactly BLE_FRAME_BUF_SIZE-1 is accepted");
    UL_CHECK(c.total_bytes == biggest, "at its full length");

    /* One byte more must be refused, not truncated into the same buffer. */
    ble_frame_init(&f);
    capture_t c2 = {0};
    const size_t too_big = BLE_FRAME_BUF_SIZE;
    uint8_t hdr2[2] = {(uint8_t)(too_big & 0xFF), (uint8_t)(too_big >> 8)};
    ble_frame_feed(&f, hdr2, sizeof(hdr2), on_msg, &c2);
    ble_frame_feed(&f, payload, sizeof(payload), on_msg, &c2);
    UL_CHECK(c2.count == 0, "one byte over the limit is refused, not truncated");
}

/* The transport calls this when it has had to drop bytes it could not take,
 * or when the link went away. Continuing across such a gap would splice two
 * unrelated byte runs into one command. */
static void test_reset_abandons_a_partial_message(void) {
    ble_frame_t f;
    ble_frame_init(&f);
    capture_t c = {0};

    uint8_t buf[64];
    size_t n = frame(buf, "{\"cmd\":\"get_info\"}");
    ble_frame_feed(&f, buf, 6, on_msg, &c); /* header plus 4 payload bytes */
    UL_CHECK(c.count == 0, "a partial message delivers nothing yet");

    ble_frame_reset(&f);
    ble_frame_feed(&f, buf + 6, n - 6, on_msg, &c);
    UL_CHECK(c.count == 0, "and after a reset its tail is not delivered either");

    ble_frame_reset(&f);
    n = frame(buf, "clean");
    ble_frame_feed(&f, buf, n, on_msg, &c);
    UL_CHECK(c.count == 1 && strcmp(c.msg[0], "clean") == 0, "a fresh message still works");
}

void test_ble_frame_run(void) {
    printf("-- ble_frame --\n");
    test_single_write();
    test_header_split_across_writes();
    test_two_messages_in_one_write();
    test_oversized_message_cannot_smuggle_a_command();
    test_oversized_payload_split_across_writes();
    test_zero_length_message_does_not_wedge();
    test_largest_accepted_payload();
    test_reset_abandons_a_partial_message();
}
