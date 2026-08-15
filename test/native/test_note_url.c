#include <string.h>

#include "note_url.h"
#include "unity_lite.h"

void test_note_url_run(void) {
    char out[256];

    UL_CHECK(note_url_build("mint.example/w", "aa11bb22", 21000, out, sizeof(out)),
              "builds a URL from host + secret + amount");
    UL_CHECK(strcmp(out, "lnurlw://mint.example/w?k1=aa11bb22&amount=21000") == 0,
              "URL has the exact expected shape");

    UL_CHECK(!note_url_build("", "aa11bb22", 21000, out, sizeof(out)), "rejects an empty host");
    UL_CHECK(!note_url_build("mint.example/w", "", 21000, out, sizeof(out)), "rejects an empty secret");
    UL_CHECK(!note_url_build(NULL, "aa11bb22", 21000, out, sizeof(out)), "rejects a NULL host");

    char tiny[16];
    UL_CHECK(!note_url_build("mint.example/w", "aa11bb22", 21000, tiny, sizeof(tiny)),
              "rejects a buffer too small to hold the result instead of truncating silently");
}
