#include "approval.h"

void approval_begin(approval_t *a, int64_t now_us, uint32_t timeout_ms) {
    a->deadline_us = now_us + (int64_t)timeout_ms * 1000;
    a->state = APPROVAL_PENDING;
    /* Assume both buttons may be down until seen otherwise. approval_poll()
     * clears each the first time it reads that button released, and until
     * then that button cannot answer anything -- see approval.h. */
    a->approve_stale = true;
    a->cancel_stale = true;
    a->holding = false;
    a->hold_since_us = 0;
    a->release_seen = false;
    a->release_since_us = 0;
    a->cancel_down = false;
    a->cancel_since_us = 0;
}

approval_state_t approval_poll(approval_t *a, bool approve_pressed, bool cancel_pressed,
                                int64_t now_us) {
    if (a->state != APPROVAL_PENDING) {
        return a->state;
    }

    /* A press only counts once that button has been seen released since the
     * prompt began. A level is not a decision: the owner may still be holding
     * the button that answered the LAST prompt, and a button stuck low would
     * otherwise answer every prompt the device ever shows. */
    if (!approve_pressed) {
        a->approve_stale = false;
    }
    if (!cancel_pressed) {
        a->cancel_stale = false;
    }
    approve_pressed = approve_pressed && !a->approve_stale;
    cancel_pressed = cancel_pressed && !a->cancel_stale;

    /* Cancel first, deliberately: if a cancel and a completing hold land on
     * the same tick, the ambiguity resolves toward not disclosing. */
    if (cancel_pressed) {
        if (!a->cancel_down) {
            a->cancel_down = true;
            a->cancel_since_us = now_us;
        }
        if (now_us - a->cancel_since_us >= APPROVAL_CANCEL_DEBOUNCE_US) {
            a->state = APPROVAL_DENIED;
            return a->state;
        }
    } else {
        a->cancel_down = false;
    }

    if (approve_pressed) {
        if (!a->holding) {
            a->holding = true;
            a->hold_since_us = now_us;
        }
        /* Contact is back. Whatever gap preceded it was bounce, and the hold
         * it interrupted continues from its original start -- hold_since_us
         * is deliberately not touched here. */
        a->release_seen = false;

        /* Not while the cancel button is down and still being debounced.
         * Otherwise there is a window -- as long as the debounce -- in which
         * the owner has already pressed cancel, the press is not yet trusted,
         * and a hold completing inside it discloses the secret anyway. The
         * hold keeps filling; it just cannot complete until the question of
         * whether that was a real cancel has been settled. If it turns out to
         * be bounce, cancel_down clears and the very next tick can grant.
         *
         * Found by test_cancel_wins_over_a_simultaneous_hold, which failed
         * against the first version of this file: checking cancel first is
         * necessary but, on its own, not sufficient. */
        if (!a->cancel_down && now_us - a->hold_since_us >= APPROVAL_HOLD_US) {
            a->state = APPROVAL_GRANTED;
            return a->state;
        }
    } else if (a->holding) {
        if (!a->release_seen) {
            a->release_seen = true;
            a->release_since_us = now_us;
        } else if (now_us - a->release_since_us >= APPROVAL_RELEASE_BOUNCE_US) {
            /* Genuinely let go before the hold completed. Not a denial: a
             * slipped finger is not a decision, and button 2 is how you say
             * no. Start the hold over and keep waiting. */
            a->holding = false;
            a->release_seen = false;
        }
    }

    if (now_us >= a->deadline_us) {
        /* One exception to the deadline: someone part-way through a hold when
         * it lapses gets to finish. Snatching approval away at 1.9 seconds of
         * a 2 second hold would be indistinguishable, from the owner's side,
         * from the device ignoring them. The grace is bounded by the hold
         * length itself, so a prompt cannot linger on a held-down button. */
        bool finishing = a->holding && (now_us - a->hold_since_us) < APPROVAL_HOLD_US &&
                          (now_us - a->deadline_us) < APPROVAL_HOLD_US;
        if (!finishing) {
            a->state = APPROVAL_EXPIRED;
        }
    }

    return a->state;
}

uint16_t approval_progress_permille(const approval_t *a, int64_t now_us) {
    if (a->state == APPROVAL_GRANTED) {
        return 1000;
    }
    if (a->state != APPROVAL_PENDING || !a->holding) {
        return 0;
    }
    int64_t held = now_us - a->hold_since_us;
    if (held <= 0) {
        return 0;
    }
    if (held >= APPROVAL_HOLD_US) {
        return 1000;
    }
    return (uint16_t)((held * 1000) / APPROVAL_HOLD_US);
}
