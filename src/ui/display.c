/* Drawing only. Panel bring-up -- bus type, pins, rotation, colour inversion,
 * controller-RAM offset -- belongs to src/board/, so nothing here knows or
 * cares which board it is running on or how the glass is wired.
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

#include <string.h>

#include "font5x7.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_lcd_panel_handle_t g_panel = NULL;
static int g_width = 0;
static int g_height = 0;

/* Two full-width rows of pixels, ping-ponged between calls.
 *
 * esp_lcd_panel_draw_bitmap() queues a DMA transfer against the caller's
 * buffer and returns; the bytes are read later. Within one fill that is
 * harmless because every row holds the identical colour, so an in-flight
 * transfer reading a row we have "reused" sees exactly what it expected.
 * Across two fills of *different* colours it is not: the tail of the first
 * fill can still be in flight when the second overwrites the buffer, and
 * those rows land in the new colour. Alternating buffers per call gives the
 * previous fill its own intact copy to drain from.
 *
 * Allocated rather than static because geometry is now a runtime property of
 * the board, and DMA-capable because that is what the panel bus sends from. */
static uint16_t *g_rows[2] = {NULL, NULL};
static int g_row_turn = 0;

static display_state_t g_current_state = DISPLAY_STATE_IDLE;

/* One glyph cell at the largest permitted scale, reused for every character.
 * Per-glyph transfers rather than one buffer for a whole line: a full line at
 * scale 4 on the widest panel is over 13KB of DMA-capable RAM held
 * permanently, against 1.1KB here, and a screen drawn once per approval does
 * not need the transfer count. */
#define GLYPH_CELL_PIXELS (FONT5X7_WIDTH * DISPLAY_MAX_TEXT_SCALE * FONT5X7_HEIGHT * DISPLAY_MAX_TEXT_SCALE)
static uint16_t *g_glyph = NULL;

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
        case DISPLAY_STATE_EXPIRED:
            return 0x8410; /* mid grey: visibly not the amber of a live
                            * prompt, and visibly not the red of a refusal */
        default:
            return 0x0000;
    }
}

void display_init(void) {
    board_display_t d = board_display_init();
    g_panel = d.panel;
    g_width = d.width;
    g_height = d.height;

    if (g_panel) {
        for (int i = 0; i < 2; i++) {
            g_rows[i] = heap_caps_malloc((size_t)g_width * sizeof(uint16_t), MALLOC_CAP_DMA);
        }
        g_glyph = heap_caps_malloc(GLYPH_CELL_PIXELS * sizeof(uint16_t), MALLOC_CAP_DMA);
        if (!g_rows[0] || !g_rows[1] || !g_glyph) {
            /* No row buffers means no drawing. Report it as no panel rather
             * than as half-working, so display_ready() tells the truth and
             * the secret-disclosing paths refuse instead of proceeding
             * blind. */
            g_panel = NULL;
        }
    }

    display_set_state(DISPLAY_STATE_IDLE);
}

bool display_ready(void) {
    return g_panel != NULL && g_rows[0] != NULL && g_rows[1] != NULL && g_glyph != NULL;
}

int display_width(void) {
    return g_width;
}

int display_height(void) {
    return g_height;
}

void display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (!display_ready() || w <= 0 || h <= 0) {
        return;
    }
    if (x < 0 || y < 0 || x + w > g_width || y + h > g_height) {
        return;
    }
    uint16_t *row = g_rows[g_row_turn];
    g_row_turn ^= 1;

    for (int i = 0; i < w; i++) {
        row[i] = color;
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

void display_text(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg) {
    if (!display_ready() || !text) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > DISPLAY_MAX_TEXT_SCALE) {
        scale = DISPLAY_MAX_TEXT_SCALE;
    }

    const int cw = FONT5X7_WIDTH * scale;
    const int ch = FONT5X7_HEIGHT * scale;
    if (y < 0 || y + ch > g_height) {
        return; /* off-panel vertically: nothing sensible to clip to */
    }

    for (const char *p = text; *p; p++) {
        if (x + cw > g_width) {
            return; /* ran out of screen -- see the header on why this clips */
        }
        const uint8_t *glyph = font5x7_glyph(*p);

        for (int gx = 0; gx < FONT5X7_WIDTH; gx++) {
            for (int sx = 0; sx < scale; sx++) {
                const int col = gx * scale + sx;
                for (int gy = 0; gy < FONT5X7_HEIGHT; gy++) {
                    const uint16_t c = (glyph[gx] >> gy) & 1 ? fg : bg;
                    for (int sy = 0; sy < scale; sy++) {
                        g_glyph[(gy * scale + sy) * cw + col] = c;
                    }
                }
            }
        }
        esp_lcd_panel_draw_bitmap(g_panel, x, y, x + cw, y + ch, g_glyph);
        x += FONT5X7_ADVANCE * scale;
    }
}

