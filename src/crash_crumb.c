#include "crash_crumb.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "crash_crumb";

#define CRUMB_MAGIC 0x4C4E5643u /* 'LNVC' -- distinguishes a real breadcrumb
                                 * from whatever RTC memory holds on a cold
                                 * boot, which is not guaranteed to be zero */
#define CRUMB_CMD_BUF 24        /* longest command name is new_secret_pair (15) */

typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    /* Command name only, never its arguments -- see crash_crumb.h. */
    char cmd[CRUMB_CMD_BUF];
    bool cmd_in_flight;
} crumb_t;

/* RTC_NOINIT_ATTR: placed in RTC slow memory and NOT zeroed by the startup
 * code, so it carries across a panic, a watchdog reset and esp_restart().
 * A power cycle loses it, which is the intended lifetime. */
static RTC_NOINIT_ATTR crumb_t g_crumb;

/* Snapshot of what the previous boot left, taken before g_crumb is re-armed
 * for this one. */
static char g_last_cmd[CRUMB_CMD_BUF];
static bool g_have_last_cmd;
static esp_reset_reason_t g_reason;
static uint32_t g_boot_count;

const char *crash_crumb_reset_reason(void) {
    switch (g_reason) {
        case ESP_RST_POWERON:
            return "poweron";
        case ESP_RST_EXT:
            return "ext";
        case ESP_RST_SW:
            return "sw";
        case ESP_RST_PANIC:
            return "panic";
        case ESP_RST_INT_WDT:
            return "int_wdt";
        case ESP_RST_TASK_WDT:
            return "task_wdt";
        case ESP_RST_WDT:
            return "wdt";
        case ESP_RST_BROWNOUT:
            return "brownout";
        case ESP_RST_DEEPSLEEP:
            return "deepsleep";
        default:
            return "unknown";
    }
}

bool crash_crumb_last_boot_was_unexpected(void) {
    switch (g_reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        default:
            return false;
    }
}

const char *crash_crumb_last_cmd(void) {
    return g_have_last_cmd ? g_last_cmd : NULL;
}

uint32_t crash_crumb_boot_count(void) {
    return g_boot_count;
}

void crash_crumb_boot(void) {
    g_reason = esp_reset_reason();

    /* On a power-on boot the contents of RTC memory are undefined, so the
     * magic is the only thing separating a real breadcrumb from noise that
     * happens to look like one. Treat a power-on as "nothing survived"
     * regardless of what is sitting there. */
    bool valid = (g_crumb.magic == CRUMB_MAGIC) && (g_reason != ESP_RST_POWERON);

    if (valid) {
        g_boot_count = g_crumb.boot_count + 1;
        if (g_crumb.cmd_in_flight) {
            g_crumb.cmd[CRUMB_CMD_BUF - 1] = '\0';
            memcpy(g_last_cmd, g_crumb.cmd, sizeof(g_last_cmd));
            g_have_last_cmd = true;
        }
    } else {
        g_boot_count = 1;
    }

    if (crash_crumb_last_boot_was_unexpected()) {
        ESP_LOGE(TAG, "previous boot ended unexpectedly: reason=%s, boot=%u, in flight=%s",
                 crash_crumb_reset_reason(), (unsigned)g_boot_count,
                 g_have_last_cmd ? g_last_cmd : "(nothing)");
    } else {
        ESP_LOGI(TAG, "boot %u, reason=%s", (unsigned)g_boot_count, crash_crumb_reset_reason());
    }

    /* Re-arm for this boot. */
    g_crumb.magic = CRUMB_MAGIC;
    g_crumb.boot_count = g_boot_count;
    g_crumb.cmd_in_flight = false;
    g_crumb.cmd[0] = '\0';
}

void crash_crumb_set_cmd(const char *cmd) {
    if (!cmd) {
        return;
    }
    size_t n = strlen(cmd);
    if (n >= CRUMB_CMD_BUF) {
        n = CRUMB_CMD_BUF - 1;
    }
    memcpy(g_crumb.cmd, cmd, n);
    g_crumb.cmd[n] = '\0';
    g_crumb.cmd_in_flight = true;
}

void crash_crumb_clear_cmd(void) {
    g_crumb.cmd_in_flight = false;
}
