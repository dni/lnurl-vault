#ifndef LNURLVAULT_BOARD_PINS_H
#define LNURLVAULT_BOARD_PINS_H

/* LilyGo T-Display S3 pin assignments.
 *
 * These numbers compile fine (confirmed — see README.md's "Status"
 * section for the full build this is part of) but that only proves they're
 * valid GPIO numbers, not that they're electrically correct for your
 * board: NOT independently verified against real hardware (no board
 * attached in the environment that built this project). These match
 * LilyGo's commonly published T-Display-S3 pins_config.h at the time of
 * writing, but LilyGo has shipped more than one board revision (e.g. a
 * later AMOLED variant) with a different pinout. Before flashing, diff
 * these against your specific board revision using LilyGo's own example
 * repo — that repo, not this file, is the source of truth:
 * https://github.com/Xinyuan-LilyGO/T-Display-S3 (pins_config.h)
 */

#define PIN_TFT_MOSI 13
#define PIN_TFT_SCLK 12
#define PIN_TFT_CS 6
#define PIN_TFT_DC 7
#define PIN_TFT_RST 5
#define PIN_TFT_BL 38
#define PIN_TFT_POWER_ON 15 /* must be driven HIGH to power the display/peripherals */

#define PIN_BUTTON_1 0  /* labeled BOOT on the board; doubles as the confirm button */
#define PIN_BUTTON_2 14 /* the cancel button */

#endif
