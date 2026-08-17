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

static button_fsm_t g_fsm;

void buttons_init(void) {
    board_buttons_init();
    button_fsm_init(&g_fsm);
}

bool buttons_raw_1(void) {
    return board_button_1_pressed();
}

bool buttons_raw_2(void) {
    return board_button_2_pressed();
}

void buttons_consume_press(void) {
    button_fsm_consume_press(&g_fsm);
}

button_event_t buttons_poll(void) {
    return button_fsm_poll(&g_fsm, board_button_1_pressed(), board_button_2_pressed(),
                            esp_timer_get_time());
}
