#ifndef LNURLVAULT_SCREEN_SLEEP_H
#define LNURLVAULT_SCREEN_SLEEP_H

#include <stdbool.h>
#include <stdint.h>

/* When the screen goes dark, and what brings it back.
 *
 * A vault spends nearly all its life plugged in, showing the same resting
 * card -- the same glyphs in the same pixels, hour after hour. That is how an
 * IPS panel ends up with a faint permanent "N NOTES" ghosted into it, and it
 * burns the backlight for a screen nobody is looking at.
 *
 * Pure logic, no GPIO and no FreeRTOS: src/ui/ui_task.c feeds it a monotonic
 * clock and calls src/ui/display.c to actually put the light out. Kept here,
 * beside button_fsm.c and approval.c, for the same reason they are -- the
 * interesting cases are all timing, and timing bugs on a device whose screen
 * is the security control are miserable to find on glass. See
 * test/native/test_screen_sleep.c.
 *
 * The invariant worth stating out loud: this cannot blank a live prompt.
 * Not because of a flag in here, but because ui_task only asks it in the
 * main loop, and while a confirmation is on screen that loop is not
 * running -- see ui_task.c's service_remote_confirm(). A confirmation
 * nobody can see is not a confirmation. */

typedef struct {
    int64_t last_activity_us;
    uint32_t timeout_ms; /* 0 = never sleep */
    bool asleep;
} screen_sleep_t;

/* Starts awake with the clock running from `now_us`.
 *
 * `timeout_ms` of 0 disables sleeping outright, which is what a board whose
 * panel did not come up should get: there is nothing to blank, and a device
 * that thinks it is asleep swallows the first press of every gesture for
 * nothing. */
void screen_sleep_init(screen_sleep_t *s, int64_t now_us, uint32_t timeout_ms);

/* Something the owner should be able to see has just been put on screen, or
 * they have just touched a button. Restarts the clock.
 *
 * Returns true only when this call was the one that woke a sleeping screen,
 * so the caller knows to turn the light back on -- and, at the call site that
 * matters, that the press which did it has already been spent. */
bool screen_sleep_touch(screen_sleep_t *s, int64_t now_us);

/* Call once per poll tick. True on the single tick the screen should go
 * dark, and false forever after until something touches it again: an edge,
 * not a level, so the caller does not have to remember whether it has
 * already blanked. */
bool screen_sleep_expired(screen_sleep_t *s, int64_t now_us);

bool screen_sleep_is_asleep(const screen_sleep_t *s);

#endif
