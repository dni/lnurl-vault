#include "note_display.h"

#include <string.h>

/* Writes `value` into out[0..cap) with spaces every three digits, returning
 * the number of characters written (excluding the NUL). Builds backwards into
 * a scratch buffer because grouping from the right is where the group
 * boundaries are. */
static size_t grouped(uint64_t value, char *out, size_t cap) {
    char rev[32];
    size_t n = 0;
    size_t digits = 0;

    if (value == 0) {
        rev[n++] = '0';
        digits = 1;
    }
    while (value > 0 && n < sizeof(rev)) {
        if (digits > 0 && digits % 3 == 0) {
            rev[n++] = ' ';
        }
        rev[n++] = (char)('0' + (value % 10));
        value /= 10;
        digits++;
    }

    size_t written = 0;
    for (size_t i = 0; i < n && written + 1 < cap; i++) {
        out[written++] = rev[n - 1 - i];
    }
    if (cap > 0) {
        out[written] = '\0';
    }
    return written;
}

void note_format_amount_parts(uint64_t msat, char *num, size_t numcap, char *unit,
                               size_t unitcap) {
    if (num && numcap > 0) {
        num[0] = '\0';
    }
    if (unit && unitcap > 0) {
        unit[0] = '\0';
    }

    const char *u;
    uint64_t value;
    if (msat % 1000 != 0) {
        value = msat;
        u = "msat";
    } else {
        value = msat / 1000;
        u = (value == 1) ? "sat" : "sats";
    }

    if (num && numcap > 0) {
        grouped(value, num, numcap);
    }
    if (unit && unitcap > 0) {
        size_t n = strlen(u);
        if (n + 1 > unitcap) {
            n = unitcap - 1;
        }
        memcpy(unit, u, n);
        unit[n] = '\0';
    }
}

void note_format_amount(uint64_t msat, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';

    /* Sub-sat remainders are shown as msat rather than rounded: rounding would
     * put a number on the screen that the note does not actually carry, on the
     * screen where that matters most. */
    if (msat % 1000 != 0) {
        size_t n = grouped(msat, out, cap);
        const char *unit = " msat";
        size_t ulen = strlen(unit);
        if (n + ulen + 1 <= cap) {
            memcpy(out + n, unit, ulen + 1);
        }
        return;
    }

    uint64_t sats = msat / 1000;
    size_t n = grouped(sats, out, cap);
    /* "1 sat", not "1 sats". The screen is read by a person. */
    const char *unit = (sats == 1) ? " sat" : " sats";
    size_t ulen = strlen(unit);
    if (n + ulen + 1 <= cap) {
        memcpy(out + n, unit, ulen + 1);
    }
}

void note_format_label(const char *label, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    if (!label || label[0] == '\0') {
        /* Something visible, rather than a blank where a name should be: an
         * empty line reads as "the screen did not load", not as "this note has
         * no label". */
        const char *none = "(no label)";
        size_t n = strlen(none);
        if (n + 1 > cap) {
            n = cap - 1;
        }
        memcpy(out, none, n);
        out[n] = '\0';
        return;
    }

    size_t w = 0;
    for (size_t i = 0; label[i] != '\0' && w + 1 < cap; i++) {
        unsigned char c = (unsigned char)label[i];
        /* Printable ASCII only. Everything else -- control bytes, high bytes,
         * a stray newline -- becomes a visible '?' rather than being dropped:
         * dropping bytes would let two different labels display identically,
         * and this is the screen a disclosure is approved from. */
        out[w++] = (c >= 32 && c <= 126) ? (char)c : '?';
    }
    out[w] = '\0';
}
