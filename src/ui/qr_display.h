#ifndef LNURLVAULT_QR_DISPLAY_H
#define LNURLVAULT_QR_DISPLAY_H

#include <stdbool.h>

/* Renders `text` (a bearer note in one of note_url.h's forms) as a QR code
 * filling the display, replacing whatever display_set_state() last showed.
 * Returns false if the text is too long to fit any QR version this device
 * tries, or the vendored QR library isn't present. See qr_display.c's header
 * comment — this depends on a third-party library not included in this repo
 * (vendor it into src/ui/ yourself; see README.md's Build & flash section).
 *
 * `caption` may be NULL, and then the code is centred exactly as it always
 * was. Given one, the code moves to the top of the panel and the caption
 * takes the strip that opens up underneath.
 *
 * That strip is free. A QR is square and both panels are landscape, so the
 * code is bounded by the SHORTER axis and there is always height left over
 * once it is not being spent on centring — no version renders smaller for
 * this, and the quiet zone is inside the square, so nothing about how the
 * code scans changes either.
 *
 * The caption is best-effort, and the code always wins. A mint host long
 * enough to push the payload up a QR version or two leaves a strip too thin
 * to write in, and then nothing is written: shrinking the code a step to make
 * room for a word about it would trade the thing being handed over for the
 * thing describing it.
 *
 * It exists because a note can now be shown in three different encodings
 * (LUD-25 names two of them; see note_url.h) and a person cycling through
 * them needs to know which one is on the glass. A QR code is the one thing
 * on this device that says nothing about itself to the naked eye. */
bool qr_display_show(const char *text, const char *caption);

#endif
