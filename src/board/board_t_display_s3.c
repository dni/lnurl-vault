/* LilyGo T-Display S3: ESP32-S3R8, ST7789 170x320 IPS panel, two buttons.
 *
 * The panel is on an 8-bit Intel 8080 (i80) PARALLEL bus, not SPI. An earlier
 * version of this firmware drove it as SPI on GPIO 12/13, which are not
 * connected to the panel at all -- the backlight came on and nothing was ever
 * drawn, which looks identical to a working board until you notice the screen
 * stays blank. Since every path that discloses a bearer secret is gated on
 * something appearing on that screen, the whole security model was inert.
 *
 * Pin numbers and the orientation triple below are taken from LilyGo's own
 * pin_config.h and factory example, not from memory:
 *   https://github.com/Xinyuan-LilyGO/T-Display-S3
 *
 * Still unverified against physical hardware. What a successful build proves
 * is that these are valid GPIOs and a valid bus configuration, not that they
 * are the right ones for the board on your desk. LilyGo has shipped more than
 * one revision under similar names (there is an AMOLED variant with a
 * different pinout entirely), so diff this against your board's own
 * pin_config.h before trusting it. See docs/HARDWARE-TEST-CHECKLIST.md.
 */
#include "board.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

const char *const BOARD_NAME = "t-display-s3";

/* --- pins (LilyGo pin_config.h) ---------------------------------------- */

#define PIN_POWER_ON 15 /* peripheral power rail; HIGH or the panel stays dark */
#define PIN_LCD_BL 38   /* backlight, active high */
#define PIN_LCD_RES 5
#define PIN_LCD_CS 6
#define PIN_LCD_DC 7
#define PIN_LCD_WR 8
#define PIN_LCD_RD 9 /* write-only panel: park HIGH so the controller never drives the bus */

#define PIN_LCD_D0 39
#define PIN_LCD_D1 40
#define PIN_LCD_D2 41
#define PIN_LCD_D3 42
#define PIN_LCD_D4 45
#define PIN_LCD_D5 46
#define PIN_LCD_D6 47
#define PIN_LCD_D7 48

#define PIN_BUTTON_1 0  /* labelled BOOT; also the confirm button */
#define PIN_BUTTON_2 14 /* the cancel button */

/* --- panel geometry ----------------------------------------------------- */

/* The ST7789 controller addresses 240x320 of RAM; this glass is a 170x320
 * window into it, offset by 35 columns. We drive it rotated into landscape,
 * so the offset lands on the axis the driver calls y once swap_xy is on. */
#define PANEL_W 320
#define PANEL_H 170
#define PANEL_GAP_X 0
#define PANEL_GAP_Y 35

/* Conservative for a first bring-up. The bus and this panel will go faster;
 * a wrong-looking screen is much easier to diagnose when timing is not also
 * a suspect. Raise it once the checklist passes. */
#define LCD_PCLK_HZ (10 * 1000 * 1000)

static void drive_high(int pin) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, 1);
}

board_display_t board_display_init(void) {
    board_display_t out = {.panel = NULL, .width = PANEL_W, .height = PANEL_H};

    /* Order matters: the peripheral rail feeds the panel, so it must be up
     * and settled before the controller is spoken to. RD is parked high for
     * the same reason it exists at all -- this is a write-only setup and a
     * floating RD can leave the controller driving the data lines against
     * us. */
    drive_high(PIN_POWER_ON);
    drive_high(PIN_LCD_RD);
    vTaskDelay(pdMS_TO_TICKS(50));

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .data_gpio_nums = {PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, PIN_LCD_D4, PIN_LCD_D5,
                            PIN_LCD_D6, PIN_LCD_D7},
        .bus_width = 8,
        /* One full-screen blit is the largest transfer we ever queue. */
        .max_transfer_bytes = PANEL_W * PANEL_H * sizeof(uint16_t),
    };
    esp_err_t err = esp_lcd_new_i80_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i80 bus init failed: %s", esp_err_to_name(err));
        return out;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_PCLK_HZ,
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
    err = esp_lcd_new_panel_io_i80(bus, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i80 panel io init failed: %s", esp_err_to_name(err));
        return out;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RES,
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

    /* The orientation triple, straight from LilyGo's factory example. Getting
     * any one of these wrong gives a picture that is drawn but wrong -- offset
     * by 35px, upside down, or with inverted colours -- rather than no picture
     * at all, so they are worth checking individually on the bench rather than
     * as a set. */
    esp_lcd_panel_invert_color(panel, true); /* IPS panel: inverted is correct */
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, false, true);
    esp_lcd_panel_set_gap(panel, PANEL_GAP_X, PANEL_GAP_Y);

    esp_lcd_panel_disp_on_off(panel, true);

    /* Backlight last, so the first thing anyone sees is an initialised panel
     * rather than a bright rectangle of noise. */
    drive_high(PIN_LCD_BL);

    out.panel = panel;
    ESP_LOGI(TAG, "%s display up: %dx%d", BOARD_NAME, out.width, out.height);
    return out;
}

void board_buttons_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_1) | (1ULL << PIN_BUTTON_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

/* Both buttons are active low against the internal pull-ups configured above. */
bool board_button_1_pressed(void) {
    return gpio_get_level(PIN_BUTTON_1) == 0;
}

bool board_button_2_pressed(void) {
    return gpio_get_level(PIN_BUTTON_2) == 0;
}
