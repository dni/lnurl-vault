#ifndef LNURLVAULT_BUTTONS_H
#define LNURLVAULT_BUTTONS_H

#include <stdbool.h>

#include "button_fsm.h" /* button_event_t */
#include "input_health.h" /* input_state_t, input_index_t */

void buttons_init(void);

/* Thin wrapper around button_fsm_poll() (src/proto/button_fsm.h, portable
 * and unit-tested — see test/native/test_button_fsm.c) feeding it real GPIO
 * levels and a real monotonic clock. Call periodically from exactly one
 * owner task — that's ui_task.c, which is the sole owner for both local
 * note browsing (tap = next/prev, both-buttons chord = unveil a note's QR)
 * and servicing remote export_secret confirm requests, so the two never
 * read the buttons concurrently. */
button_event_t buttons_poll(void);

/* Raw, undebounced levels, for the approval gesture.
 *
 * The approval screen measures a two-second hold itself (see approval.h)
 * rather than consuming the tap events above, for the plain reason that a
 * hold is not a tap: button_fsm reports a press only once it is over, and by
 * then there is nothing left to show a progress bar for. approval.c does its
 * own debouncing, including the part button_fsm does not do -- treating a
 * contact skip part-way through a hold as bounce rather than as a release. */
bool buttons_raw_1(void);
bool buttons_raw_2(void);

/* Declares whatever is held right now to have already been acted on, so its
 * release produces no event -- see button_fsm_consume_press(). The approval
 * screen calls this once it has an answer, because the button that gave that
 * answer is still physically down. */
void buttons_consume_press(void);

/* Whether each button can be believed, for get_info's `inputs` object.
 *
 * Fed automatically by buttons_raw_1()/buttons_raw_2(), which every read path
 * goes through -- both the browsing poll and the approval loop -- so there is
 * no separate tick to forget. See src/proto/input_health.h for what the states
 * mean and, more importantly, what they deliberately do not claim. */
input_state_t buttons_input_state(input_index_t which);

#endif
