/* This board's display is an 8-bit parallel (Intel 8080 / "i80") bus, NOT
 * SPI — confirmed directly against LilyGo's own pin_config.h and ESP-IDF's
 * own bundled i80 LCD example (examples/peripherals/lcd/i80_controller/)
 * after a real device came up with backlight on, black screen, and no
 * serial output at all. An earlier version of this file called the SPI
 * panel-IO driver against a "MOSI"/"SCLK" pair that was never actually
 * connected to the display (see board_pins.h's header comment for the
 * full story) — esp_lcd_new_panel_io_spi() presumably hung or otherwise
 * misbehaved talking to nothing, which explains why display_init() being
 * first in app_main() meant *nothing* after it — BLE, storage, our own
 * USB-CDC — ever ran either. Confirmed to compile against ESP-IDF 6.0.1
 * with the i80 bus API below (see README.md's "Status" section) —
 * including the esp_lcd panel-vendor struct field names (`rgb_ele_order`
 * etc.), which have changed across ESP-IDF releases before (e.g. IDF 5.x
 * renamed `color_space` to `rgb_ele_order` with a different enum). Actual
 * on-screen rendering still hasn't been confirmed against a real display
 * — this fixes a confirmed-wrong bus/protocol, not a confirmed-working one.
 *
 * v1 deliberately does NOT render note text (id/amount/label) on screen. See
 * README.md's "Known limitations" for why, and note that this is the single
 * biggest remaining gap in the security model: a confirm prompt that cannot
 * name the note it is asking about is a "press to continue", not a
 * confirmation. Adding real text is the natural next step now that a panel
 * can actually be brought up and checked on a bench.
 *
 * Until then this still gives a real signal -- a distinct full-screen colour
 * per state, and a blinked-out position count while browsing -- and the
 * gating itself (confirm/cancel/timeout) is fully functional regardless of
 * what is drawn. */
#include "display.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t g_panel = NULL;
static uint16_t *g_line_buf = NULL; /* one row of pixels; see display_init()'s allocation for why
                                        this can't just be a plain static array */

static uint16_t color_for_state(display_state_t state) {
    switch (state) {
        case DISPLAY_STATE_IDLE:
            return 0x39C7; /* muted grey-blue, RGB565 */
        case DISPLAY_STATE_BROWSE:
            return 0x781F; /* purple */
        case DISPLAY_STATE_CONFIRM_PENDING:
            return 0xFEA0; /* amber */
        case DISPLAY_STATE_APPROVED:
            return 0x07E0; /* green */
        case DISPLAY_STATE_DECLINED:
            return 0xF800; /* red */
        default:
            return 0x0000;
    }
}

/* Extra ST7789 register tuning this exact panel needs beyond ESP-IDF's
 * generic init (SLPOUT/MADCTL/COLMOD/RAMCTRL only) — command bytes and
 * parameter values transcribed verbatim from TFT_eSPI's
 * `#else // TTGO ESP32 S3 T-Display` branch (TFT_Drivers/ST7789_Init.h),
 * not invented. Command opcodes cross-checked against TFT_eSPI's
 * ST7789_Defines.h (0xD6 is undocumented/manufacturer-specific there too —
 * TFT_eSPI sends it with no named constant, so neither do we). Sent after
 * esp_lcd_panel_init() and before disp_on_off(true), matching where these
 * commands sit in TFT_eSPI's own sequence (all before its DISPON). */