/* The largest scale at which `text` fits `avail_w`, never below
 * FONT5X7_MIN_READABLE_SCALE even if that means it will not fit.
 *
 * Shortening beats shrinking: a line nobody can read conveys nothing whether
 * or not it is complete, and display_text() clips at the panel edge anyway.
 * That order was decided by a person looking at the screen and saying the text
 * was too small, not by arithmetic. */
static int fit_scale(const char *text, int avail_w, int max_scale) {
    if (!text || !text[0]) {
        return FONT5X7_MIN_READABLE_SCALE;
    }
    const int len = (int)strlen(text);
    if (max_scale > DISPLAY_MAX_TEXT_SCALE) {
        max_scale = DISPLAY_MAX_TEXT_SCALE;
    }
    for (int scale = max_scale; scale > FONT5X7_MIN_READABLE_SCALE; scale--) {
        /* The last glyph needs its cell, not its advance. */
        const int w = (len - 1) * FONT5X7_ADVANCE * scale + FONT5X7_WIDTH * scale;
        if (w <= avail_w) {
            return scale;
        }
    }
    return FONT5X7_MIN_READABLE_SCALE;
}

void display_note_detail(display_state_t state, const char *amount_num,
                          const char *amount_unit, const char *label, const char *id) {
    if (!display_ready()) {
        return;
    }
    g_current_state = state;
    const uint16_t bg = color_for_state(state);
    const uint16_t ink = 0x0000;
    fill_screen(bg);

    /* Every line is drawn at the largest scale that fits the panel, rather
     * than at a scale picked from the panel height. The first version did the
     * latter and produced 21-pixel digits with a 14-pixel label, which a
     * person on real hardware could not read. Fitting to the width instead
     * means a short amount gets big automatically, which is the common case.
     *
     * The lower band is left clear for display_progress(). */
    const int margin = 6;
    const int avail = g_width - 2 * margin;
    /* Keep out of the progress bar's band -- see display_progress(). */
    const int usable_h = g_height - (g_height / 8) - (g_height / 12) - margin;

    int y = margin;

    /* The digits get the FULL width, at the largest scale that fits. An
     * earlier version reserved room for the unit on the same line, which cost
     * 90 of 228 pixels and held a seven-digit amount down to the same 21-pixel
     * height a person had already told us was too small to read. The unit goes
     * on the next line with the label instead: it is a word, and the digits are
     * the thing a mistake costs money on. */
    if (amount_num && amount_num[0]) {
        const int scale = fit_scale(amount_num, avail, DISPLAY_MAX_TEXT_SCALE);
        display_text(margin, y, amount_num, scale, ink, bg);
        y += FONT5X7_HEIGHT * scale + 5;
    }

    /* Unit and label share a line: "sats  rent". */
    char second[40];
    second[0] = '\0';
    if (amount_unit && amount_unit[0]) {
        size_t n = strlen(amount_unit);
        if (n > sizeof(second) - 3) {
            n = sizeof(second) - 3;
        }
        memcpy(second, amount_unit, n);
        second[n] = '\0';
    }
    if (label && label[0]) {
        size_t used = strlen(second);
        if (used > 0 && used + 2 < sizeof(second)) {
            second[used++] = ' ';
            second[used++] = ' ';
            second[used] = '\0';
        }
        size_t room = sizeof(second) - used - 1;
        size_t n = strlen(label);
        if (n > room) {
            n = room;
        }
        memcpy(second + used, label, n);
        second[used + n] = '\0';
    }
    if (second[0] && y + FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE <= usable_h) {
        const int scale = fit_scale(second, avail, FONT5X7_MIN_READABLE_SCALE + 1);
        display_text(margin, y, second, scale, ink, bg);
        y += FONT5X7_HEIGHT * scale + 4;
    }
    if (id && id[0] && y + FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE <= usable_h) {
        display_text(margin, y, id, FONT5X7_MIN_READABLE_SCALE, ink, bg);
    }
}

void display_progress(uint16_t permille) {
    if (!display_ready()) {
        return;
    }
    if (permille > 1000) {
        permille = 1000;
    }

    /* A band across the LOWER part of the panel, inset from the edges so it
     * reads as a bar rather than as the screen changing colour. Low rather
     * than centred so it cannot paint over the note detail
     * display_confirm_note() draws above it -- the whole point of the hold is
     * that the owner can read what they are approving while they hold. */
    int margin = g_width / 8;
    int track_w = g_width - 2 * margin;
    int bar_h = g_height / 8;
    int y = g_height - bar_h - (g_height / 12);
    if (track_w <= 2 || bar_h <= 2) {
        return; /* a panel too small to draw a meaningful bar on */
    }

    int filled = (int)(((int32_t)track_w * permille) / 1000);

    /* Track, then fill. Repainting the whole track each call is what makes
     * this safe to call at any rate and in any order, including going
     * backwards if a hold restarts. */
    display_fill_rect(margin, y, track_w, bar_h, 0x0000);
    if (filled > 0) {
        display_fill_rect(margin, y, filled, bar_h, 0xFFFF);
    }
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
