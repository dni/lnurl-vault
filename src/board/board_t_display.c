/* LilyGo T-Display (the ORIGINAL, classic ESP32-D0WD -- not the S3):
 * ST7789 240x135 colour TFT over SPI, two buttons, CH9102 USB-UART bridge.
 *
 * This board cannot exercise the S3 build's i80 parallel display bus (the
 * classic ESP32 has no LCD_CAM peripheral) or its native USB-CDC transport
 * (no USB-OTG). It exists in this tree because it can exercise the things
 * that are hardest to get right without hardware and most costly to get
 * wrong: the two-button gesture state machine against real contact bounce,
 * a QR code a real phone has to scan, NVS across real power cycles, and BLE
 * reassembly against a real central.
 *
 * The pin map and panel parameters below are not guesses. They come from a
 * sibling ESP32 project that runs on this exact board:
 *   https://github.com/forgesworn/heartwood-esp32/blob/main/firmware/src/board.rs
 *
 * Note the panel differences from the S3 board, which are easy to conflate:
 * this glass is 135x240 native (offsets 52,40 into the controller's 240x320)
 * rather than 170x320 (offset 35), and its backlight is a plain GPIO rather
 * than sharing a peripheral power rail.
 */
#ifdef LNURLVAULT_BOARD_T_DISPLAY

#include "board.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "serial_uart.h"

static const char *TAG = "board";

const char *const BOARD_NAME = "t-display";

/* --- pins --------------------------------------------------------------- */

#define PIN_TFT_SCLK 18
#define PIN_TFT_MOSI 19
#define PIN_TFT_CS 5
#define PIN_TFT_DC 16
#define PIN_TFT_RST 23
#define PIN_TFT_BL 4 /* backlight, active high, plain GPIO */

#define PIN_BUTTON_1 0  /* labelled BOOT, internal pull-up; the confirm button */
#define PIN_BUTTON_2 35 /* input-only on the classic ESP32: no internal pull, */
                        /* relies on the board's external pull-up            */

/* --- panel geometry ----------------------------------------------------- */

/* Native portrait is 135x240, sitting at offset (52,40) inside the ST7789's
 * 240x320 of RAM. Driven rotated into a 240x135 landscape surface, which is
 * why the gap below is expressed on the swapped axes. */
#define PANEL_W 240
#define PANEL_H 135
#define PANEL_GAP_X 40
#define PANEL_GAP_Y 52

/* The panel is write-only and the bus is a dedicated one, so this can go
 * considerably faster. Kept conservative for first bring-up: a wrong-looking
 * screen is much easier to diagnose when clock timing is not also a suspect. */
#define LCD_PCLK_HZ (20 * 1000 * 1000)
#define LCD_SPI_HOST SPI2_HOST

