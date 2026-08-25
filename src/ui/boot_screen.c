/* See boot_screen.h. Drawing only, through display.c's primitives, so the
 * boot screen cannot drift from the palette or the geometry the cards use.
 *
 * Every timing here is a delay this firmware did not used to take. That is
 * deliberate and it is the point -- but it is also latency before the device
 * answers a host, so the numbers are named and kept together rather than
 * scattered as magic values through the frames. */
#include "boot_screen.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "font5x7.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "palette.h"

/* The shutter: how long the ground takes to open from the centre, and in how
 * many steps. Frames, not a duration, because a frame is a full-width blit
 * and the panel is what sets the floor. */
#define SHUTTER_FRAMES 18
#define SHUTTER_FRAME_MS 22

/* One character of the name per beat. Slow enough to read as typing rather
 * than as a slow draw. */
#define TYPE_MS 55

/* The name, before the checklist replaces it. */
#define NAME_HOLD_MS 450

/* Each checklist line, after it lands. */
#define STEP_HOLD_MS 220

/* The finished screen, before ui_task paints over it. */
#define DONE_HOLD_MS 650

/* Split rather than one string: "LNURL VAULT" across a 240px panel fits only
 * at the readable minimum, which is the size a note's label gets. The name of
 * the device should not be the smallest thing on its own boot screen. */
#define NAME_TOP "LNURL"
#define NAME_BOTTOM "VAULT"

/* Cards are what the boot screen animates into, so it borrows their state --
 * teal band, warm ground, warm ink. Nothing here is a second palette. */
#define BOOT_STATE DISPLAY_STATE_IDLE

static int g_steps;      /* checklist rows drawn so far */
static int g_step_top;   /* where the next one goes */
static int g_step_max;   /* one past the last row that fits */
static char g_strip[40]; /* version and board, for the band */

static int band(void) {
    return display_band_height();
}

static int bar(void) {
    return display_bar_height();
}

/* Ink on the band and the bar is the card's own ground -- see palette.h on
 * why one value doing both jobs is what keeps them feeling like one device. */
static uint16_t band_ink(void) {
    return display_state_color(BOOT_STATE);
}

/* Below the card minimum, deliberately.
 *
 * FONT5X7_MIN_READABLE_SCALE is a rule about note CONTENT -- an amount or a
 * label read at arm's length while deciding something. The identity strip is
 * neither: it is the firmware version and the board name, read once, up
 * close, by someone already looking for them, usually to put in a bug report.
 * At scale 3 the narrow panel holds thirteen characters and
 * "v0.0.7  t-display" is seventeen, so the rule would not make it more
 * readable -- it would cut the board name off, which is exactly the half a
 * bug report needs. */
#define STRIP_SCALE 2

static void draw_band(const char *text) {
    const int h = band();
    const int w = display_width();
    display_fill_rect(0, 0, w, h, display_state_accent(BOOT_STATE));
    if (text && text[0]) {
        char fitted[sizeof(g_strip)];
        snprintf(fitted, sizeof(fitted), "%s", text);
        /* Cut to what the panel holds rather than letting display_text clip
         * mid-glyph: a board name sliced down the middle of a letter reads as
         * a different board name, not as a truncated one. */
        const int room = (w - 12) / (FONT5X7_ADVANCE * STRIP_SCALE);
        if (room > 0 && (int)strlen(fitted) > room) {
            fitted[room] = '\0';
        }
        const int th = FONT5X7_HEIGHT * STRIP_SCALE;
        display_text(6, (h - th) / 2, fitted, STRIP_SCALE, band_ink(),
                     display_state_accent(BOOT_STATE));
    }
}

/* The bar as a count, not a percentage: three subsystems, and the bar says how
 * many of them have answered. It is the same bar the approval hold fills, in
 * the same place, which is the point -- by the time a prompt uses it the
 * shape is already familiar. */
static void draw_bar(int done, int total) {
    const int h = bar();
    const int y = display_height() - h;
    const int w = display_width();
    display_fill_rect(0, y, w, h, PALETTE_TRACK);
    if (done > 0 && total > 0) {
        int filled = (w * done) / total;
        if (filled > w) {
            filled = w;
        }
        if (filled > 0) {
            display_fill_rect(0, y, filled, h, display_state_accent(BOOT_STATE));
        }
    }
}

static void clear_middle(void) {
    const int top = band();
    const int h = display_height() - top - bar();
    if (h > 0) {
        display_fill_rect(0, top, display_width(), h, display_state_color(BOOT_STATE));
    }
}

/* The shutter.
 *
 * Starts as a full field of the state colour and opens the ground out from
 * the centre until all that is left of it is the header band and the bar --
 * so the animation does not decorate the card geometry, it ARRIVES at it. The
 * opened rectangle only ever grows, so each frame is one blit over the last
 * rather than a repaint. */
static void shutter(void) {
    const int w = display_width();
    const int h = display_height();
    const int top = band();
    const int span = h - top - bar();
    if (span <= 0) {
        display_set_state(BOOT_STATE);
        return;
    }
    const int centre = top + span / 2;

    display_fill_rect(0, 0, w, h, display_state_accent(BOOT_STATE));
    for (int f = 1; f <= SHUTTER_FRAMES; f++) {
        int gap = (span * f) / SHUTTER_FRAMES;
        int y = centre - gap / 2;
        if (y < top) {
            y = top;
        }
        if (y + gap > top + span) {
            gap = top + span - y;
        }
        if (gap > 0) {
            display_fill_rect(0, y, w, gap, display_state_color(BOOT_STATE));
        }
        vTaskDelay(pdMS_TO_TICKS(SHUTTER_FRAME_MS));
    }
}

