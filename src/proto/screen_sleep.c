#include "screen_sleep.h"

void screen_sleep_init(screen_sleep_t *s, int64_t now_us, uint32_t timeout_ms) {
    s->last_activity_us = now_us;
    s->timeout_ms = timeout_ms;
    s->asleep = false;
}

bool screen_sleep_touch(screen_sleep_t *s, int64_t now_us) {
    s->last_activity_us = now_us;
    const bool woke = s->asleep;
    s->asleep = false;
    return woke;
}

bool screen_sleep_expired(screen_sleep_t *s, int64_t now_us) {
    if (s->asleep || s->timeout_ms == 0) {
        return false;
    }
    /* Widened before multiplying: the largest timeout this can be given is
     * over a month in microseconds, which does not fit the 32 bits it
     * arrived in. */
    const int64_t idle_us = now_us - s->last_activity_us;
    if (idle_us < (int64_t)s->timeout_ms * 1000) {
        return false;
    }
    s->asleep = true;
    return true;
}

bool screen_sleep_is_asleep(const screen_sleep_t *s) {
    return s->asleep;
}
