/* Every command, at every buffer size, must do two things: write only inside
 * the buffer it was given, and leave something a client can parse.
 *
 * dispatcher.c's finish() exists for the second one, and states why every
 * response goes through it "including the ones whose fields are fixed-size
 * and cannot overflow today, because 'cannot overflow today' is a property
 * that quietly stops being true when someone adds a field". That is a claim
 * about every handler, and exactly one test anywhere exercised the overflow
 * path -- on list_notes, the one response already known to be unbounded.
 *
 * Issue #7 is what happens when this is not checked: handlers returned
 * whatever was in the buffer, the writer tracked overflow correctly and
 * nobody asked it, and a client received a string that stopped mid-object
 * with no error field. Output stopped being parseable at 29 notes against a
 * declared capacity of 128, silently.
 *
 * The canary is the other half. A writer that overflows is one thing; a
 * writer that overflows past the end of the caller's buffer is memory
 * corruption on a device holding bearer secrets, and no amount of checking
 * the JSON afterwards would notice it. */
#include <stdio.h>
#include <string.h>

#include "dispatcher.h"
#include "json.h"
#include "unity_lite.h"
#include "vault.h"

#define CANARY_LEN 32
#define CANARY_BYTE 0xA5

static bool rng_stub(uint8_t *out, size_t len) {
    static uint32_t s = 99;
    for (size_t i = 0; i < len; i++) {
        s = s * 1103515245u + 12345u;
        out[i] = (uint8_t)(s >> 16);
    }
    return true;
}

static const char *storage_ok(void) {
    return "ok";
}

static confirm_result_t always_yes(const note_meta_t *note) {
    (void)note;
    return CONFIRM_YES;
}

/* Runs one command into a buffer of exactly `cap` bytes, with a canary
 * immediately after it, and reports what happened. */
static void probe(const char *label, const char *cmd, size_t cap, int *bad_canary,
                   int *unparseable) {
    static uint8_t arena[4096];
    if (cap + CANARY_LEN > sizeof(arena)) {
        return;
    }
    memset(arena, CANARY_BYTE, sizeof(arena));
    char *out = (char *)arena;

    dispatcher_handle(cmd, out, cap);

    for (size_t i = 0; i < CANARY_LEN; i++) {
        if ((uint8_t)arena[cap + i] != CANARY_BYTE) {
            char msg[160];
            snprintf(msg, sizeof(msg), "%s wrote past the end of a %zu-byte buffer", label, cap);
            UL_CHECK(false, msg);
            (*bad_canary)++;
            return;
        }
    }

    /* Must be NUL-terminated inside the buffer, or a caller reading it as a
     * C string runs off the end. */
    bool terminated = false;
    for (size_t i = 0; i < cap; i++) {
        if (out[i] == '\0') {
            terminated = true;
            break;
        }
    }
    if (!terminated) {
        char msg[160];
        snprintf(msg, sizeof(msg), "%s left no NUL inside a %zu-byte buffer", label, cap);
        UL_CHECK(false, msg);
        (*unparseable)++;
        return;
    }

    /* Either a complete response, or nothing at all -- never a JSON object
     * that stops partway. That middle case is what issue #7 actually was:
     * the client receives something that looks like a reply, parses as far
     * as it can, and cannot tell it was cut off. An empty buffer is not
     * pretty but it is unambiguous.
     *
     * Empty is the real behaviour below about 97 bytes, which is where the
     * longest error response stops fitting (measured: the
     * response_too_large reply with its message is 96 bytes). Both
     * transports hand this a 4096-byte buffer, so that floor is roughly
     * forty times below anything reachable -- the sizes here are probing the
     * shape of the failure, not a situation a device can be in. */
    bool ok = false;
    if (out[0] != '\0' && !json_get_bool(out, "ok", &ok)) {
        char msg[200];
        snprintf(msg, sizeof(msg), "%s at %zu bytes left a partial response: %.60s", label, cap,
                 out);
        UL_CHECK(false, msg);
        (*unparseable)++;
    }
}

