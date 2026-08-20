/* Drawing only. Panel bring-up -- bus type, pins, rotation, colour inversion,
 * controller-RAM offset -- belongs to src/board/, so nothing here knows or
 * cares which board it is running on or how the glass is wired.
 *
 * Three kinds of screen live here. A note card (display_note_detail) is a
 * dense set of fields fitted into a panel that barely holds them: the verb,
 * the amount, the unit and label, and the gesture. A message
 * (display_message) is two or three short lines with the screen to
 * themselves: an outcome, what the device is at boot, what it holds at rest.
 * A bar (display_progress) is the hold filling up.
 *
 * The colours are named in palette.h, and the ink is chosen per state rather
 * than assumed black -- see display_state_ink().
 *
 * None of this needs a board to look at: test/native/hostgfx stands in for
 * ESP-IDF underneath this file, so `make preview` renders every screen below
 * to a PNG at both real panel geometries, and test_card_render.c asserts
 * about the pixels. That exists because every display fault this project has
 * shipped was a layout fault found by a person squinting at hardware. */
#include "display.h"

#include <string.h>

#include "font5x7.h"
#include "palette.h"

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static esp_lcd_panel_handle_t g_panel = NULL;
static int g_width = 0;
static int g_height = 0;

/* A ring of full-width row buffers.
 *
 * esp_lcd_panel_draw_bitmap() queues a DMA transfer against the caller's
 * buffer and returns; the bytes are read later. Overwrite a buffer while its
 * transfer is still draining and the panel shows whatever you replaced it
 * with.
 *
 * There used to be two of these, ping-ponged, and two was enough only because
 * the one caller filled every row of a rectangle with the SAME colour -- an
 * in-flight transfer re-reading a "reused" row saw exactly what it expected.
 * The moment display_text() started emitting rows whose contents DIFFER, two
 * stopped being enough, and text at larger scales came out as garbage on real
 * hardware: bigger glyph, longer transfer, more overlap with the row being
 * composed behind it.
 *
 * The ring closes that without a completion callback, by leaning on a
 * guarantee the driver already gives. the board files create the panel IO with
 * trans_queue_depth = 10, so the driver accepts at most that many outstanding
 * transfers and blocks on the next one until one retires. Keep more buffers
 * than that depth and, by the time the ring comes back around to a buffer,
 * the driver has necessarily drained it. ROW_BUFFERS must therefore stay
 * greater than the largest trans_queue_depth any board configures.
 *
 * Allocated rather than static because geometry is a runtime property of the
 * board, and DMA-capable because that is what the panel bus sends from. */
#define ROW_BUFFERS 16
static uint16_t *g_rows[ROW_BUFFERS] = {NULL};
static int g_row_turn = 0;

static uint16_t *next_row(void) {
    uint16_t *r = g_rows[g_row_turn];
    g_row_turn = (g_row_turn + 1) % ROW_BUFFERS;
    return r;
}

static display_state_t g_current_state = DISPLAY_STATE_IDLE;

/* Where the card ends and the progress bar begins.
 *
 * Derived in ONE place and used by both, because two functions each working
 * out "where the bar is" from the panel height is exactly how a line came to
 * be computed, reserved for, and then drawn one pixel into a band that was
 * not its own.
 *
 * The bar used to take an eighth of the panel plus a twelfth of it again as
 * bottom margin -- 27 of the classic T-Display's 135 rows, a fifth of the
 * screen, for a progress indicator. That left 102 rows for a card that has
 * four things to say, and four readable lines plus their gaps do not fit in
 * 102 without holding the amount to the 21-pixel height a person on real
 * hardware could not read. Slimming the bar is what makes the whole card fit:
 * a 180-pixel-wide bar reads perfectly well at 8 rows.
 */
#define CARD_MARGIN 6

static int progress_bar_h(void) {
    const int h = g_height / 16;
    return h < 6 ? 6 : h;
}

static int progress_bar_top(void) {
    return g_height - CARD_MARGIN - progress_bar_h();
}

