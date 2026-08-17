/* Newline-delimited command framing, for both serial transports.
 *
 * This carries every command on USB-CDC, and on the classic T-Display it is
 * the ONLY transport there is -- so it is the sole path between a host and a
 * device holding money, and it had no tests at all. The BLE side got them when
 * its reassembly was extracted (test_ble_frame.c) and four bugs fell out; this
 * is the same treatment for the serial side.
 *
 * The behaviour that matters most is the same one: an over-long line is
 * DROPPED, never truncated. Half a command that still parses as JSON is worse
 * than no command, because it could name a different note than the sender
 * meant. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "line_proto.h"
#include "unity_lite.h"

#define MAX_CAPTURED 8

typedef struct {
    int count;
    char line[MAX_CAPTURED][128];
    size_t total_chars; /* so long-line cases can assert without a big copy */
} capture_t;

static void on_line(const char *line, void *ctx) {
    capture_t *c = (capture_t *)ctx;
    c->total_chars += strlen(line);
    if (c->count < MAX_CAPTURED) {
        snprintf(c->line[c->count], sizeof(c->line[0]), "%s", line);
    }
    c->count++;
}

static void feed(line_proto_t *lp, const char *s, capture_t *c) {
    line_proto_feed(lp, (const uint8_t *)s, strlen(s), on_line, c);
}

static void test_one_command(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "{\"cmd\":\"get_info\"}\n", &c);
    UL_CHECK(c.count == 1, "one line in, one command out");
    UL_CHECK(strcmp(c.line[0], "{\"cmd\":\"get_info\"}") == 0, "delivered without its terminator");
}

/* A host may write a command in as many pieces as it likes; nothing in the
 * framing promises a whole line arrives at once. */
static void test_a_command_split_across_reads(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "{\"cmd\":", &c);
    UL_CHECK(c.count == 0, "an incomplete line delivers nothing yet");
    feed(&lp, "\"get_info\"}", &c);
    UL_CHECK(c.count == 0, "still nothing without a terminator");
    feed(&lp, "\n", &c);
    UL_CHECK(c.count == 1 && strcmp(c.line[0], "{\"cmd\":\"get_info\"}") == 0,
             "and arrives whole once terminated");
}

static void test_one_byte_at_a_time(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    const char *msg = "{\"cmd\":\"list_notes\"}\n";
    for (const char *p = msg; *p; p++) {
        line_proto_feed(&lp, (const uint8_t *)p, 1, on_line, &c);
    }
    UL_CHECK(c.count == 1 && strcmp(c.line[0], "{\"cmd\":\"list_notes\"}") == 0,
             "a command fed one byte at a time still arrives");
}

static void test_several_commands_in_one_read(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "first\nsecond\nthird\n", &c);
    UL_CHECK(c.count == 3, "three commands in one read");
    UL_CHECK(strcmp(c.line[0], "first") == 0 && strcmp(c.line[1], "second") == 0 &&
                 strcmp(c.line[2], "third") == 0,
             "in order and intact");
}

/* A host on Windows, or a terminal, sends CRLF. That must be ONE command, not
 * one command followed by an empty one. */
static void test_crlf_is_one_command(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "{\"cmd\":\"get_info\"}\r\n", &c);
    UL_CHECK(c.count == 1, "CRLF terminates exactly one command");
    UL_CHECK(strcmp(c.line[0], "{\"cmd\":\"get_info\"}") == 0, "with no stray carriage return");
}

static void test_lone_cr_terminates(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "one\rtwo\r", &c);
    UL_CHECK(c.count == 2, "a bare CR terminates too");
    UL_CHECK(strcmp(c.line[0], "one") == 0 && strcmp(c.line[1], "two") == 0, "both intact");
}

/* Blank lines are noise -- a terminal echoing, a host padding -- and must not
 * reach the dispatcher as empty commands to be answered with bad_request. */
static void test_blank_lines_are_ignored(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "\n\n\r\n\nreal\n\n\n", &c);
    UL_CHECK(c.count == 1, "only the one real command is delivered");
    UL_CHECK(strcmp(c.line[0], "real") == 0, "intact");
}

/* ---- the property that matters ---------------------------------------- */

/* An over-long line is dropped whole. Truncating it would hand the dispatcher
 * a prefix, and a prefix of a command that still parses could name a different
 * note than the sender meant. */