/* Types `text` in, one character per beat, at a fixed left edge so the line
 * grows rightwards instead of re-centring under itself on every frame -- which
 * looks like a bug rather than like typing. */
static void type_line(int y, const char *text, int scale) {
    const int full = font5x7_text_width(text, scale);
    int x = (display_width() - full) / 2;
    if (x < 0) {
        x = 0;
    }
    char buf[16];
    const size_t n = strlen(text) < sizeof(buf) - 1 ? strlen(text) : sizeof(buf) - 1;
    for (size_t i = 1; i <= n; i++) {
        memcpy(buf, text, i);
        buf[i] = '\0';
        display_text(x, y, buf, scale, display_state_ink(BOOT_STATE),
                     display_state_color(BOOT_STATE));
        vTaskDelay(pdMS_TO_TICKS(TYPE_MS));
    }
}

void boot_screen_begin(const char *version, const char *board) {
    g_steps = 0;
    g_strip[0] = '\0';
    if (version && board) {
        snprintf(g_strip, sizeof(g_strip), "v%s  %s", version, board);
    } else if (version) {
        snprintf(g_strip, sizeof(g_strip), "v%s", version);
    }

    if (!display_ready()) {
        return; /* a dead panel still boots -- it just has nothing to say */
    }

    shutter();
    draw_band(NULL);
    draw_bar(0, 3);

    /* The name at the largest scale both halves fit at, which on the narrow
     * panel is several times what the old single line managed.
     *
     * Fitted to the HEIGHT as well as the width. Width alone picks the same
     * scale on both panels -- five characters is never what runs out first --
     * and then the second line lands in the progress bar on the shorter one.
     * The two lines want air between them too: at scale 6 the glyphs are 42
     * pixels tall and a 3-pixel card gap reads as one squashed word. */
    const int avail = display_width();
    const int top = band();
    const int span = display_height() - top - bar();
    int scale = font5x7_fit_scale(NAME_TOP, avail, DISPLAY_MAX_TEXT_SCALE);
    const int other = font5x7_fit_scale(NAME_BOTTOM, avail, DISPLAY_MAX_TEXT_SCALE);
    if (other < scale) {
        scale = other;
    }
    /* Half a line of air demanded on top of the block itself. Without it the
     * fit is satisfied by a wordmark that touches the band above and the bar
     * below, which does not read as big -- it reads as too big for the
     * screen. The narrow panel lands a size smaller for it; the S3 has the
     * room and keeps the larger one. */
    while (scale > 1 &&
           2 * FONT5X7_HEIGHT * scale + (FONT5X7_HEIGHT * scale) / 5 +
                   (FONT5X7_HEIGHT * scale) / 2 >
               span) {
        scale--;
    }
    const int line_h = FONT5X7_HEIGHT * scale;
    const int gap = line_h / 5;
    int y = top + (span - (2 * line_h + gap)) / 2;
    if (y < top) {
        y = top;
    }
    type_line(y, NAME_TOP, scale);
    type_line(y + line_h + gap, NAME_BOTTOM, scale);
    vTaskDelay(pdMS_TO_TICKS(NAME_HOLD_MS));
}

void boot_screen_step(const char *label, bool ok) {
    if (!display_ready() || !label || !label[0]) {
        return;
    }

    /* The first step is what turns the name into the checklist: the band
     * takes over saying which firmware and which board, and the middle is
     * cleared for what actually came up. */
    if (g_steps == 0) {
        draw_band(g_strip);
        clear_middle();
        g_step_top = band() + FONT5X7_CARD_GAP;
        g_step_max = display_height() - bar();
    }

    const int scale = FONT5X7_MIN_READABLE_SCALE;
    const int line_h = FONT5X7_HEIGHT * scale;
    const int y = g_step_top + g_steps * (line_h + FONT5X7_CARD_GAP);
    g_steps++;
    draw_bar(g_steps, 3);
    if (y + line_h > g_step_max) {
        return; /* out of panel; the bar still counts it */
    }

    /* Label left, verdict right, so the column of answers can be read down
     * without reading the labels at all -- which is how anyone actually
     * checks a list like this. */
    const char *verdict = ok ? "OK" : "FAIL";
    display_text(6, y, label, scale, display_state_ink_dim(BOOT_STATE),
                 display_state_color(BOOT_STATE));
    const int vw = font5x7_text_width(verdict, scale);
    int vx = display_width() - 6 - vw;
    if (vx < 0) {
        vx = 0;
    }
    /* A failure gets the full ink weight and the dimmed label beside it: the
     * one line worth noticing must not be the one that looks like the rest. */
    display_text(vx, y, verdict, scale,
                 ok ? display_state_ink_dim(BOOT_STATE) : display_state_ink(BOOT_STATE),
                 display_state_color(BOOT_STATE));
    vTaskDelay(pdMS_TO_TICKS(STEP_HOLD_MS));
}

void boot_screen_done(void) {
    if (!display_ready()) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(DONE_HOLD_MS));
}