/* One past the last row a card may draw on. */
static int card_usable_h(void) {
    return progress_bar_top() - FONT5X7_CARD_GAP;
}



uint16_t display_state_color(display_state_t state) {
    switch (state) {
        case DISPLAY_STATE_IDLE:
            return PALETTE_IDLE;
        case DISPLAY_STATE_BROWSE:
            return PALETTE_BROWSE;
        case DISPLAY_STATE_CONFIRM_PENDING:
            return PALETTE_PENDING;
        case DISPLAY_STATE_APPROVED:
            return PALETTE_APPROVED;
        case DISPLAY_STATE_DECLINED:
            return PALETTE_DECLINED;
        case DISPLAY_STATE_EXPIRED:
            return PALETTE_EXPIRED;
        default:
            return PALETTE_INK_DARK;
    }
}

/* Which ink reads on that colour.
 *
 * Every card used to draw black whatever it was drawing on, which was true
 * enough while only amber and green ever carried text. It is not true of the
 * dark grey idle background or of pure red: black on #383838 is close to
 * invisible, and that is now the screen the device rests on all day and the
 * card it shows when it refuses. Picked per state rather than per call site so
 * a new screen cannot get it wrong. */
uint16_t display_state_ink(display_state_t state) {
    switch (state) {
        case DISPLAY_STATE_IDLE:
        case DISPLAY_STATE_BROWSE:
        case DISPLAY_STATE_DECLINED:
            return PALETTE_INK_LIGHT;
        default:
            return PALETTE_INK_DARK;
    }
}