static void test_an_overlong_line_is_dropped_not_truncated(void) {
    static line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    static char huge[LINE_PROTO_BUF_SIZE * 2];
    memset(huge, 'A', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    feed(&lp, huge, &c);
    feed(&lp, "\n", &c);

    UL_CHECK(c.count == 0, "nothing is delivered for an over-long line");
    UL_CHECK(c.total_chars == 0, "not even a truncated prefix of it");
}

/* And the stream recovers: the next line must be understood normally. A
 * transport that wedged on one bad line would need a power cycle. */
static void test_the_stream_resyncs_after_an_overlong_line(void) {
    static line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    static char huge[LINE_PROTO_BUF_SIZE + 64];
    memset(huge, 'B', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    feed(&lp, huge, &c);
    feed(&lp, "\n{\"cmd\":\"get_info\"}\n", &c);

    UL_CHECK(c.count == 1, "the command after an over-long line is delivered");
    UL_CHECK(strcmp(c.line[0], "{\"cmd\":\"get_info\"}") == 0, "and is intact");
}

/* The over-long line's own bytes must not leak into the next command, which is
 * what a naive "reset the length" recovery would do. */
static void test_an_overlong_line_does_not_bleed_into_the_next(void) {
    static line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    static char huge[LINE_PROTO_BUF_SIZE + 10];
    memset(huge, 'C', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    feed(&lp, huge, &c);
    feed(&lp, "\nclean\n", &c);

    UL_CHECK(c.count == 1, "one command after the drop");
    UL_CHECK(strcmp(c.line[0], "clean") == 0, "carrying none of the dropped line's bytes");
    UL_CHECK(strchr(c.line[0], 'C') == NULL, "no leakage at all");
}

/* The exact boundary, both sides of it. */
static void test_the_longest_accepted_line(void) {
    static line_proto_t lp;
    static char line[LINE_PROTO_BUF_SIZE + 8];

    /* BUF_SIZE - 1 characters plus a NUL is exactly the buffer. */
    line_proto_init(&lp);
    capture_t c = {0};
    memset(line, 'x', LINE_PROTO_BUF_SIZE - 1);
    line[LINE_PROTO_BUF_SIZE - 1] = '\0';
    feed(&lp, line, &c);
    feed(&lp, "\n", &c);
    UL_CHECK(c.count == 1, "a line of exactly BUF_SIZE-1 characters is accepted");
    UL_CHECK(c.total_chars == LINE_PROTO_BUF_SIZE - 1, "at its full length");

    /* One more and it must be refused outright. */
    line_proto_init(&lp);
    capture_t c2 = {0};
    memset(line, 'y', LINE_PROTO_BUF_SIZE);
    line[LINE_PROTO_BUF_SIZE] = '\0';
    feed(&lp, line, &c2);
    feed(&lp, "\n", &c2);
    UL_CHECK(c2.count == 0, "one character more is dropped, not truncated to fit");
}

/* Several over-long lines in a row must each be dropped independently, and the
 * stream still recover. */
static void test_repeated_overlong_lines(void) {
    static line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    static char huge[LINE_PROTO_BUF_SIZE + 32];
    memset(huge, 'D', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = '\0';
    for (int i = 0; i < 3; i++) {
        feed(&lp, huge, &c);
        feed(&lp, "\n", &c);
    }
    UL_CHECK(c.count == 0, "three over-long lines deliver nothing");

    feed(&lp, "still here\n", &c);
    UL_CHECK(c.count == 1 && strcmp(c.line[0], "still here") == 0,
             "and the transport is still usable afterwards");
}

/* init must clear a reused struct, including a part-received line -- both
 * transports keep one of these as file-scope state for the life of the boot. */
static void test_init_clears_a_partial_line(void) {
    line_proto_t lp;
    line_proto_init(&lp);
    capture_t c = {0};

    feed(&lp, "half a comm", &c);
    line_proto_init(&lp);
    feed(&lp, "and\n", &c);

    UL_CHECK(c.count == 1, "one command after re-init");
    UL_CHECK(strcmp(c.line[0], "and") == 0, "with nothing left over from before");
}

void test_line_proto_run(void) {
    printf("-- line_proto --\n");
    test_one_command();
    test_a_command_split_across_reads();
    test_one_byte_at_a_time();
    test_several_commands_in_one_read();
    test_crlf_is_one_command();
    test_lone_cr_terminates();
    test_blank_lines_are_ignored();
    test_an_overlong_line_is_dropped_not_truncated();
    test_the_stream_resyncs_after_an_overlong_line();
    test_an_overlong_line_does_not_bleed_into_the_next();
    test_the_longest_accepted_line();
    test_repeated_overlong_lines();
    test_init_clears_a_partial_line();
}
