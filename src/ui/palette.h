#ifndef LNURLVAULT_PALETTE_H
#define LNURLVAULT_PALETTE_H

#include <stdint.h>

/* RGB565, as the panels store it. Values unchanged from the first bring-up:
 * saturated and far apart, so a state is unmistakable at arm's length. */

#define PALETTE_INK_DARK 0x0000  /* black */
#define PALETTE_INK_LIGHT 0xFFFF /* white */

#define PALETTE_IDLE 0x39C7     /* #383838 dark grey */
#define PALETTE_BROWSE 0x781F   /* #7800F8 purple */
#define PALETTE_PENDING 0xFEA0  /* #F8D400 amber */
#define PALETTE_APPROVED 0x07E0 /* #00FC00 green */
#define PALETTE_DECLINED 0xF800 /* #F80000 red */
/* Neither the amber of a live prompt nor the red of a refusal: a prompt
 * nobody answered is neither. */
#define PALETTE_EXPIRED 0x8410 /* #808080 */

#define PALETTE_PAPER 0xFFFF /* the QR screen's own ground */

#endif
