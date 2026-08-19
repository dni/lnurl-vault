#include "input_health.h"

void input_health_init(input_health_t *h, int64_t now_us) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        h->in[i].start_us = now_us;
        h->in[i].seen_released = false;
    }
}

static void track(input_track_t *t, bool pressed) {
    if (!pressed) {
        /* The only proof available, and sticky on purpose -- see the header. */
        t->seen_released = true;
    }
}

void input_health_observe(input_health_t *h, input_index_t which, bool pressed) {
    if ((unsigned)which >= (unsigned)INPUT_COUNT) {
        return;
    }
    track(&h->in[which], pressed);
}

void input_health_poll(input_health_t *h, bool confirm_pressed, bool cancel_pressed) {
    input_health_observe(h, INPUT_CONFIRM, confirm_pressed);
    input_health_observe(h, INPUT_CANCEL, cancel_pressed);
}

input_state_t input_health_state(const input_health_t *h, input_index_t which, int64_t now_us) {
    /* Unsigned: a `which < 0` branch is always-false under -Wextra -Werror. */
    if ((unsigned)which >= (unsigned)INPUT_COUNT) {
        return INPUT_UNKNOWN;
    }
    const input_track_t *t = &h->in[which];
    if (t->seen_released) {
        return INPUT_OK;
    }
    /* Judged on read, not latched in poll() -- see the header. */
    if (now_us - t->start_us >= INPUT_HEALTH_STUCK_US) {
        return INPUT_STUCK;
    }
    return INPUT_UNKNOWN;
}

const char *input_health_name(input_state_t state) {
    switch (state) {
        case INPUT_OK:
            return "ok";
        case INPUT_STUCK:
            return "stuck";
        case INPUT_UNKNOWN:
        default:
            return "unknown";
    }
}

bool input_health_any_stuck(const input_health_t *h, int64_t now_us) {
    for (int i = 0; i < INPUT_COUNT; i++) {
        if (input_health_state(h, (input_index_t)i, now_us) == INPUT_STUCK) {
            return true;
        }
    }
    return false;
}
