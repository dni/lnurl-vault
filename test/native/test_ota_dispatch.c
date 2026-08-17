#include <stdio.h>
#include <string.h>

#include "base64.h"
#include "dispatcher.h"
#include "hex.h"
#include "json.h"
#include "monocypher-ed25519.h"
#include "ota_sign.h"
#include "sha256.h"
#include "unity_lite.h"

/* Exercises dispatcher.c's ota_begin/ota_chunk/ota_finish state machine end
 * to end through dispatcher_handle() itself (not the internals directly),
 * against a real ed25519 keypair/signature and a fake in-memory "flash"
 * standing in for the injected ota_write_* deps — the same
 * dependency-injection seam confirm_export/free_heap/reset already use, so
 * this is exercised the same way the rest of dispatcher.c is. */

#define FAKE_FLASH_MAX (8 * 1024)
static uint8_t g_fake_flash[FAKE_FLASH_MAX];
static size_t g_fake_flash_len;
static bool g_write_begin_called, g_write_finish_called, g_write_abort_called;
static bool g_fail_next_begin, g_fail_next_chunk, g_fail_next_finish;
static confirm_result_t g_approve_response;
static bool g_approve_called;

static confirm_result_t fake_approve(uint32_t size_bytes, uint32_t timeout_ms) {
    (void)size_bytes;
    (void)timeout_ms;
    g_approve_called = true;
    return g_approve_response;
}
static bool fake_write_begin(uint32_t total_size) {
    (void)total_size;
    g_write_begin_called = true;
    g_fake_flash_len = 0;
    if (g_fail_next_begin) {
        g_fail_next_begin = false;
        return false;
    }
    return true;
}
static bool fake_write_chunk(const uint8_t *data, size_t len) {
    if (g_fail_next_chunk) {
        g_fail_next_chunk = false;
        return false;
    }
    memcpy(g_fake_flash + g_fake_flash_len, data, len);
    g_fake_flash_len += len;
    return true;
}
static bool fake_write_finish(void) {
    g_write_finish_called = true;
    if (g_fail_next_finish) {
        g_fail_next_finish = false;
        return false;
    }
    return true;
}
static void fake_write_abort(void) {
    g_write_abort_called = true;
}

static void reset_fakes(void) {
    g_fake_flash_len = 0;
    g_write_begin_called = g_write_finish_called = g_write_abort_called = false;
    g_fail_next_begin = g_fail_next_chunk = g_fail_next_finish = false;
    g_approve_response = CONFIRM_YES;
    g_approve_called = false;
}

static bool rng_stub(uint8_t *out, size_t len) {
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)i;
    return true;
}

static void sign_image(const uint8_t secret_key[64], const uint8_t *image, size_t image_len,
                        uint8_t digest_out[OTA_DIGEST_LEN], uint8_t sig_out[OTA_SIGNATURE_LEN]) {
    sha256(image, image_len, digest_out);
    uint8_t message[OTA_MESSAGE_MAX_LEN];
    size_t message_len = 0;
    ota_signing_message(digest_out, message, &message_len);
    crypto_ed25519_sign(sig_out, secret_key, message, message_len);
}

static void send_ota_chunk(const uint8_t *image, uint32_t offset, size_t len, char *out, size_t outcap) {
    char b64[1400];
    base64_encode(image + offset, len, b64);
    char cmd_buf[1600];
    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"ota_chunk\",\"offset\":%u,\"data\":\"%s\"}",
             (unsigned)offset, b64);
    dispatcher_handle(cmd_buf, out, outcap);
}

