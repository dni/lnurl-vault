#include "note_url.h"

#include <stdio.h>

bool note_url_build(const char *host, const char *k1_hex, uint64_t amount_msat, char *out,
                     size_t outcap) {
    if (!out || outcap == 0) {
        return false;
    }
    /* Empty on every failure path, before anything else can be written.
     *
     * This used to leave whatever snprintf had managed, and say so in the
     * header ("out's contents are unspecified"). On this device that is a
     * sharp edge: a truncated result is not obviously broken, it is a
     * plausible-looking lnurlw:// URL carrying a SHORTENED k1 -- a different
     * secret, or none. A caller that forgot to check the return would render
     * that into a QR code and hand it to somebody as money.
     *
     * Failing to an empty string cannot be mistaken for success by anything:
     * qr_display_show() refuses an empty payload, and a person sees nothing
     * rather than a code that scans to the wrong note. */
    out[0] = '\0';

    if (!host || !host[0] || !k1_hex || !k1_hex[0]) {
        return false;
    }
    int written =
        snprintf(out, outcap, "lnurlw://%s?k1=%s&amount=%llu", host, k1_hex,
                  (unsigned long long)amount_msat);
    if (written < 0 || (size_t)written >= outcap) {
        out[0] = '\0'; /* discard the truncation snprintf just wrote */
        return false;
    }
    return true;
}
