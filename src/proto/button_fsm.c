#include "button_fsm.h"

#define BUTTON_FSM_DEBOUNCE_US (30 * 1000)
#define BUTTON_FSM_CHORD_HOLD_US (200 * 1000)

void button_fsm_init(button_fsm_t *fsm) {
    fsm->b1_down = false;
    fsm->b2_down = false;
    fsm->b1_down_since_us = 0;
    fsm->b2_down_since_us = 0;
    fsm->chord_fired = false;
}

button_event_t button_fsm_poll(button_fsm_t *fsm, bool b1_pressed, bool b2_pressed,
                                int64_t now_us) {
    if (b1_pressed && !fsm->b1_down) {
        fsm->b1_down = true;
        fsm->b1_down_since_us = now_us;
    }
    if (b2_pressed && !fsm->b2_down) {
        fsm->b2_down = true;
        fsm->b2_down_since_us = now_us;
    }

    button_event_t event = BTN_EVENT_NONE;

    if (b1_pressed && b2_pressed) {
        if (!fsm->chord_fired && (now_us - fsm->b1_down_since_us) >= BUTTON_FSM_CHORD_HOLD_US &&
            (now_us - fsm->b2_down_since_us) >= BUTTON_FSM_CHORD_HOLD_US) {
            fsm->chord_fired = true;
            event = BTN_EVENT_BOTH_CHORD;
        }
    } else {
        /* At most one button is currently down. A release-edge here counts
         * as a tap unless a chord already fired for this hold cycle — e.g.
         * button 1 released quickly while button 2 is still held (never
         * having overlapped long enough to register as a chord) still
         * counts as an ordinary tap of button 1, deliberately: requiring
         * "no overlap ever" tracking for that edge case isn't worth the
         * complexity for a two-button device. */
        if (!b1_pressed && fsm->b1_down) {
            bool long_enough = (now_us - fsm->b1_down_since_us) >= BUTTON_FSM_DEBOUNCE_US;
            if (long_enough && !fsm->chord_fired) {
                event = BTN_EVENT_1_TAP;
            }
            fsm->b1_down = false;
        }
        if (!b2_pressed && fsm->b2_down) {
            bool long_enough = (now_us - fsm->b2_down_since_us) >= BUTTON_FSM_DEBOUNCE_US;
            if (long_enough && !fsm->chord_fired && event == BTN_EVENT_NONE) {
                event = BTN_EVENT_2_TAP;
            }
            fsm->b2_down = false;
        }
    }

    if (!b1_pressed && !b2_pressed) {
        fsm->chord_fired = false;
    }

    return event;
}