void test_ota_dispatch_run(void) {
    uint8_t seed[32];
    memset(seed, 0x42, sizeof(seed));
    uint8_t secret_key[64], public_key[32];
    crypto_ed25519_key_pair(secret_key, public_key, seed);

    dispatcher_deps_t deps = {
        .rng = rng_stub,
        .ota_pubkey = public_key,
        .ota_approve = fake_approve,
        .ota_write_begin = fake_write_begin,
        .ota_write_chunk = fake_write_chunk,
        .ota_write_finish = fake_write_finish,
        .ota_write_abort = fake_write_abort,
    };
    dispatcher_init(&deps);

    uint8_t image[2500];
    for (size_t i = 0; i < sizeof(image); i++) image[i] = (uint8_t)(i * 7 + 3);
    uint8_t digest[OTA_DIGEST_LEN], signature[OTA_SIGNATURE_LEN];
    sign_image(secret_key, image, sizeof(image), digest, signature);
    char digest_hex[65], sig_hex[129];
    hex_encode(digest, sizeof(digest), digest_hex, sizeof(digest_hex));
    hex_encode(signature, sizeof(signature), sig_hex, sizeof(sig_hex));

    char out[512];
    bool ok;
    char err[32];
    char cmd_buf[256];

    /* --- ota_chunk / ota_finish with no active session — run first, while
     * g_ota (a static, so process-lifetime) is still in its untouched
     * zero-initialized state; every other block below leaves a session
     * active or explicitly finished/aborted, so this check only means
     * what it says here, at the very start. --- */
    reset_fakes();
    dispatcher_handle("{\"cmd\":\"ota_chunk\",\"offset\":0,\"data\":\"AA==\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "ota_chunk with no active session is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "invalid_state") == 0,
             "ota_chunk with no active session reports invalid_state");
    dispatcher_handle("{\"cmd\":\"ota_finish\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "ota_finish with no active session is rejected");

    /* --- happy path: begin, three chunks (1024+1024+452), finish --- */
    reset_fakes();
    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"ota_begin\",\"size\":%u,\"sha256\":\"%s\",\"signature\":\"%s\"}",
             (unsigned)sizeof(image), digest_hex, sig_hex);
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "ota_begin with a valid signature returns ok:true");
    UL_CHECK(g_write_begin_called, "a valid ota_begin opens the OTA partition");

    send_ota_chunk(image, 0, 1024, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "first ota_chunk (offset 0) returns ok:true");
    send_ota_chunk(image, 1024, 1024, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "second ota_chunk (offset 1024) returns ok:true");
    send_ota_chunk(image, 2048, sizeof(image) - 2048, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "final short ota_chunk returns ok:true");

    dispatcher_handle("{\"cmd\":\"ota_finish\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "ota_finish on a fully, correctly received image returns ok:true");
    UL_CHECK(g_write_finish_called, "ota_finish calls the injected finish hook");
    UL_CHECK(g_fake_flash_len == sizeof(image) && memcmp(g_fake_flash, image, sizeof(image)) == 0,
             "the fake flash ends up holding exactly the transferred image bytes");

    /* --- a bad signature is refused before the owner is ever asked --- */
    reset_fakes();
    uint8_t bad_sig[OTA_SIGNATURE_LEN];
    memcpy(bad_sig, signature, sizeof(signature));
    bad_sig[0] ^= 1;
    char bad_sig_hex[129];
    hex_encode(bad_sig, sizeof(bad_sig), bad_sig_hex, sizeof(bad_sig_hex));
    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"ota_begin\",\"size\":%u,\"sha256\":\"%s\",\"signature\":\"%s\"}",
             (unsigned)sizeof(image), digest_hex, bad_sig_hex);
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "ota_begin with a bad signature is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_signature") == 0,
             "a bad signature reports bad_signature");
    UL_CHECK(!g_write_begin_called, "a bad signature never opens the OTA partition or bothers the owner");

    /* --- declined physical approval --- */
    reset_fakes();
    g_approve_response = CONFIRM_NO;
    snprintf(cmd_buf, sizeof(cmd_buf), "{\"cmd\":\"ota_begin\",\"size\":%u,\"sha256\":\"%s\",\"signature\":\"%s\"}",
             (unsigned)sizeof(image), digest_hex, sig_hex);
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a declined approval is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "user_declined") == 0,
             "a declined approval reports user_declined");
    UL_CHECK(!g_write_begin_called, "a declined approval never opens the OTA partition");

    /* --- a chunk with the wrong offset doesn't kill the session --- */
    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out)); /* re-run the valid ota_begin from above */
    dispatcher_handle("{\"cmd\":\"ota_chunk\",\"offset\":999,\"data\":\"AA==\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "a chunk at the wrong offset is rejected");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_request") == 0,
             "a wrong-offset chunk reports bad_request");
    send_ota_chunk(image, 0, 1024, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok,
             "the session survives a rejected wrong-offset chunk — the correct next chunk still succeeds");

    /* --- finishing short of the declared size aborts the session --- */
    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out)); /* valid ota_begin again */
    send_ota_chunk(image, 0, 1024, out, sizeof(out));
    dispatcher_handle("{\"cmd\":\"ota_finish\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok, "finishing early (short of declared size) is rejected");
    UL_CHECK(g_write_abort_called, "finishing early aborts the session rather than leaving it dangling");

    /* --- a torn/corrupted transfer is caught at finish, not silently accepted --- */
    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out)); /* valid ota_begin again */
    uint8_t corrupted[sizeof(image)];
    memcpy(corrupted, image, sizeof(image));
    corrupted[1200] ^= 0xFF; /* flip a byte inside the second chunk */
    send_ota_chunk(corrupted, 0, 1024, out, sizeof(out));
    send_ota_chunk(corrupted, 1024, 1024, out, sizeof(out));
    send_ota_chunk(corrupted, 2048, sizeof(image) - 2048, out, sizeof(out));
    dispatcher_handle("{\"cmd\":\"ota_finish\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok,
             "a fully-sized but corrupted transfer fails the finish-time re-verify");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_signature") == 0,
             "a corrupted transfer reports bad_signature, not a generic failure");
    UL_CHECK(!g_write_finish_called, "esp_ota_end/set_boot_partition is never called for a failed re-verify");
    UL_CHECK(g_write_abort_called, "a failed re-verify aborts the session instead of leaving it half-committed");

    /* --- a fresh, correct ota_begin still works after an aborted session --- */
    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && ok, "a new ota_begin after a prior abort works normally");

    /* --- a device with no release key trusts nothing ---------------------
     * Both verify sites are guarded `!g_deps.ota_pubkey || !verify(...)`.
     * The || short-circuits, so with a NULL pubkey ota_verify_signature is
     * never reached and the outcome rests entirely on that first operand —
     * an untested branch deciding whether an unprovisioned build accepts
     * arbitrary firmware. Everything else here is genuine: real keypair,
     * real signature, real image. Only the device's trust anchor is
     * missing, which is exactly the state a build with the all-zero
     * placeholder key (or a port that forgets to wire ota_pubkey) ships
     * in. Fail-open here would mean any image at all is installable. */
    dispatcher_deps_t unprovisioned = deps;
    unprovisioned.ota_pubkey = NULL;
    dispatcher_init(&unprovisioned);

    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out)); /* the same signed, valid ota_begin as above */
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok,
             "a device with no release key refuses an image even when its signature is genuine");
    UL_CHECK(json_get_str(out, "error", err, sizeof(err)) && strcmp(err, "bad_signature") == 0,
             "a missing release key reports bad_signature, the same refusal as a bad one");
    UL_CHECK(!g_approve_called,
             "the owner is never asked to approve an update the device cannot verify");
    UL_CHECK(!g_write_begin_called,
             "no flash partition is opened for an update the device cannot verify");

    /* The finish-time re-verify carries the same guard, and it is the one
     * that actually gates set_boot_partition. Reaching it needs a session
     * opened while the key was present, so this swaps the key out mid
     * transfer — contrived as an attack, but it is the only way to prove
     * the second guard is load-bearing rather than shadowed by the first. */
    dispatcher_init(&deps);
    reset_fakes();
    dispatcher_handle(cmd_buf, out, sizeof(out));
    send_ota_chunk(image, 0, 1024, out, sizeof(out));
    send_ota_chunk(image, 1024, 1024, out, sizeof(out));
    send_ota_chunk(image, 2048, sizeof(image) - 2048, out, sizeof(out));
    dispatcher_init(&unprovisioned);
    dispatcher_handle("{\"cmd\":\"ota_finish\"}", out, sizeof(out));
    UL_CHECK(json_get_bool(out, "ok", &ok) && !ok,
             "losing the release key mid-transfer fails the finish-time re-verify too");
    UL_CHECK(!g_write_finish_called,
             "set_boot_partition is never reached without a key to verify against");
    UL_CHECK(g_write_abort_called, "that refusal aborts the session rather than leaving it dangling");

    dispatcher_init(&deps); /* leave the shared dispatcher state provisioned */
}
