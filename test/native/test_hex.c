#include <string.h>

#include "hex.h"
#include "unity_lite.h"

void test_hex_run(void) {
    uint8_t bytes[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    char hex[9];
    UL_CHECK(hex_encode(bytes, 4, hex, sizeof(hex)), "encode reports success");
    UL_CHECK(strcmp(hex, "deadbeef") == 0, "encode produces lowercase hex");

    uint8_t decoded[4];
    UL_CHECK(hex_decode(hex, 8, decoded, sizeof(decoded)), "decode reports success");
    UL_CHECK(memcmp(decoded, bytes, 4) == 0, "decode round-trips to original bytes");

    UL_CHECK(!hex_decode("xy00", 4, decoded, sizeof(decoded)), "decode rejects non-hex chars");
    UL_CHECK(!hex_decode("abc", 3, decoded, sizeof(decoded)), "decode rejects odd length");
    UL_CHECK(!hex_encode(bytes, 4, hex, 4), "encode rejects too-small buffer");
}