void display_init(void) {
    board_display_t d = board_display_init();
    g_panel = d.panel;
    g_width = d.width;
    g_height = d.height;

    if (g_panel) {
        bool all = true;
        for (int i = 0; i < ROW_BUFFERS; i++) {
            g_rows[i] = heap_caps_malloc((size_t)g_width * sizeof(uint16_t), MALLOC_CAP_DMA);
            if (!g_rows[i]) {
                all = false;
            }
        }
        if (!all) {
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
    if (!g_panel) {
        return false;
    }
    for (int i = 0; i < ROW_BUFFERS; i++) {
        if (!g_rows[i]) {
            return false;
        }
    }
    return true;
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
    uint16_t *row = next_row();

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
    fill_screen(display_state_color(state));
}

void display_text(int x, int y, const char *text, int scale, uint16_t fg, uint16_t bg) {
    if (!display_ready() || !text || !text[0] || x < 0) {
        return;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > DISPLAY_MAX_TEXT_SCALE) {
        scale = DISPLAY_MAX_TEXT_SCALE;
    }

    const int ch = FONT5X7_HEIGHT * scale;
    if (y < 0 || y + ch > g_height) {
        return;
    }

    const int cell_w = FONT5X7_ADVANCE * scale; /* glyph plus its trailing gap */
    const int len = (int)strlen(text);
    int line_w = len * cell_w;
    if (x + line_w > g_width) {
        line_w = g_width - x; /* clip: see the header on why this does not wrap */
    }
    if (line_w <= 0) {
        return;
    }

    /* One row of the whole line at a time, each into its own buffer from the
     * ring. Composing a glyph at a time into one shared buffer is what
     * produced garbage on hardware -- see ROW_BUFFERS. */
    for (int row = 0; row < ch; row++) {
        const int gy = row / scale;
        uint16_t *dst = next_row();
        for (int px = 0; px < line_w; px++) {
            const int gx = (px % cell_w) / scale;
            uint16_t colour = bg;
            if (gx < FONT5X7_WIDTH) {
                const uint8_t *glyph = font5x7_glyph(text[px / cell_w]);
                if ((glyph[gx] >> gy) & 1) {
                    colour = fg;
                }
            }
            dst[px] = colour;
        }
        esp_lcd_panel_draw_bitmap(g_panel, x, y + row, x + line_w, y + row + 1, dst);
    }
}

void display_note_detail(display_state_t state, const char *action, const char *amount_num,
                          const char *amount_unit, const char *label, const char *id,
                          const char *hint) {
    if (!display_ready()) {
        return;
    }
    g_current_state = state;
    const uint16_t bg = display_state_color(state);
    const uint16_t ink = display_state_ink(state);
    fill_screen(bg);

    /* Every line is drawn at the largest scale that fits the panel, rather
     * than at a scale picked from the panel height. The first version did the
     * latter and produced 21-pixel digits with a 14-pixel label, which a
     * person on real hardware could not read. Fitting to the width instead
     * means a short amount gets big automatically, which is the common case.
     *
     * The lower band is left clear for display_progress(). */
    const int margin = CARD_MARGIN;
    const int avail = g_width - 2 * margin;
    /* Keep out of the progress bar's band -- see card_usable_h(). */
    const int usable_h = card_usable_h();
    const int small_h = FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE;
    /* One gap for reserving and for advancing alike -- see FONT5X7_CARD_GAP,
     * which is where the reason it must be a single constant is written down. */
    const int gap = FONT5X7_CARD_GAP;

    int y = margin;

    /* Unit and label share a line: "sats  rent". Built before anything is
     * drawn, because the amount's scale depends on how much room the lines
     * BELOW it still need -- see the reservation below. */
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

    /* What is being asked, first and at the readable minimum. Without it the
     * screen showed an amount and left the owner to infer the verb -- and the
     * verb is the difference between handing over one note's secret and
     * erasing every note on the device. Both used to look identical here. */
    if (action && action[0] && y + small_h <= usable_h) {
        display_text(margin, y, action, FONT5X7_MIN_READABLE_SCALE, ink, bg);
        y += small_h + gap;
    }

    /* The digits get the FULL width, at the largest scale the lines below
     * still leave room for. An earlier version reserved room for the unit on
     * the same line, which cost 90 of 228 pixels and held a seven-digit amount
     * down to the same 21-pixel height a person had already told us was too
     * small to read. The unit goes on the next line with the label instead: it
     * is a word, and the digits are the thing a mistake costs money on.
     *
     * The budget itself lives in font5x7_card_amount_scale(), where it can be
     * tested without a board -- including which lines are reserved for, which
     * is the part that got this wrong last time. */
    if (amount_num && amount_num[0]) {
        const int lines_below = (second[0] ? 1 : 0) + ((hint && hint[0]) ? 1 : 0);
        const int scale = font5x7_card_amount_scale(amount_num, avail, usable_h, y, lines_below);
        if (scale > 0) {
            display_text(margin, y, amount_num, scale, ink, bg);
            y += FONT5X7_HEIGHT * scale + gap;
        }
    }

    if (second[0] && y + small_h <= usable_h) {
        const int scale = font5x7_fit_scale(second, avail, FONT5X7_MIN_READABLE_SCALE + 1);
        /* Cut it to what the width actually holds. font5x7_fit_scale cannot
         * shrink below the readable minimum, so a long label otherwise runs
         * off the panel and is clipped mid-glyph -- "card-check" rendered as
         * "card-ch" with the h sliced down the middle, which reads as a
         * different label rather than as a truncated one. */
        const int fits = avail / (FONT5X7_ADVANCE * scale);
        if (fits > 0 && (int)strlen(second) > fits) {
            second[fits] = '\0';
        }
        display_text(margin, y, second, scale, ink, bg);
        y += FONT5X7_HEIGHT * scale + gap;
    }
    if (id && id[0] && y + small_h <= usable_h) {
        display_text(margin, y, id, FONT5X7_MIN_READABLE_SCALE, ink, bg);
        y += small_h + gap;
    }
    /* Pinned to the bottom of the card, against the bar it describes, rather
     * than laid out after whatever came before it. The gesture is the one line
     * that must never be the one that falls off: a card that does not say the
     * approval is a two-second HOLD leaves tapping -- and then concluding the
     * device is dead -- as the obvious thing to try, which is exactly what
     * happened. Everything above it is allowed to run out of room. This one
     * has its own row and keeps it. */
    if (hint && hint[0]) {
        const int hint_y = usable_h - small_h;
        if (hint_y >= y - gap && hint_y >= margin) {
            display_text(margin, hint_y, hint, FONT5X7_MIN_READABLE_SCALE, ink, bg);
        }
    }
}

/* Centres `text` horizontally, or starts it at the margin if it is wider than
 * the panel -- display_text clips at the edge, and a negative x it would
 * refuse to draw at all. */
static void draw_centred(int y, const char *text, int scale, uint16_t ink, uint16_t bg) {
    const int w = font5x7_text_width(text, scale);
    int x = (g_width - w) / 2;
    if (x < CARD_MARGIN) {
        x = CARD_MARGIN;
    }
    display_text(x, y, text, scale, ink, bg);
}

void display_message(display_state_t state, const char *title, const char *line1,
                     const char *line2) {
    if (!display_ready()) {
        return;
    }
    g_current_state = state;
    const uint16_t bg = display_state_color(state);
    const uint16_t ink = display_state_ink(state);
    fill_screen(bg);

    const int margin = CARD_MARGIN;
    const int avail = g_width - 2 * margin;
    const int small_h = FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE;
    /* Roomier than a note card's gap. A card is a dense list of fields packed
     * into a panel that barely holds them; a message is two or three short
     * lines with the screen to themselves, and at the card's 3px they read as
     * one solid block of pixels rather than as separate statements. */
    const int gap = small_h / 3;
    /* No progress bar on a message, so unlike a note card this one owns the
     * whole panel. */
    const int usable_h = g_height - 2 * margin;

    const int extra = ((line1 && line1[0]) ? small_h + gap : 0) +
                      ((line2 && line2[0]) ? small_h + gap : 0);

    int title_scale = 0;
    int block_h = extra > 0 ? extra - gap : 0;
    if (title && title[0]) {
        int room = usable_h - extra;
        int max_scale = room / FONT5X7_HEIGHT;
        if (max_scale > DISPLAY_MAX_TEXT_SCALE) {
            max_scale = DISPLAY_MAX_TEXT_SCALE;
        }
        title_scale = font5x7_fit_scale(title, avail, max_scale);
        block_h += FONT5X7_HEIGHT * title_scale + (extra > 0 ? gap : 0);
    }
    if (block_h <= 0) {
        return;
    }

    /* Centred vertically as a block. A message is the whole screen -- an
     * outcome, or what the device is at rest -- and hanging it off the top
     * leaves it looking like a card that failed to finish drawing. */
    int y = (g_height - block_h) / 2;
    if (y < margin) {
        y = margin;
    }

    if (title_scale > 0) {
        draw_centred(y, title, title_scale, ink, bg);
        y += FONT5X7_HEIGHT * title_scale + gap;
    }
    if (line1 && line1[0]) {
        draw_centred(y, line1, FONT5X7_MIN_READABLE_SCALE, ink, bg);
        y += small_h + gap;
    }
    if (line2 && line2[0]) {
        draw_centred(y, line2, FONT5X7_MIN_READABLE_SCALE, ink, bg);
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
    int bar_h = progress_bar_h();
    int y = progress_bar_top();
    if (track_w <= 2 || bar_h <= 2) {
        return; /* a panel too small to draw a meaningful bar on */
    }

    int filled = (int)(((int32_t)track_w * permille) / 1000);

    /* Track, then fill. Repainting the whole track each call is what makes
     * this safe to call at any rate and in any order, including going
     * backwards if a hold restarts. */
    display_fill_rect(margin, y, track_w, bar_h, PALETTE_INK_DARK);
    if (filled > 0) {
        display_fill_rect(margin, y, filled, bar_h, PALETTE_PAPER);
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
        fill_screen(PALETTE_PAPER); /* white */
        vTaskDelay(pdMS_TO_TICKS(150));
        fill_screen(display_state_color(g_current_state));
        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

esp_lcd_panel_handle_t display_panel_handle(void) {
    return g_panel;
}
