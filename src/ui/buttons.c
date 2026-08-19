/* A thin adapter: reads the board's two buttons and feeds their raw state to
 * the portable gesture state machine in src/proto/button_fsm.c, which is
 * where debounce and tap-vs-chord actually live and is unit-tested without
 * hardware (test/native/test_button_fsm.c).
 *
 * Which GPIOs, and which polarity, is src/board/'s business -- see board.h.
 *
 * Real button behaviour is verified on the classic T-Display: the state
 * machine produced zero spurious events across 31s at rest, and that board's
 * external pull-up on GPIO35 is present. It is NOT verified on the S3, where
 * the cancel button currently reads as permanently pressed and makes
 * export_secret impossible -- an open fault, see
 * docs/HARDWARE-TEST-CHECKLIST.md section 7a.
 *
 * Which is the reason the levels here are read raw and judged in portable
 * code rather than trusted: a wrong pin, a floating input or a coupled
 * glitch all look identical from this side of the wire. */
#include "buttons.h"

#include "board.h"
#include "button_fsm.h"
#include "esp_timer.h"
#include "input_health.h"

static button_fsm_t g_fsm;

/* Updated by the raw reads below rather than by a tick of its own, so that
 * both the browsing poll and the approval loop feed it without either having
 * to remember to. See input_health.h. */
static input_health_t g_health;

void buttons_init(void) {
    board_buttons_init();
    button_fsm_init(&g_fsm);
    input_health_init(&g_health, esp_timer_get_time());
}

bool buttons_raw_1(void) {
    const bool pressed = board_button_1_pressed();
    input_health_observe(&g_health, INPUT_CONFIRM, pressed);
    return pressed;
}

bool buttons_raw_2(void) {
    const bool pressed = board_button_2_pressed();
    input_health_observe(&g_health, INPUT_CANCEL, pressed);
    return pressed;
}

input_state_t buttons_input_state(input_index_t which) {
    return input_health_state(&g_health, which, esp_timer_get_time());
}

void buttons_consume_press(void) {
    button_fsm_consume_press(&g_fsm);
}

button_event_t buttons_poll(void) {
    /* Through the raw accessors, not board_button_*_pressed() directly, so
     * this path feeds the health record too. */
    const bool b1 = buttons_raw_1();
    const bool b2 = buttons_raw_2();
    return button_fsm_poll(&g_fsm, b1, b2, esp_timer_get_time());
}
