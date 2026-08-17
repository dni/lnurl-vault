#ifndef LNURLVAULT_NOTE_DISPLAY_H
#define LNURLVAULT_NOTE_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

/* Turns a note's fields into the short strings the approval screen shows.
 *
 * Separated from the drawing code, and kept free of ESP-IDF, because this is
 * the part with something to get wrong: an amount rendered a factor of a
 * thousand out, or with its digits ungrouped so 21000 and 210000 look alike,
 * is a wrong number on the one screen where the owner is deciding whether to
 * hand over money. test/native/test_note_display.c covers it.
 */

/* Enough for the largest uint64 in msat, grouped, plus a unit and a NUL:
 * 18446744073709551615 is 20 digits, 6 separators, " msat" and a terminator. */
#define NOTE_AMOUNT_BUF 40

/* Writes a human-readable amount, always NUL-terminated, never exceeding cap.
 *
 * Sats when the amount is a whole number of them, msat otherwise -- rounding
 * a sub-sat remainder away would show an amount the note does not carry.
 * Digits are grouped in threes with spaces, because the failure that matters
 * here is misreading a magnitude, not misreading a digit.
 *
 *      0 ->        "0 sats"
 *   1000 ->         "1 sat"
 *  21000 ->        "21 sats"
 * 2100000 ->    "2 100 sats"
 *   1500 ->     "1500 msat"
 */
void note_format_amount(uint64_t msat, char *out, size_t cap);

/* Copies `label` into `out` with anything unprintable replaced by '?', capped
 * at cap-1 characters plus a NUL.
 *
 * A label arrives over the wire and is shown on the approval screen, so it is
 * attacker-influenced text drawn at the moment of a disclosure decision.
 * Control bytes must not be able to do anything at all -- not move a cursor,
 * not blank the rest of the line, not silently shorten what is displayed --
 * and an empty or all-unprintable label must render as something visible
 * rather than as a blank space where a name should be. */
void note_format_label(const char *label, char *out, size_t cap);

#endif
