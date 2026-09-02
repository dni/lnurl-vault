/* Host tests for the newline repair used by the S3's USB-CDC writer.
 *
 * A partial JSON reply without its newline poisons the next reply: the host
 * sees one glued, unparseable line. These tests model a link that accepts a
 * prefix and then stops, without needing an ESP32 or TinyUSB. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "line_tx.h"
#include "unity_lite.h"

typedef struct {
    uint8_t out[256];
    size_t out_len;
    size_t capacity;
    size_t max_chunk;
    size_t refill_per_wait;
    unsigned waits_left;
    unsigned flushes;
} fake_link_t;

static size_t fake_write(void *ctx, const uint8_t *data, size_t len) {
    fake_link_t *link = (fake_link_t *)ctx;
    size_t n = len;
    if (n > link->capacity) n = link->capacity;
    if (link->max_chunk > 0 && n > link->max_chunk) n = link->max_chunk;
    if (n > sizeof(link->out) - link->out_len) n = sizeof(link->out) - link->out_len;
    memcpy(link->out + link->out_len, data, n);
    link->out_len += n;
    link->capacity -= n;
    return n;
}

static void fake_flush(void *ctx) {
    fake_link_t *link = (fake_link_t *)ctx;
    link->flushes++;
}

static bool fake_wait(void *ctx) {
    fake_link_t *link = (fake_link_t *)ctx;
    if (link->waits_left == 0) return false;
    link->waits_left--;
    link->capacity += link->refill_per_wait;
    return true;
}

static line_tx_result_t send(line_tx_t *tx, fake_link_t *link, const char *payload) {
    return line_tx_send(tx, (const uint8_t *)payload, strlen(payload), fake_write,
                        fake_flush, fake_wait, link);
}

static void test_complete_line(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {.capacity = 256};

    UL_CHECK(send(&tx, &link, "{\"ok\":true}") == LINE_TX_OK,
             "a response that fits is sent");
    UL_CHECK(link.out_len == strlen("{\"ok\":true}\n") &&
                 memcmp(link.out, "{\"ok\":true}\n", link.out_len) == 0,
             "the newline is part of the successful send");
    UL_CHECK(!tx.needs_resync, "a complete line leaves the stream clean");
}

static void test_temporary_backpressure(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {
        .capacity = 3,
        .max_chunk = 3,
        .refill_per_wait = 3,
        .waits_left = 8,
    };

    UL_CHECK(send(&tx, &link, "{\"ok\":true}") == LINE_TX_OK,
             "bounded waits carry a response through temporary backpressure");
    UL_CHECK(link.out_len == strlen("{\"ok\":true}\n") &&
                 memcmp(link.out, "{\"ok\":true}\n", link.out_len) == 0,
             "chunked writes preserve every byte and their order");
    UL_CHECK(link.flushes > 1, "each accepted chunk kicks the transport");
}

static void test_drop_before_first_byte_keeps_boundary(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {0};

    UL_CHECK(send(&tx, &link, "first") == LINE_TX_DROPPED,
             "a permanently blocked link gives up before writing");
    UL_CHECK(!tx.needs_resync, "no accepted byte means no torn line to repair");

    link.capacity = 256;
    UL_CHECK(send(&tx, &link, "second") == LINE_TX_OK,
             "the next response can start normally");
    UL_CHECK(link.out_len == strlen("second\n") &&
                 memcmp(link.out, "second\n", link.out_len) == 0,
             "nothing from the dropped response leaked into it");
}

static void test_partial_drop_repairs_before_next_response(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {.capacity = 5};

    UL_CHECK(send(&tx, &link, "first-response") == LINE_TX_DROPPED_PARTIAL,
             "a stalled response reports that a prefix escaped");
    UL_CHECK(tx.needs_resync, "the torn line is remembered");
    UL_CHECK(link.out_len == 5 && memcmp(link.out, "first", 5) == 0,
             "the fixture contains the same prefix a browser would hold");

    link.capacity = 256;
    UL_CHECK(send(&tx, &link, "{\"ok\":true}") == LINE_TX_OK,
             "a later response succeeds once the link moves again");
    UL_CHECK(link.out_len == strlen("first\n{\"ok\":true}\n") &&
                 memcmp(link.out, "first\n{\"ok\":true}\n", link.out_len) == 0,
             "a recovery newline separates the torn prefix from valid JSON");
    UL_CHECK(!tx.needs_resync, "successful repair leaves the stream clean");
}

static void test_missing_terminator_is_also_partial(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {.capacity = strlen("whole-payload")};

    UL_CHECK(send(&tx, &link, "whole-payload") == LINE_TX_DROPPED_PARTIAL,
             "losing only the newline still marks the line torn");
    UL_CHECK(tx.needs_resync, "the missing terminator is repaired later");

    link.capacity = 256;
    UL_CHECK(send(&tx, &link, "next") == LINE_TX_OK,
             "the writer recovers after a missing terminator");
    UL_CHECK(link.out_len == strlen("whole-payload\nnext\n") &&
                 memcmp(link.out, "whole-payload\nnext\n", link.out_len) == 0,
             "the repair creates two distinct lines");
}

static void test_no_new_payload_before_repair(void) {
    line_tx_t tx;
    line_tx_init(&tx);
    fake_link_t link = {.capacity = 2};
    UL_CHECK(send(&tx, &link, "torn") == LINE_TX_DROPPED_PARTIAL,
             "fixture begins with a torn line");

    UL_CHECK(send(&tx, &link, "must-not-leak") == LINE_TX_DROPPED,
             "a blocked recovery drops the new response before it starts");
    UL_CHECK(link.out_len == 2 && memcmp(link.out, "to", 2) == 0,
             "none of the new payload is appended to the torn line");
    UL_CHECK(tx.needs_resync, "the recovery obligation survives another stall");

    link.capacity = 256;
    UL_CHECK(send(&tx, &link, "clean") == LINE_TX_OK,
             "a subsequent call can still repair and continue");
    UL_CHECK(link.out_len == strlen("to\nclean\n") &&
                 memcmp(link.out, "to\nclean\n", link.out_len) == 0,
             "the first bytes after recovery are a delimiter, then valid payload");
}

void test_line_tx_run(void) {
    printf("-- line_tx --\n");
    test_complete_line();
    test_temporary_backpressure();
    test_drop_before_first_byte_keeps_boundary();
    test_partial_drop_repairs_before_next_response();
    test_missing_terminator_is_also_partial();
    test_no_new_payload_before_repair();
}
