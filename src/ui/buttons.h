#ifndef LNURLVAULT_BUTTONS_H
#define LNURLVAULT_BUTTONS_H

#include "button_fsm.h" /* button_event_t */

void buttons_init(void);

/* Thin wrapper around button_fsm_poll() (src/proto/button_fsm.h, portable
 * and unit-tested — see test/native/test_button_fsm.c) feeding it real GPIO
 * levels and a real monotonic clock. Call periodically from exactly one
 * owner task — that's ui_task.c, which is the sole owner for both local
 * note browsing (tap = next/prev, both-buttons chord = unveil a note's QR)
 * and servicing remote export_secret confirm requests, so the two never
 * read the buttons concurrently. */
button_event_t buttons_poll(void);

#endif
