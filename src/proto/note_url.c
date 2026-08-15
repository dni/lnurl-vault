#include "note_url.h"

#include <stdio.h>

bool note_url_build(const char *host, const char *k1_hex, uint64_t amount_msat, char *out,
                     size_t outcap) {
    if (!host || !host[0] || !k1_hex || !k1_hex[0]) {
        return false;
    }
    int written =
        snprintf(out, outcap, "lnurlw://%s?k1=%s&amount=%llu", host, k1_hex,
                  (unsigned long long)amount_msat);
    if (written < 0 || (size_t)written >= outcap) {
        return false;
    }
    return true;
}
