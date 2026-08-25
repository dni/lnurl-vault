/* Every screen this firmware can draw, at both panel geometries, as PNGs.
 *
 *     make preview                    -> PNGs in ./preview/
 *     make preview PREVIEW_OUT=/tmp/shots PREVIEW_ZOOM=1
 *
 * Drawn by src/ui/display.c itself (see hostgfx/hostgfx.h), so what comes out
 * is what the glass gets. Amounts go through note_display.c rather than being
 * typed in here, so the grouping and unit are the real ones. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "boot_screen.h"
#include "display.h"
#include "hostgfx.h"
#include "note_display.h"

/* The panels are small; at 1:1 they are hard to look at. */
#define ZOOM_DEFAULT 3

static const char *g_dir = "preview";
static int g_zoom = ZOOM_DEFAULT;
static const char *g_board = "";
static int g_written;
static int g_failed;

static void shot(const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s-%s.png", g_dir, g_board, name);
    if (hostgfx_write_png(path, g_zoom) == 0) {
        printf("  %s  (%dx%d at %dx)\n", path, hostgfx_width(), hostgfx_height(), g_zoom);
        g_written++;
    } else {
        printf("  FAILED to write %s\n", path);
        g_failed++;
    }
}

/* Fresh panel per screen, so nothing a previous card left behind can make
 * this one look better than it is. */
static void fresh(int w, int h) {
    hostgfx_reset(w, h);
    display_init();
}

/* Formatted the way ui_task.c formats it. */
static void note_card(display_state_t state, const char *action, uint64_t msat,
                      const char *label, const char *id, const char *hint) {
    char amount[NOTE_AMOUNT_BUF];
    char unit[8];
    char shown[24];
    note_format_amount_parts(msat, amount, sizeof(amount), unit, sizeof(unit));
    note_format_label(label, shown, sizeof(shown));
    display_note_detail(state, action, amount, unit, shown, id, hint);
}

#define HINT "HOLD BTN1 2s"
#define RELEASE "LET GO FIRST"

static void render_board(const char *board, int w, int h) {
    g_board = board;
    printf("%s (%dx%d)\n", board, w, h);

    /* The boot screen animates; vTaskDelay is a no-op here, so what lands is
     * the last frame of each stage. Three shots because it is three screens,
     * and the middle one -- a checklist half filled in -- is the one nobody
     * would think to look at and the one a failing device stops on. */
    fresh(w, h);
    boot_screen_begin("0.0.7", board);
    shot("00-boot-name");

    fresh(w, h);
    boot_screen_begin("0.0.7", board);
    boot_screen_step("STORAGE", true);
    boot_screen_step("IDENTITY", true);
    shot("00b-boot-partway");

    fresh(w, h);
    boot_screen_begin("0.0.7", board);
    boot_screen_step("STORAGE", true);
    boot_screen_step("IDENTITY", true);
    boot_screen_step("LINK", true);
    shot("00c-boot-ready");

    fresh(w, h);
    boot_screen_begin("0.0.7", board);
    boot_screen_step("STORAGE", false);
    boot_screen_step("IDENTITY", true);
    boot_screen_step("LINK", true);
    shot("00d-boot-storage-failed");

    fresh(w, h);
    display_message(DISPLAY_STATE_IDLE, "3 NOTES", "TAP TO VIEW", NULL);
    shot("01-idle");

    fresh(w, h);
    display_message(DISPLAY_STATE_IDLE, "NO NOTES", "PAIR TO ADD", NULL);
    shot("01b-idle-empty");

    fresh(w, h);
    note_card(DISPLAY_STATE_BROWSE, "NOTE 3/12", 21000, "rent", "f822a462", NULL);
    shot("02-browse");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 21000, "rent", NULL, HINT);
    display_progress(0);
    shot("03-confirm-show-secret");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 21000, "rent", NULL, HINT);
    display_progress(450);
    shot("04-confirm-holding");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "MARK SPENT", 50000, "wos handoff", NULL, HINT);
    display_progress(0);
    shot("05-confirm-mark-spent");

    fresh(w, h);
    display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "WIPE ALL", NULL, NULL, NULL, NULL, HINT);
    display_progress(0);
    shot("06-confirm-wipe-all");

    fresh(w, h);
    display_note_detail(DISPLAY_STATE_CONFIRM_PENDING, "NEW FIRMWARE", NULL, NULL, NULL, NULL,
                        HINT);
    display_progress(0);
    shot("07-confirm-new-firmware");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 21000, "rent", NULL, RELEASE);
    display_progress(0);
    shot("07b-confirm-button-already-held");

    fresh(w, h);
    display_message(DISPLAY_STATE_APPROVED, "APPROVED", "SHOW SECRET", NULL);
    shot("08-approved");

    fresh(w, h);
    display_message(DISPLAY_STATE_DECLINED, "DECLINED", "SHOW SECRET", "NOTHING DONE");
    shot("09-declined");

    fresh(w, h);
    display_message(DISPLAY_STATE_EXPIRED, "NO ANSWER", "WIPE ALL", "NOTHING DONE");
    shot("10-expired");

    fresh(w, h);
    display_message(DISPLAY_STATE_DECLINED, "FAILED", "NOT SHOWN", NULL);
    shot("10b-unveil-failed");

    /* The awkward ones: where a layout stops being pretty and starts being
     * wrong. */
    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 2100000000, "cold store", NULL, HINT);
    display_progress(0);
    shot("11-confirm-large-amount");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 21000,
              "a label far longer than any panel here can hold", NULL, HINT);
    display_progress(0);
    shot("12-confirm-long-label");

    fresh(w, h);
    note_card(DISPLAY_STATE_CONFIRM_PENDING, "SHOW SECRET", 1, "dust", NULL, HINT);
    display_progress(0);
    shot("13-confirm-sub-sat");

    fresh(w, h);
    note_card(DISPLAY_STATE_BROWSE, "NOTE 3/12", 21000, NULL, "f822a462", NULL);
    shot("14-browse-no-label");
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    if (argc > 2) {
        const int zoom = atoi(argv[2]);
        if (zoom > 0) {
            g_zoom = zoom;
        }
    }
    if (mkdir(g_dir, 0755) != 0) {
        /* Existing is fine; anything else shows up as a write failure below. */
    }

    /* Both panels src/board/ supports. */
    render_board("t-display", 240, 135);
    render_board("t-display-s3", 320, 170);

    printf("\n%d screen(s) written to %s/\n", g_written, g_dir);
    if (g_failed) {
        printf("%d FAILED\n", g_failed);
        return 1;
    }
    return 0;
}