void test_response_bounds_run(void) {
    vault_init(NULL, NULL);
    /* board and storage_state are wired deliberately: they push get_info's
     * reply past the 96 bytes the longest error needs, which is the only way
     * a fixed-size handler can be observed overflowing at all. Without them
     * its whole reply is 72 bytes -- smaller than the error -- so there is no
     * buffer size at which it overflows while an error still fits, and a
     * handler that skipped finish() would be indistinguishable. */
    dispatcher_deps_t deps = {.rng = rng_stub,
                              .confirm_export = always_yes,
                              .board = "t-display-s3",
                              .storage_state = storage_ok};
    dispatcher_init(&deps);

    /* A few notes, so list_notes has real work and the fixed-size handlers
     * have something to answer about. */
    char big[2048];
    char id[VAULT_ID_BUF] = {0};
    for (int i = 0; i < 4; i++) {
        char mk[128];
        snprintf(mk, sizeof(mk), "{\"cmd\":\"new_secret\",\"label\":\"note %d\"}", i);
        dispatcher_handle(mk, big, sizeof(big));
        if (i == 0) {
            json_get_str(big, "id", id, sizeof(id));
        }
        char c[196];
        char nid[VAULT_ID_BUF];
        if (json_get_str(big, "id", nid, sizeof(nid))) {
            snprintf(c, sizeof(c),
                     "{\"cmd\":\"confirm\",\"id\":\"%s\",\"amount_msat\":21000,"
                     "\"host\":\"mint.example\"}",
                     nid);
            dispatcher_handle(c, big, sizeof(big));
        }
    }

    char export_cmd[96], rename_cmd[128], spent_cmd[96], del_cmd[96], get_cmd[96];
    snprintf(export_cmd, sizeof(export_cmd), "{\"cmd\":\"export_secret\",\"id\":\"%s\"}", id);
    snprintf(rename_cmd, sizeof(rename_cmd), "{\"cmd\":\"rename\",\"id\":\"%s\",\"label\":\"x\"}", id);
    snprintf(spent_cmd, sizeof(spent_cmd), "{\"cmd\":\"mark_spent\",\"id\":\"%s\"}", id);
    snprintf(del_cmd, sizeof(del_cmd), "{\"cmd\":\"delete\",\"id\":\"%s\"}", id);
    snprintf(get_cmd, sizeof(get_cmd), "{\"cmd\":\"discard\",\"id\":\"%s\"}", id);

    const char *cases[][2] = {
        {"get_info", "{\"cmd\":\"get_info\"}"},
        {"list_notes", "{\"cmd\":\"list_notes\"}"},
        {"list_notes paged", "{\"cmd\":\"list_notes\",\"offset\":0,\"limit\":4}"},
        {"new_secret", "{\"cmd\":\"new_secret\",\"label\":\"probe\"}"},
        {"new_secret_pair", "{\"cmd\":\"new_secret_pair\"}"},
        {"export_secret", export_cmd},
        {"rename", rename_cmd},
        {"mark_spent", spent_cmd},
        {"delete", del_cmd},
        {"discard", get_cmd},
        {"import_secret",
         "{\"cmd\":\"import_secret\",\"k1\":\"" "00112233445566778899aabbccddeeff"
         "00112233445566778899aabbccddeeff" "\",\"host\":\"h\",\"amount_msat\":1}"},
        {"wipe (unsupported)", "{\"cmd\":\"wipe\",\"confirm\":\"WIPE\"}"},
        {"unknown command", "{\"cmd\":\"totally_unknown\"}"},
        {"malformed input", "not even json"},
        {"reset", "{\"cmd\":\"reset\"}"},
    };

    /* From comfortably large down to smaller than any complete response. 48
     * is below what {"ok":false,"error":"response_too_large"} needs, so it
     * probes the floor rather than the happy path. */
    const size_t caps[] = {1024, 512, 256, 160, 128, 96, 80, 64, 48};

    int bad_canary = 0, unparseable = 0;
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            probe(cases[i][0], cases[i][1], caps[c], &bad_canary, &unparseable);
        }
    }

    UL_CHECK(bad_canary == 0,
             "no command writes past the end of the buffer it was handed, at any size");
    UL_CHECK(unparseable == 0,
             "no command leaves a partially-written response -- it is complete or it is empty");

    /* A fixed-size handler that overflows must still say so. This is the
     * window the deps above create: get_info's reply no longer fits in 104
     * bytes, but response_too_large does. finish() is what turns that into a
     * named error instead of silence -- a handler that ended its object
     * directly would leave an empty buffer here, and a client would have no
     * idea why. */
    {
        static uint8_t a3[4096];
        memset(a3, CANARY_BYTE, sizeof(a3));
        dispatcher_handle("{\"cmd\":\"get_info\"}", (char *)a3, 104);
        const char *r = (const char *)a3;
        bool ok = true;
        UL_CHECK(r[0] != '\0',
                 "a fixed-size handler that overflows answers, rather than falling silent");
        UL_CHECK(json_get_bool(r, "ok", &ok) && !ok,
                 "and answers with a parseable failure");
        char code[40];
        UL_CHECK(json_get_str(r, "error", code, sizeof(code)) &&
                     strcmp(code, "response_too_large") == 0,
                 "naming response_too_large, the one thing a client can act on");
    }

    /* The floor is where it should be: at a buffer that fits the longest
     * error, every command answers properly. Pins that the empty-output
     * regime above is a genuine floor and not a handler quietly failing at a
     * size it should manage. */
    int c2 = 0, u2 = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        static uint8_t a2[4096];
        memset(a2, CANARY_BYTE, sizeof(a2));
        dispatcher_handle(cases[i][1], (char *)a2, 256);
        bool ok = false;
        char msg[160];
        snprintf(msg, sizeof(msg), "%s answers properly in a 256-byte buffer", cases[i][0]);
        UL_CHECK(json_get_bool((const char *)a2, "ok", &ok), msg);
        (void)c2; (void)u2;
    }
}
