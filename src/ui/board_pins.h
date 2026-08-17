#ifndef LNURLVAULT_BOARD_PINS_H
#define LNURLVAULT_BOARD_PINS_H

/* LilyGo T-Display S3 pin assignments.
 *
 * Verified directly against LilyGo's own official pin_config.h for this
 * board (Xinyuan-LilyGO/T-Display-S3, examples/factory/pin_config.h,
 * fetched and diffed against this file, not assumed) after a real device
 * came up with backlight on but a black screen and no serial output at
 * all. That diff found one real, significant error, now fixed: this
 * board's display is NOT SPI. It's an 8-bit parallel (Intel 8080 / "i80")
 * bus — there is no MOSI/SCLK signal at all, which is exactly why display
 * bring-up was silently going nowhere (see display.c, which used to call
 * the SPI panel-IO driver against pins that were never actually connected
 * to the display). Every other pin below (CS/DC/RST/BL/POWER_ON/both
 * buttons) already matched LilyGo's config exactly.
 *
 * LilyGo has shipped more than one board revision (e.g. a later AMOLED
 * variant) with a different pinout — if you have a different revision,
 * re-diff against LilyGo's own repo, which remains the source of truth,
 * not this file:
 * https://github.com/Xinyuan-LilyGO/T-Display-S3/blob/main/examples/factory/pin_config.h
 */

#define PIN_TFT_CS 6
#define PIN_TFT_DC 7
#define PIN_TFT_RST 5
#define PIN_TFT_WR 8  /* i80 bus write strobe — ESP-IDF's esp_lcd_i80_bus_config_t calls this wr_gpio_num */
#define PIN_TFT_RD 9  /* wired on the board but unused: esp_lcd's i80 driver is write-only, no rd_gpio_num field exists */
#define PIN_TFT_D0 39
#define PIN_TFT_D1 40
#define PIN_TFT_D2 41
#define PIN_TFT_D3 42
#define PIN_TFT_D4 45
#define PIN_TFT_D5 46
#define PIN_TFT_D6 47
#define PIN_TFT_D7 48
#define PIN_TFT_BL 38
#define PIN_TFT_POWER_ON 15 /* must be driven HIGH to power the display/peripherals */

#define PIN_BUTTON_1 0  /* labeled BOOT on the board; doubles as the confirm button */
#define PIN_BUTTON_2 14 /* the cancel button */

#endif
