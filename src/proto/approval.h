#ifndef LNURLVAULT_APPROVAL_H
#define LNURLVAULT_APPROVAL_H

#include <stdbool.h>
#include <stdint.h>

/* The gesture that stands between a remote request and a bearer secret.
 *
 * It used to be a single tap of button 1. A tap is a low bar for disclosing
 * something that spends like cash: it is one accidental brush of a pocket
 * button away, it gives no feedback that anything registered, and it asks
 * for no more deliberation than dismissing a notification. This replaces it
 * with a two-second hold, which cannot happen by accident and which the
 * screen can show filling up so the owner knows the device heard them.
 *
 * Pure logic, no GPIO and no FreeRTOS, for the same reason button_fsm.c is:
 * this is where a debounce or edge-case bug would be easy to introduce and
 * almost impossible to find on hardware. Driven a tick at a time by
 * test/native/test_approval.c.
 *
 * The rules, each of which is a specific way this can go wrong:
 *
 *  - Only button 1 ever approves. Button 2 is cancel and nothing else. A
 *    device where either button might approve has no cancel.
 *  - Cancel is checked before approval, so a cancel arriving on the same
 *    tick as a completing hold still cancels. Ambiguity resolves toward not
 *    disclosing.
 *  - Contact bounce mid-hold must not read as the owner letting go.
 *    Mechanical buttons chatter; a hold that silently restarted every time
 *    the contact skipped would feel broken and, worse, would teach the owner
 *    to mash the button.
 *  - Letting go early does NOT deny. It abandons the hold and waits. Denying
 *    on release would make a slipped finger indistinguishable from a
 *    decision, and cancelling is what button 2 is for.
 *  - A timeout is its own outcome, distinct from a denial, so the screen can
 *    say the prompt went stale rather than leaving something that looks live
 *    but is not.
 *  - A button ALREADY HELD when the prompt appears answers nothing. Both
 *    buttons must be seen released before a press counts. Without that, a
 *    level is mistaken for a decision, and there are two ways that bites:
 *    an owner still holding button 1 from approving one request silently
 *    approves the next one that arrives, having decided nothing about it;
 *    and a cancel button stuck low -- a wrong pin, a shorted pad, a board
 *    revision that moved it -- turns every prompt the device will ever show
 *    into an instant refusal. The second is not hypothetical: on an
 *    ESP32-S3 it made export_secret impossible, returning user_declined in
 *    0.9s with nobody touching the device.
 *
 * Adapted from forgesworn/heartwood-esp32's approval.rs, which found each of
 * these on real hardware. */

/* Hold this long, on button 1, to approve. */
#define APPROVAL_HOLD_US (2 * 1000 * 1000)

/* A gap in contact shorter than this, part-way through a hold, is bounce
 * and is ignored. Longer, and the owner has genuinely let go. */
#define APPROVAL_RELEASE_BOUNCE_US (40 * 1000)

/* Button 2 must be down at least this long to count as a cancel.
 *
 * 30ms was enough for contact bounce and was not enough for the real world.
 * Measured on a classic T-Display: a confirm over serial, then a confirm over
 * BLE, and the second was refused in about a second with nobody touching the
 * device. On a fresh boot with only the BLE prompt it timed out correctly at
 * 31s, so it is not a stuck line -- something makes that input read pressed
 * briefly, and it correlates with the radio being busy.
 *
 * That is exactly the failure board_t_display.c warns about for this pin:
 * button 2 there is GPIO35, which on the classic ESP32 is input-only and has
 * NO internal pull resistor at all, so it depends entirely on the board's
 * external one. A weak or absent pull-up leaves a high-impedance input next to
 * a transmitting radio.
 *
 * 250ms is far below what a person pressing a button produces -- nobody taps a
 * cancel that fast on purpose -- and far above bounce or a coupled glitch. It
 * costs the owner nothing and removes a class of spurious refusal that, on a
 * device whose whole job is to ask permission, reads as the device saying no
 * on its own. It does NOT make a genuinely stuck line safe; that is what the
 * fresh-press rule above is for. */
#define APPROVAL_CANCEL_DEBOUNCE_US (250 * 1000)

typedef enum {
    APPROVAL_PENDING, /* still waiting on the owner */
    APPROVAL_GRANTED,
    APPROVAL_DENIED,  /* button 2: an actual decision */
    APPROVAL_EXPIRED, /* nobody answered */
} approval_state_t;

typedef struct {
    int64_t deadline_us;
    approval_state_t state;

    /* Set while a button was already down when the prompt began. Cleared
     * only once that button has been seen released, after which a press is
     * a fresh one and counts. */
    bool approve_stale;
    bool cancel_stale;

    bool holding;
    int64_t hold_since_us;
    bool release_seen; /* contact lost mid-hold, not yet judged real */
    int64_t release_since_us;

    bool cancel_down;
    int64_t cancel_since_us;
} approval_t;

void approval_begin(approval_t *a, int64_t now_us, uint32_t timeout_ms);

/* Call once per poll tick with each button's raw pressed state and a
 * monotonic microsecond clock. Returns the current state; once it is
 * anything but APPROVAL_PENDING it stays there, so the caller can stop
 * polling whenever it notices. */
approval_state_t approval_poll(approval_t *a, bool approve_pressed, bool cancel_pressed,
                                int64_t now_us);

/* How far through the hold, in parts per thousand, for drawing a progress
 * bar. 0 when not holding, 1000 once granted. A bounce being filtered does
 * not drop this back to 0 -- the bar must not stutter at every skipped
 * contact, or it reads as the device losing the press. */
uint16_t approval_progress_permille(const approval_t *a, int64_t now_us);

#endif
