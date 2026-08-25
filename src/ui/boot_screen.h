#ifndef LNURLVAULT_BOOT_SCREEN_H
#define LNURLVAULT_BOOT_SCREEN_H

#include <stdbool.h>

/* What the vault does with its screen for the first two seconds.
 *
 * It used to draw three lines of grey text on the same grey the device rests
 * on, and be gone before anyone looked up. That is a wasted screen twice
 * over: it is the only moment the device has a person's attention with
 * nothing to ask them for, and it is the one moment when what it is DOING is
 * worth saying -- storage, keys, transport, in that order, any of which can
 * fail and each of which used to fail silently into an identical dark
 * rectangle.
 *
 * So: a shutter opens into the same band-and-bar the cards use, the name
 * types itself in, and then the band becomes an identity strip while the
 * middle fills with what actually came up. The animation is about a second of
 * it; the rest is real work being reported as it happens, so the boot is
 * longer mostly because it is now saying something.
 *
 * Drawn entirely through src/ui/display.c, so it inherits the palette and the
 * geometry rather than carrying a second copy of either -- and so
 * test/native/preview.c can render it without a board. */

/* The shutter and the wordmark. Call once, straight after display_init().
 * Returns having left the name on screen; the first boot_screen_step() turns
 * that into the checklist. */
void boot_screen_begin(const char *version, const char *board);

/* One line of the checklist, as that subsystem finishes coming up. `label`
 * is short and shouted -- STORAGE, IDENTITY, LINK -- and `ok` decides whether
 * it reads as up or as failed. A failure is drawn, not skipped: a vault whose
 * storage did not come up still boots, still answers, and must say so before
 * anyone trusts a note to it.
 *
 * Safe to call more steps than fit; the ones past the bottom are dropped
 * rather than drawn over the bar. */
void boot_screen_step(const char *label, bool ok);

/* Holds the finished screen long enough to be read, then returns. The caller
 * starts ui_task afterwards, which draws the resting card over it. */
void boot_screen_done(void);

#endif
