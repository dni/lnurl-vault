/* Confirmed to compile against ESP-IDF 6.0.1 as part of a full firmware
 * build (see README.md's "Status" section). Uses standard ESP-IDF
 * GPIO/esp_timer APIs, which have been stable across recent IDF releases —
 * board_pins.h is the piece most likely to need adjustment on real
 * hardware, not this file (see that header's own comment). The actual
 * gesture logic (debounce, tap vs. chord) lives in the portable,
 * unit-tested src/proto/button_fsm.c; this file is deliberately just a thin
 * adapter feeding it real hardware state — real button behavior itself is
 * still unverified without a physical board. */
#include "buttons.h"

#include "board_pins.h"
#include "button_fsm.h"
#include "driver/gpio.h"
#include "esp_timer.h"

static button_fsm_t g_fsm;

void buttons_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_BUTTON_1) | (1ULL << PIN_BUTTON_2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    button_fsm_init(&g_fsm);
}

button_event_t buttons_poll(void) {
    bool b1 = gpio_get_level(PIN_BUTTON_1) == 0; /* active-low, per pull-up config above */
    bool b2 = gpio_get_level(PIN_BUTTON_2) == 0;
    return button_fsm_poll(&g_fsm, b1, b2, esp_timer_get_time());
}
