#include <string.h>

#include "hex.h"
#include "sha256.h"
#include "unity_lite.h"

static void check_vector(const char *input, const char *expected_hex) {
    uint8_t out[32];
    sha256((const uint8_t *)input, strlen(input), out);
    char hex[65];
    hex_encode(out, sizeof(out), hex, sizeof(hex));
    UL_CHECK(strcmp(hex, expected_hex) == 0, "sha256 known-answer vector mismatch");
}

void test_sha256_run(void) {
    /* FIPS 180-4 / NIST standard test vectors. */
    check_vector("", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check_vector("abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    /* 56-byte input: crosses the single-block padding boundary (needs two
     * 64-byte blocks once the 0x80 pad + 8-byte length are appended), the
     * standard NIST multi-block vector. */
    check_vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}