static esp_err_t send_lilygo_t_display_s3_tuning(esp_lcd_panel_io_handle_t io) {
    esp_err_t err;
#define TX(cmd, ...)                                                                             \
    do {                                                                                          \
        uint8_t params[] = {__VA_ARGS__};                                                         \
        err = esp_lcd_panel_io_tx_param(io, (cmd), params, sizeof(params));                       \
        if (err != ESP_OK) {                                                                      \
            return err;                                                                           \
        }                                                                                          \
    } while (0)

    err = esp_lcd_panel_io_tx_param(io, 0x13 /* NORON */, NULL, 0); /* Normal display mode on */
    if (err != ESP_OK) {
        return err;
    }
    TX(0xB2 /* PORCTRL */, 0x0b, 0x0b, 0x00, 0x33, 0x33);
    TX(0xB7 /* GCTRL */, 0x75);
    TX(0xBB /* VCOMS */, 0x28);
    TX(0xC0 /* LCMCTRL */, 0x2C);
    TX(0xC2 /* VDVVRHEN */, 0x01);
    TX(0xC3 /* VRHS */, 0x1F);
    TX(0xC6 /* FRCTR2 */, 0x13);
    TX(0xD0 /* PWCTRL1 */, 0xa7);
    TX(0xD0 /* PWCTRL1, sent again with a second payload — matches TFT_eSPI exactly */, 0xa4, 0xa1);
    TX(0xD6 /* undocumented/manufacturer-specific, per TFT_eSPI */, 0xa1);
    TX(0xE0 /* PVGAMCTRL */, 0xf0, 0x05, 0x0a, 0x06, 0x06, 0x03, 0x2b, 0x32, 0x43, 0x36, 0x11, 0x10,
       0x2b, 0x32);
    TX(0xE1 /* NVGAMCTRL */, 0xf0, 0x08, 0x0c, 0x0b, 0x09, 0x24, 0x2b, 0x22, 0x43, 0x38, 0x15, 0x16,
       0x2f, 0x37);
#undef TX
    return ESP_OK;
}

/* Every step below is checked and bails out on failure (logging why)
 * rather than plowing ahead into esp_lcd_panel_* calls on a NULL handle,
 * which would crash. A display fault has no reason to take the rest of
 * the device down with it — buttons, BLE, serial, and the vault itself
 * have no dependency on the screen actually working, and fill_screen()/
 * display_flash_count()/display_panel_handle() already treat a NULL
 * g_panel as "no-op", not an error, for exactly this reason. Found this
 * gap (nothing here checked a single return code) while diagnosing a real
 * "backlight on, black screen, no serial output at all" report — the
 * backlight GPIO write below happens before any of the SPI/panel calls
 * that could fail, which independently explains why the backlight symptom
 * alone doesn't prove the panel init succeeded. */
