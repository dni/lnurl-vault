#ifndef LNURLVAULT_BUTTON_FSM_H
#define LNURLVAULT_BUTTON_FSM_H

#include <stdbool.h>
#include <stdint.h>

/* Pure state machine turning raw two-button levels into discrete gesture
 * events: a tap (press-then-release) of either button alone, or a "chord"
 * once both have been held down together for CHORD_HOLD_US. No GPIO or
 * FreeRTOS access — src/ui/buttons.c is a thin wrapper feeding this real
 * levels and a real clock, which is what makes this logic (the one place a
 * debounce/edge-case bug would be easy to introduce) unit-testable without
 * hardware; see test/native/test_button_fsm.c. */

typedef enum {
    BTN_EVENT_NONE,
    BTN_EVENT_1_TAP,
    BTN_EVENT_2_TAP,
    BTN_EVENT_BOTH_CHORD,
} button_event_t;

typedef struct {
    bool b1_down;
    bool b2_down;
    int64_t b1_down_since_us;
    int64_t b2_down_since_us;
    bool chord_fired;
} button_fsm_t;

void button_fsm_init(button_fsm_t *fsm);

/* Call once per poll tick with each button's current raw pressed state and
 * a monotonic microsecond clock (doesn't need to start at 0 — only deltas
 * matter). Reports at most one event per call:
 *   - BTN_EVENT_1_TAP / BTN_EVENT_2_TAP: that button was held for at least
 *     BUTTON_FSM_DEBOUNCE_US then released, alone (not as part of a chord
 *     that already fired).
 *   - BTN_EVENT_BOTH_CHORD: both buttons have been held down together for
 *     at least BUTTON_FSM_CHORD_HOLD_US (tracked independently per button,
 *     so a staggered press-then-join still works). Fires exactly once per
 *     hold; releasing afterward does not also emit trailing taps. Chord
 *     detection re-arms once both buttons are released.
 * A button released before BUTTON_FSM_DEBOUNCE_US elapses is treated as a
 * bounce/glitch and produces no event. */
button_event_t button_fsm_poll(button_fsm_t *fsm, bool b1_pressed, bool b2_pressed,
                                int64_t now_us);

#endif
