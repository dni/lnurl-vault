#ifndef LNURLVAULT_QR_DISPLAY_H
#define LNURLVAULT_QR_DISPLAY_H

#include <stdbool.h>

/* Renders `text` (the lnurlw:// URL from note_url_build(), see
 * src/proto/note_url.h) as a QR code filling the display, replacing
 * whatever display_set_state() last showed. Returns false if the text is
 * too long to fit any QR version this device tries, or the vendored QR
 * library isn't present. See qr_display.c's header comment — this depends
 * on a third-party library not included in this repo. */
bool qr_display_show(const char *text);

#endif