void display_init(void) {
    board_display_t d = board_display_init();
    g_panel = d.panel;
    g_width = d.width;
    g_height = d.height;

    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_TFT_BL, 1);

    /* Pixel clock matches LilyGo's own factory example
     * (EXAMPLE_LCD_PIXEL_CLOCK_HZ, examples/factory/pin_config.h) rather
     * than an invented value — their own comment there notes "too low or
     * too high pixel clock may cause screen mosaic" for this exact
     * display. */
    esp_lcd_i80_bus_handle_t i80_bus = NULL;
    esp_lcd_i80_bus_config_t bus_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_TFT_DC,
        .wr_gpio_num = PIN_TFT_WR,
        .data_gpio_nums =
            {
                PIN_TFT_D0,
                PIN_TFT_D1,
                PIN_TFT_D2,
                PIN_TFT_D3,
                PIN_TFT_D4,
                PIN_TFT_D5,
                PIN_TFT_D6,
                PIN_TFT_D7,
            },
        .bus_width = 8,
        .max_transfer_bytes = LCD_WIDTH * 100 * sizeof(uint16_t),
        .dma_burst_size = 64,
    };
    esp_err_t err = esp_lcd_new_i80_bus(&bus_config, &i80_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_i80_bus failed: %s (display disabled, rest of device continues)",
                 esp_err_to_name(err));
        return;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i80_config_t io_config = {
        .cs_gpio_num = PIN_TFT_CS,
        .pclk_hz = 16 * 1000 * 1000,
        .trans_queue_depth = 10,
        .dc_levels =
            {
                .dc_idle_level = 0,
                .dc_cmd_level = 0,
                .dc_dummy_level = 0,
                .dc_data_level = 1,
            },
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle);
    if (err != ESP_OK || !io_handle) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_i80 failed: %s (display disabled, rest of device continues)",
                 esp_err_to_name(err));
        return;
    }

    /* esp_lcd_io_i80.h's own doc comment on this allocator: it "differs
     * from the normal 'heap_caps_*' functions in that it can also
     * automatically handle the alignment required by DMA burst, cache
     * line size, etc." — a plain `static uint16_t[]` (what this file used
     * before) has none of those guarantees. ESP-IDF's own bundled i80
     * example allocates its draw buffers exactly this way, not with
     * malloc/a static array — confirmed by reading that example, not
     * assumed. A misaligned buffer feeding DMA bursts is a very plausible
     * cause of "the bus is provably talking to the panel now, but the
     * image is glitchy" — corrupt data, not a wrong command/pin. */
    g_line_buf = esp_lcd_i80_alloc_draw_buffer(io_handle, LCD_WIDTH * sizeof(uint16_t), 0);
    if (!g_line_buf) {
        ESP_LOGE(TAG, "esp_lcd_i80_alloc_draw_buffer failed (display disabled, rest of device continues)");
        return;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        /* ESP-IDF's ST7789 driver defaults to LCD_RGB_DATA_ENDIAN_BIG
         * (confirmed by reading esp_lcd_panel_st7789.c directly), but the
         * uint16_t colors this file writes into the framebuffer are native
         * little-endian (as they are on any ESP32). Without this, every
         * pixel's bytes are swapped — this is confirmed, not speculative:
         * ESP-IDF's own bundled i80/LVGL example manually calls
         * lv_draw_sw_rgb565_swap() "because LCD is big-endian", i.e. hits
         * the exact same mismatch and works around it a different way. */
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io_handle, &panel_config, &g_panel);
    if (err != ESP_OK || !g_panel) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s (display disabled, rest of device continues)",
                 esp_err_to_name(err));
        g_panel = NULL;
        return;
    }

    if (esp_lcd_panel_reset(g_panel) != ESP_OK || esp_lcd_panel_init(g_panel) != ESP_OK) {
        ESP_LOGE(TAG, "panel reset/init failed (display disabled, rest of device continues)");
        g_panel = NULL;
        return;
    }

    /* Still missing after the i80-bus/tuning/endian fixes above: this
     * panel's ST7789 controller natively addresses a 240x320 RAM window,
     * but the physical glass is only 170x320, and esp_lcd_panel_init()
     * leaves MADCTL in the controller's default (no swap, no mirror)
     * portrait orientation with a zero gap. This file draws LCD_WIDTH x
     * LCD_HEIGHT (320x170, landscape) rows straight into that unrotated,
     * unoffset addressing.
     *
     * The exact swap_xy/mirror/gap values below are taken from
     * github.com/UsefulElectronics/esp32s3_lilygo_8bit_parallel_display_lvgl
     * (main/main.c) — not a generic ST7789 reference, but an esp_lcd-based
     * example targeting this exact board (T-Display-S3), this exact 8-bit
     * i80 parallel bus, and this exact esp_lcd_panel_st7789 driver, which
     * makes it a much stronger match than a same-size-panel-in-general
     * driver would be. A first attempt at this fix used
     * mirror_x=true/mirror_y=false, sourced from a generic 170x320-panel
     * rotation table (russhughes/s3lcd) rather than a board-specific
     * example — that turned a "bus/format correct, screen glitched"
     * failure into a *worse* one (reported on real hardware: solid black,
     * nothing visible at all), consistent with an incorrectly-mirrored
     * gap-adjusted draw window landing entirely outside the physical
     * glass's visible rows rather than randomly across them. Swapped to
     * this board-specific source's mirror_x=false/mirror_y=true instead;
     * swap_xy=true and gap=(0, 35) already matched between both sources,
     * so those weren't the problem. */
    if (esp_lcd_panel_swap_xy(g_panel, true) != ESP_OK ||
        esp_lcd_panel_mirror(g_panel, false, true) != ESP_OK ||
        esp_lcd_panel_set_gap(g_panel, 0, 35) != ESP_OK) {
        ESP_LOGE(TAG, "panel swap_xy/mirror/gap failed (display disabled, rest of device continues)");
        g_panel = NULL;
        return;
    }

    /* ESP-IDF's generic ST7789 init (just above) only sends SLPOUT/MADCTL/
     * COLMOD/RAMCTRL. This exact panel needs additional gamma/power/porch
     * tuning beyond that generic baseline — confirmed against TFT_eSPI's
     * own T-Display-S3-specific ST7789 init sequence (TFT_Drivers/
     * ST7789_Init.h, the `#else // TTGO ESP32 S3 T-Display` branch,
     * selected by this exact board's own Setup206_LilyGo_T_Display_S3.h
     * defining INIT_SEQUENCE_3), not invented. Without it the panel still
     * responds (this bug wasn't found until backlight+bus were both
     * already confirmed working) but with wrong contrast/gamma — a
     * plausible contributor to a "mostly white, colorful glitches" report,
     * alongside the endian fix above. */
    if (send_lilygo_t_display_s3_tuning(io_handle) != ESP_OK) {
        ESP_LOGE(TAG, "panel-specific tuning sequence failed (display disabled, rest of device continues)");
        g_panel = NULL;
        return;
    }

    if (esp_lcd_panel_invert_color(g_panel, true) != ESP_OK ||
        esp_lcd_panel_disp_on_off(g_panel, true) != ESP_OK) {
        ESP_LOGE(TAG, "panel invert/disp_on failed (display disabled, rest of device continues)");
        g_panel = NULL;
        return;
    }

    /* No positive confirmation existed anywhere in this function before —
     * every failure path above already logs why, but there was no way to
     * tell "reached the end and DISPON was sent" from "silently never got
     * here" without this. Real hardware has now shown backlight-on/
     * no-visible-image after two different swap_xy/mirror/gap attempts,
     * and that symptom is consistent with either an early return above
     * (DISPON never sent) or a fully successful init whose draw
     * coordinates still don't land on the visible glass — this line is
     * what distinguishes the two from a monitor log, instead of guessing
     * again. */
    ESP_LOGI(TAG, "display init complete: bus/panel/tuning/invert/disp_on all succeeded, entering IDLE");
    display_set_state(DISPLAY_STATE_IDLE);
}

bool display_ready(void) {
    return g_panel != NULL && g_rows[0] != NULL && g_rows[1] != NULL;
}

static void fill_screen(uint16_t color) {
    if (!g_panel || !g_line_buf) {
        return;
    }
    for (int i = 0; i < LCD_WIDTH; i++) {
        g_line_buf[i] = color;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, LCD_WIDTH, y + 1, g_line_buf);
    }
    for (int r = 0; r < h; r++) {
        esp_lcd_panel_draw_bitmap(g_panel, x, y + r, x + w, y + r + 1, row);
    }
}

static void fill_screen(uint16_t color) {
    display_fill_rect(0, 0, g_width, g_height, color);
}

void display_set_state(display_state_t state) {
    g_current_state = state;
    fill_screen(color_for_state(state));
}

void display_flash_count(int count) {
    if (count < 1 || !display_ready()) {
        return;
    }
    if (count > 20) {
        count = 20; /* don't turn a large note collection into a light show */
    }
    for (int i = 0; i < count; i++) {
        fill_screen(0xFFFF); /* white */
        vTaskDelay(pdMS_TO_TICKS(150));
        fill_screen(color_for_state(g_current_state));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

esp_lcd_panel_handle_t display_panel_handle(void) {
    return g_panel;
}
