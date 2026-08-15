/* NOTE: unverified by compilation (see README.md). Uses standard ESP-IDF
 * GPIO/FreeRTOS/esp_timer APIs, which have been stable across recent IDF
 * releases — the piece most likely to need adjustment is board_pins.h, not
 * this file. */
#include "buttons.h"

#include <stdbool.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void buttons_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_1) | (1ULL << PIN_BUTTON_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool pressed(int pin) {
    return gpio_get_level(pin) == 0; /* active-low, per pull-up config above */
}

confirm_result_t buttons_wait_confirm(uint32_t timeout_ms) {
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (pressed(PIN_BUTTON_1) || pressed(PIN_BUTTON_2)) {
        if (esp_timer_get_time() > deadline_us) {
            return CONFIRM_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    while (esp_timer_get_time() < deadline_us) {
        if (pressed(PIN_BUTTON_1)) {
            vTaskDelay(pdMS_TO_TICKS(30)); /* debounce */
            if (pressed(PIN_BUTTON_1)) {
                return CONFIRM_YES;
            }
        }
        if (pressed(PIN_BUTTON_2)) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (pressed(PIN_BUTTON_2)) {
                return CONFIRM_NO;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return CONFIRM_TIMEOUT;
}
