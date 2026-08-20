#ifndef LNURLVAULT_PALETTE_H
#define LNURLVAULT_PALETTE_H

#include <stdint.h>

/* The device's colours, named.
 *
 * RGB565, which is what the panels store. These are the values that have been
 * on real glass since the first bring-up and they are deliberately unchanged:
 * saturated and far apart, because the job of a state colour on a 240x135
 * panel seen at arm's length is to be unmistakable, not tasteful. What was
 * missing is that they were magic hex inside one switch, so nothing else could
 * refer to them and nobody could tell PENDING from DECLINED without decoding
 * the constant.
 *
 * The ink colour is the part that was actually wrong. Every card drew black
 * text regardless of what it was drawing on, which is fine on amber and green
 * and unreadable on the dark grey idle background (#383838) -- a screen that
 * had no text on it when the colour was chosen, and now does.
 */

#define PALETTE_INK_DARK 0x0000  /* black */
#define PALETTE_INK_LIGHT 0xFFFF /* white */

#define PALETTE_IDLE 0x39C7     /* #383838 -- dark neutral grey */
#define PALETTE_BROWSE 0x781F   /* #7800F8 -- purple */
#define PALETTE_PENDING 0xFEA0  /* #F8D400 -- amber: something is being asked */
#define PALETTE_APPROVED 0x07E0 /* #00FC00 -- green */
#define PALETTE_DECLINED 0xF800 /* #F80000 -- red */
/* Mid grey: visibly not the amber of a live prompt, and visibly not the red
 * of a refusal. A prompt nobody answered is neither. */
#define PALETTE_EXPIRED 0x8410 /* #808080 */

/* The QR screen paints its own white ground so no state colour shows around
 * the code -- a QR on a coloured margin still scans but looks like a glitch. */
#define PALETTE_PAPER 0xFFFF

#endif
