#include "device_reboot.h"

#include "esp_system.h"
#include "esp_timer.h"

/* 10s: comfortably clears serial_cdc.c's own TX_GIVE_UP_US (8s) so a
 * response has every chance to actually leave the TX buffer first, even
 * under this device's documented worst-case response latency (README.md's
 * Status section). */
#define REBOOT_DELAY_US (10 * 1000 * 1000)

static void do_restart(void *arg) {
    (void)arg;
    esp_restart();
}

void device_reboot_delayed(void) {
    const esp_timer_create_args_t args = {
        .callback = &do_restart,
        .name = "device_reboot",
    };
    esp_timer_handle_t timer;
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, REBOOT_DELAY_US);
    }
}
