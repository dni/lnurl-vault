#include <string.h>

#include "base64.h"
#include "unity_lite.h"

static void check_roundtrip(const char *plain, const char *expected_b64) {
    size_t plain_len = strlen(plain);
    char encoded[128];
    UL_CHECK(base64_encoded_len(plain_len) + 1 <= sizeof(encoded), "encoded buffer big enough");
    base64_encode((const unsigned char *)plain, plain_len, encoded);
    UL_CHECK(strcmp(encoded, expected_b64) == 0, expected_b64);

    unsigned char decoded[128];
    size_t decoded_len = 0;
    bool ok = base64_decode(encoded, strlen(encoded), decoded, &decoded_len);
    UL_CHECK(ok, "decode reports success");
    UL_CHECK(decoded_len == plain_len, "decoded length matches original");
    UL_CHECK(decoded_len == plain_len && memcmp(decoded, plain, plain_len) == 0,
              "decoded bytes match original");
}

void test_base64_run(void) {
    /* RFC 4648 section 10 test vectors. */
    check_roundtrip("", "");
    check_roundtrip("f", "Zg==");
    check_roundtrip("fo", "Zm8=");
    check_roundtrip("foo", "Zm9v");
    check_roundtrip("foob", "Zm9vYg==");
    check_roundtrip("fooba", "Zm9vYmE=");
    check_roundtrip("foobar", "Zm9vYmFy");

    unsigned char decoded[16];
    size_t decoded_len = 0;
    UL_CHECK(!base64_decode("A", 1, decoded, &decoded_len), "length not a multiple of 4 is rejected");
    UL_CHECK(!base64_decode("AB=D", 4, decoded, &decoded_len), "misplaced padding is rejected");
    UL_CHECK(!base64_decode("A===", 4, decoded, &decoded_len), "over-padded group is rejected");
    UL_CHECK(!base64_decode("AB!=", 4, decoded, &decoded_len), "invalid character is rejected");

    /* A 1024-byte OTA chunk round-trips too, not just short strings. */
    unsigned char chunk[1024];
    for (size_t i = 0; i < sizeof(chunk); i++) {
        chunk[i] = (unsigned char)(i * 37 + 11);
    }
    char chunk_b64[base64_encoded_len(sizeof(chunk)) + 1];
    base64_encode(chunk, sizeof(chunk), chunk_b64);
    unsigned char chunk_out[1024];
    size_t chunk_out_len = 0;
    bool ok = base64_decode(chunk_b64, strlen(chunk_b64), chunk_out, &chunk_out_len);
    UL_CHECK(ok && chunk_out_len == sizeof(chunk) && memcmp(chunk, chunk_out, sizeof(chunk)) == 0,
             "a 1024-byte binary chunk round-trips through base64 intact");
}
