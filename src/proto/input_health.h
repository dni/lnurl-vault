#ifndef LNURLVAULT_INPUT_HEALTH_H
#define LNURLVAULT_INPUT_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

/* Whether this device's buttons can be believed.
 *
 * approval.c already makes a wedged input harmless -- a button not seen
 * released since a prompt began cannot answer it -- but not visible. A vault
 * with a wedged cancel line has silently lost its cancel button: the owner
 * presses it, nothing happens, and the request times out looking like broken
 * firmware. This is how the device says so instead. Reported by get_info.
 *
 * Real case: the ESP32-S3's PIN_BUTTON_2 reads permanently pressed (checklist
 * section 7a).
 *
 * It claims one thing only. Reading a pin released proves it is not wedged
 * low. Nothing proves a button is connected -- a disconnected one reads
 * released forever -- so INPUT_OK means "not stuck", never "works".
 *
 * INPUT_OK is sticky: approving anything means holding a button for two
 * seconds, and a fault flag that trips during normal use is one nobody reads.
 *
 * Portable and clock-injected like button_fsm.c and approval.c, for the same
 * reason. Driven a tick at a time by test/native/test_input_health.c. */

#define INPUT_HEALTH_STUCK_US (5 * 1000 * 1000)

typedef enum {
    INPUT_UNKNOWN, /* pressed since boot, not yet long enough to judge */
    INPUT_OK,      /* seen released -- this pin is not wedged low */
    INPUT_STUCK,   /* pressed continuously since boot, past the threshold */
} input_state_t;

/* Same order approval.c takes them: button 1 approves, button 2 cancels. */
typedef enum {
    INPUT_CONFIRM = 0,
    INPUT_CANCEL = 1,
    INPUT_COUNT = 2,
} input_index_t;

typedef struct {
    int64_t start_us;
    bool seen_released;
} input_track_t;

typedef struct {
    input_track_t in[INPUT_COUNT];
} input_health_t;

/* now_us starts the stuck window. */
void input_health_init(input_health_t *h, int64_t now_us);

/* Once per poll tick. Takes no clock: the deadline is evaluated on READ, so a
 * caller that stops polling (ui_task busy on an approval screen) still gets an
 * answer from the clock rather than from its last tick. */
void input_health_poll(input_health_t *h, bool confirm_pressed, bool cancel_pressed);

/* One input at a time. buttons.c feeds this from buttons_raw_1/2 so every
 * read path updates it -- including the approval loop, which reads raw levels
 * and never calls buttons_poll(). Missing that path would report "ok" for a
 * wedged pin exactly while a disclosure is being asked for. */
void input_health_observe(input_health_t *h, input_index_t which, bool pressed);

input_state_t input_health_state(const input_health_t *h, input_index_t which, int64_t now_us);

/* "ok" | "stuck" | "unknown", reported verbatim by get_info. Never NULL. */
const char *input_health_name(input_state_t state);

/* One-bit summary, for a screen or a boot self-test. */
bool input_health_any_stuck(const input_health_t *h, int64_t now_us);

#endif