board_display_t board_display_init(void) {
    board_display_t out = {.panel = NULL, .width = PANEL_W, .height = PANEL_H};

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TFT_SCLK,
        .mosi_io_num = PIN_TFT_MOSI,
        .miso_io_num = -1, /* write-only panel */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = PANEL_W * PANEL_H * (int)sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return out;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_TFT_CS,
        .dc_gpio_num = PIN_TFT_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel io init failed: %s", esp_err_to_name(err));
        return out;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "st7789 panel init failed: %s", esp_err_to_name(err));
        return out;
    }

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true); /* this panel ships inverted */

    /* Both axes mirrored, established empirically on the board rather than
     * reasoned from the datasheet -- the self-test's red corner was walked
     * from bottom-left to top-left one boolean at a time:
     *
     *   mirror(false, true)  -> red bottom-LEFT
     *   mirror(false, false) -> red bottom-RIGHT
     *   mirror(true,  true)  -> red top-left
     *
     * Note what the middle line shows: toggling mirror_Y moved the image
     * HORIZONTALLY. esp_lcd applies mirroring in panel coordinates, before
     * swap_xy exchanges the axes, so with swap_xy on the two mirror flags are
     * transposed relative to the surface we draw into. Worth knowing before
     * "fixing" this by inspection.
     *
     * THAT WALK PICKED THE WRONG SETTING, and could not have picked the right
     * one. It tracked a single red square, and a corner marker cannot tell a
     * rotation from a reflection: mirror(true, true) and mirror(true, false)
     * both put red in the top-left, and only one of them is not mirrored.
     * swap_xy is itself a transpose -- a reflection -- so the total transform
     * has to be checked with something asymmetric, not with a corner.
     *
     * Nothing drawn since could reveal it. A flat colour has no handedness. A
     * QR code has none either, as far as a phone is concerned: decoders
     * correct orientation themselves, so a mirrored code still scans. On-screen
     * TEXT was the first content with a handedness, and it came out reversed
     * the moment it appeared.
     *
     * Re-established with a letter F -- asymmetric under every rotation and
     * every reflection, so exactly one of the four flips can look right -- drawn
     * four times, each tagged with a COUNT of squares rather than a label or a
     * position, since which way round the screen is was the open question.
     * Reported from the bench: the whole layout sat on the wrong side, and the
     * copy that read correctly was the one pre-flipped horizontally. That is a
     * horizontal mirror, which under swap_xy is mirror_Y.
     *
     * The gaps do not move with it: on this glass the 240-pixel axis sits at
     * offset 40 in a 320-wide controller window, and 320 - 240 - 40 is also 40,
     * so flipping that axis lands on the same offset.
     *
     * These values are for THIS panel. The S3 board uses a different
     * combination despite the same controller family, and has NOT been checked
     * this way -- see docs/HARDWARE-TEST-CHECKLIST.md. */
    BOARD_APPLY_ORIENTATION(panel, true, true, false);
    esp_lcd_panel_set_gap(panel, PANEL_GAP_X, PANEL_GAP_Y);
    esp_lcd_panel_disp_on_off(panel, true);

    /* Backlight last, so nobody sees a bright rectangle of uninitialised RAM. */
    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_TFT_BL, 1);

    out.panel = panel;
    ESP_LOGI(TAG, "%s display up: %dx%d", BOARD_NAME, out.width, out.height);
    return out;
}

void board_serial_start(void) {
    serial_uart_start(); /* CH9102 USB-UART bridge on UART0 */
}

board_input_caps_t board_input_caps(void) {
    /* Two buttons, no touch panel. See board.h. */
    return (board_input_caps_t){.buttons = 2, .touch = false};
}

void board_buttons_init(void) {
    /* Back to plain digital GPIO first. gpio_config() undoes neither a pad
     * hold nor RTC mux ownership; both survive a software reset, both outrank
     * the digital register, and both present as a permanently-pressed button
     * -- the section 7a fault. These boards get reflashed with other firmware.
     *
     * Idempotent and free when the pads were already clean. Returns ignored:
     * rtc_gpio_deinit() answers INVALID_ARG on a pin with no RTC function,
     * which is the correct outcome there.
     *
     * Not confirmed as the cause of 7a -- that needs the board. It is the
     * cheapest hypothesis, and one flash cycle to rule out. */
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_BUTTON_1);
    gpio_hold_dis(PIN_BUTTON_2);
    rtc_gpio_deinit(PIN_BUTTON_1);
    rtc_gpio_deinit(PIN_BUTTON_2);

    /* Button 1 (GPIO0) gets the internal pull-up. Button 2 (GPIO35) cannot:
     * GPIO34-39 on the classic ESP32 are input-only and have no internal
     * pull resistors at all, so it depends entirely on the board's external
     * one. If a clone omits that, the pin floats and button 2 will appear to
     * be pressed at random -- which on this device means spurious cancels
     * and spurious browse steps. */
    gpio_config_t pulled = {
        .pin_bit_mask = 1ULL << PIN_BUTTON_1,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&pulled);

    gpio_config_t floating = {
        .pin_bit_mask = 1ULL << PIN_BUTTON_2,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&floating);
}

bool board_button_1_pressed(void) {
    return gpio_get_level(PIN_BUTTON_1) == 0;
}

bool board_button_2_pressed(void) {
    return gpio_get_level(PIN_BUTTON_2) == 0;
}

#endif /* LNURLVAULT_BOARD_T_DISPLAY */
