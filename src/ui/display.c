/* NOTE: unverified by compilation (see README.md). esp_lcd's panel-io/
 * panel-vendor struct field names have changed across ESP-IDF releases
 * (e.g. IDF 5.x renamed `color_space` to `rgb_ele_order` with a different
 * enum) — if a build error points here, that's the likely mismatch to
 * reconcile against your installed IDF version's esp_lcd_panel_io.h /
 * esp_lcd_panel_vendor.h.
 *
 * v1 deliberately does NOT render note text (id/amount/label) on-screen —
 * see README.md's "Known limitations" section for why (a hand-transcribed
 * bitmap font couldn't be visually verified in this environment, and a
 * wrong glyph is a silent correctness risk this project would rather not
 * ship) and what's needed to add it later (LVGL, most likely). This still
 * gives a real, meaningful visual signal — a distinct full-screen color per
 * state — and the actual security-relevant gating (confirm/cancel/timeout)
 * in ui/buttons.c is fully functional regardless of what's drawn. */
#include "display.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_SPI_HOST SPI2_HOST

static esp_lcd_panel_handle_t g_panel = NULL;

static uint16_t color_for_state(display_state_t state) {
    switch (state) {
        case DISPLAY_STATE_IDLE:
            return 0x39C7; /* muted blue, RGB565 */
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

void display_init(void) {
    gpio_config_t power_cfg = {
        .pin_bit_mask = 1ULL << PIN_TFT_POWER_ON,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&power_cfg);
    gpio_set_level(PIN_TFT_POWER_ON, 1);

    gpio_config_t bl_cfg = {
        .pin_bit_mask = 1ULL << PIN_TFT_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_cfg);
    gpio_set_level(PIN_TFT_BL, 1);

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_TFT_SCLK,
        .mosi_io_num = PIN_TFT_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * 2,
    };
    spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_TFT_CS,
        .dc_gpio_num = PIN_TFT_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_config, &io_handle);

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_TFT_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    esp_lcd_new_panel_st7789(io_handle, &panel_config, &g_panel);

    esp_lcd_panel_reset(g_panel);
    esp_lcd_panel_init(g_panel);
    esp_lcd_panel_invert_color(g_panel, true);
    esp_lcd_panel_disp_on_off(g_panel, true);

    display_set_state(DISPLAY_STATE_IDLE);
}

static display_state_t g_current_state = DISPLAY_STATE_IDLE;

static void fill_screen(uint16_t color) {
    if (!g_panel) {
        return;
    }
    static uint16_t line[LCD_WIDTH];
    for (int i = 0; i < LCD_WIDTH; i++) {
        line[i] = color;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
        esp_lcd_panel_draw_bitmap(g_panel, 0, y, LCD_WIDTH, y + 1, line);
    }
}

void display_set_state(display_state_t state) {
    g_current_state = state;
    fill_screen(color_for_state(state));
}

void display_flash_count(int count) {
    if (count < 1 || !g_panel) {
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
