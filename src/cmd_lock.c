#include "cmd_lock.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t g_mutex;

void cmd_lock_init(void) {
    g_mutex = xSemaphoreCreateMutex();
}

void cmd_lock_acquire(void) {
    xSemaphoreTake(g_mutex, portMAX_DELAY);
}

void cmd_lock_release(void) {
    xSemaphoreGive(g_mutex);
}
