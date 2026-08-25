/* Drawing only. Panel bring-up -- bus type, pins, rotation, colour inversion,
 * controller-RAM offset -- belongs to src/board/, so nothing here knows or
 * cares which board it is running on or how the glass is wired.
 *
 * Three kinds of screen: a note card (dense fields, display_note_detail), a
 * message (two or three centred lines, display_message), and the hold bar.
 * Colours are named in palette.h and the ink is per state, not always black.
 *
 * test/native/hostgfx stands in for ESP-IDF underneath this file, so
 * `make preview` renders every screen to a PNG without a board and
 * test_card_render.c asserts about the pixels. */
#include "display.h"

#include <stdbool.h>

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

/* Whether the light is off. Not a display_state_t: every state is something
 * the screen is SAYING, and asleep is the screen saying nothing. Folding it
 * into that enum would have given every switch over it a case that is not a
 * card. */
static bool g_asleep = false;

/* Where the card ends and the bar begins, in one place: two functions each
 * deriving it from the panel height is how a line got drawn one pixel into a
 * band that wasn't its own. The bar was h/8 plus h/12 of margin -- a fifth of
 * the panel -- which left 102 rows, and four readable lines don't fit in 102
 * without holding the amount to an unreadable 21px. */
#define CARD_MARGIN 6

/* Taller and full-bleed since the redesign. Inset and thin, it read as a
 * detail floating on the card; edge to edge along the bottom it is the card's
 * base, and the hold fills the whole width of the screen. */
static int progress_bar_h(void) {
    const int h = g_height / 12;
    return h < 8 ? 8 : h;
}

static int progress_bar_top(void) {
    return g_height - progress_bar_h();
}

/* One past the last row a card may draw on. */
static int card_usable_h(void) {
    return progress_bar_top() - FONT5X7_CARD_GAP;
}



/* Which of the two kinds of screen this is -- see palette.h.
 *
 * Cards are read up close and carry detail, so they get a dark ground and the
 * state colour as structure. Fields are read at a glance from across a room
 * and carry no detail at all, so they get the whole panel in colour. The
 * split is by what the screen is FOR, not by which looked nicer: an outcome
 * has one job, and a field of colour does that job better than any amount of
 * layout. */
static bool state_is_field(display_state_t state) {
    return state == DISPLAY_STATE_APPROVED || state == DISPLAY_STATE_DECLINED ||
           state == DISPLAY_STATE_EXPIRED;
}

uint16_t display_state_accent(display_state_t state) {
    switch (state) {
        case DISPLAY_STATE_IDLE:
            return PALETTE_ACCENT_IDLE;
        case DISPLAY_STATE_BROWSE:
            return PALETTE_ACCENT_BROWSE;
        case DISPLAY_STATE_CONFIRM_PENDING:
            return PALETTE_ACCENT_PENDING;
        case DISPLAY_STATE_APPROVED:
            return PALETTE_ACCENT_APPROVED;
        case DISPLAY_STATE_DECLINED:
            return PALETTE_ACCENT_DECLINED;
        case DISPLAY_STATE_EXPIRED:
            return PALETTE_ACCENT_EXPIRED;
        default:
            return PALETTE_ACCENT_IDLE;
    }
}

uint16_t display_state_color(display_state_t state) {
    return state_is_field(state) ? display_state_accent(state) : PALETTE_GROUND;
}

/* One ink for every card, because the ground is now the same on all of them;
 * the warm dark for every field, because it is the ground those colours were
 * chosen against. The old per-state table existed to keep text legible on six
 * different backgrounds, and there are no longer six. */
uint16_t display_state_ink(display_state_t state) {
    return state_is_field(state) ? PALETTE_GROUND : PALETTE_INK;
}

/* The second weight. Context rather than content: the unit, the label, the
 * id, the gesture line. On a field there is no such thing -- an outcome is
 * all content -- so it collapses back to the same ink. */
uint16_t display_state_ink_dim(display_state_t state) {
    return state_is_field(state) ? PALETTE_GROUND : PALETTE_INK_DIM;
}

/* The band at the top of a card, and the height it occupies. Tall enough to
 * carry the verb at the readable minimum with air around it: this is the one
 * element that has to work both up close (as a label) and across a room (as a
 * colour). */
static int band_h(void) {
    const int text = FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE;
    const int pad = CARD_MARGIN - 2 < 3 ? 3 : CARD_MARGIN - 2;
    int h = text + 2 * pad;
    if (h > g_height / 3) {
        h = g_height / 3;
    }
    return h;
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

    g_asleep = false;
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

static display_confirm_side_t g_confirm_side = DISPLAY_CONFIRM_SIDE_UNKNOWN;

void display_set_confirm_side(display_confirm_side_t side) {
    g_confirm_side = side;
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

int display_band_height(void) {
    return band_h();
}

int display_bar_height(void) {
    return progress_bar_h();
}

void display_sleep(void) {
    if (g_asleep || !display_ready()) {
        return;
    }
    g_asleep = true;
    /* Light out first, so the blank is never seen happening -- the screen
     * goes dark, rather than visibly wiping itself and then going dark. */
    board_display_backlight(false);
    /* And then actually clear it. Killing the backlight alone would leave the
     * card still held in the crystal, which is the half of this that causes
     * the ghosting; it would also leave a bearer secret on the glass if this
     * ever blanked a QR, which one day it will. */
    fill_screen(PALETTE_INK_DARK);
}

void display_wake(void) {
    if (!g_asleep || !display_ready()) {
        return;
    }
    g_asleep = false;
    board_display_backlight(true);
}

bool display_asleep(void) {
    return g_asleep;
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
/* The two buttons, drawn where they physically are, with the one that
 * approves filled in.
 *
 * Naming the button was not enough and neither was naming the side. The
 * buttons carry no labels a person can see, so "BTN1" helps only someone who
 * already knows, and the natural reach is for the left -- which on the classic
 * board is cancel. That cost two bench runs of section 17 with a correct hint
 * on screen the whole time. A picture of two buttons with one filled needs no
 * reading and no prior knowledge; it is the same shape as the thing in the
 * owner's hands.
 *
 * No sprite sheet for this. It is two squares, and display_fill_rect already
 * draws squares -- a blitter and a generated bitmap would be more machinery
 * than the drawing deserves.
 *
 * Whether there is room for it is draw_gesture_row's call, not this one's.
 */
static void draw_button_guide(int row_y, int size, uint16_t ink, uint16_t bg) {
    const int right_x = g_width - CARD_MARGIN - size;
    const int left_x = CARD_MARGIN;
    const bool confirm_right = g_confirm_side == DISPLAY_CONFIRM_SIDE_RIGHT;

    /* Filled = the one to hold. Outlined = the one that refuses. */
    display_fill_rect(left_x, row_y, size, size, ink);
    display_fill_rect(right_x, row_y, size, size, ink);
    const int inset = size >= 6 ? 2 : 1;
    const int hollow_x = confirm_right ? left_x : right_x;
    display_fill_rect(hollow_x + inset, row_y + inset, size - 2 * inset, size - 2 * inset, bg);
}

/* The gesture row: the two buttons, and the words between them.
 *
 * Centred in the span the glyphs leave rather than laid out from the left,
 * because the row is symmetric and a line hugging one glyph reads as belonging
 * to that button. Falls back to the plain left-aligned line when there is no
 * guide to sit between, or when the words do not fit between the glyphs --
 * clipping the gesture is the one thing this row must never do. */
static void draw_gesture_row(int row_y, int size, const char *hint, uint16_t ink, uint16_t bg) {
    const int text_w = font5x7_text_width(hint, FONT5X7_MIN_READABLE_SCALE);
    const int span_l = CARD_MARGIN + size;
    const int span_r = g_width - CARD_MARGIN - size;

    /* Decided before anything is drawn, not after. Drawing the guide first and
     * then discovering the words do not fit leaves them overlapping, which is
     * how LET GO FIRST came to sit on top of both glyphs.
     *
     * When they do not fit, the words win and the guide is dropped. LET GO
     * FIRST is the longest of them and also the most urgent: it is shown
     * precisely when a button is already down, when saying which button to
     * reach for is redundant and saying to release is not. */
    const bool guide = g_confirm_side != DISPLAY_CONFIRM_SIDE_UNKNOWN &&
                       span_r > span_l && text_w <= span_r - span_l;
    if (!guide) {
        display_text(CARD_MARGIN, row_y, hint, FONT5X7_MIN_READABLE_SCALE, ink, bg);
        return;
    }

    draw_button_guide(row_y, size, ink, bg);
    display_text(span_l + (span_r - span_l - text_w) / 2, row_y, hint,
                 FONT5X7_MIN_READABLE_SCALE, ink, bg);
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
    const uint16_t dim = display_state_ink_dim(state);
    const uint16_t accent = display_state_accent(state);
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

    /* The band, first and always. The verb is the difference between handing
     * over one note's secret and erasing every note, which used to look
     * identical here; it now says which twice, in words and in a colour
     * readable from further away than the words are.
     *
     * Drawn even with nothing to put in it. A card without its band reads as
     * one that failed to finish drawing, and browse -- which has no verb --
     * needs the spine as much as a prompt does. */
    {
        const int bh = band_h();
        display_fill_rect(0, 0, g_width, bh, accent);
        if (action && action[0]) {
            /* Clipped rather than shrunk if it is too long: this is the one
             * line whose size must not vary, because a smaller verb on a
             * wider verb's card is exactly how WIPE ALL comes to look like
             * an ordinary prompt. See font5x7.h.
             *
             * The ground is the ink here -- the same warm dark the card sits
             * on. One value doing both jobs is what stops the band reading as
             * a separate sticker on top of the card. */
            const int th = FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE;
            display_text(margin, (bh - th) / 2, action, FONT5X7_MIN_READABLE_SCALE, bg, accent);
        }
        y = bh + gap;
    }

    /* Full width for the digits. Reserving room for the unit on the same line
     * cost 90 of 228 pixels and held a seven-digit amount to an unreadable
     * 21px; the unit is a word, the digits are what a mistake costs money on.
     * The budget is in font5x7_card_amount_scale(), where it is testable. */
    if (amount_num && amount_num[0]) {
        /* The id counts. It was left out, so on a card whose amount happened
         * to take a large scale the id line simply did not fit and was
         * dropped -- silently, and on the one screen whose job is to say
         * WHICH bearer note the next gesture discloses. The band shrinking
         * the content area is what surfaced it; it was always wrong. */
        const int lines_below = (second[0] ? 1 : 0) + ((id && id[0]) ? 1 : 0) +
                                ((hint && hint[0]) ? 1 : 0);
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
        display_text(margin, y, second, scale, dim, bg);
        y += FONT5X7_HEIGHT * scale + gap;
    }
    if (id && id[0] && y + small_h <= usable_h) {
        display_text(margin, y, id, FONT5X7_MIN_READABLE_SCALE, dim, bg);
        y += small_h + gap;
    }
    /* Pinned to the bottom rather than laid out after what precedes it: the
     * gesture is the one line that must never be the one that falls off.
     * Everything above may run out of room; this keeps its row. */
    if (hint && hint[0]) {
        const int hint_y = usable_h - small_h;
        if (hint_y >= y - gap && hint_y >= margin) {
            draw_gesture_row(hint_y, small_h, hint, ink, bg);
        }
    }
}


/* Centred, or at the margin if wider than the panel: display_text refuses a
 * negative x outright, so an over-long line would vanish rather than clip. */
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
    const uint16_t dim = display_state_ink_dim(state);
    fill_screen(bg);

    const int margin = CARD_MARGIN;
    const int avail = g_width - 2 * margin;
    const int small_h = FONT5X7_HEIGHT * FONT5X7_MIN_READABLE_SCALE;
    /* Roomier than a card's 3px: two or three lines with the screen to
     * themselves read as one block at that spacing. */
    const int gap = small_h / 3;

    /* A card gets a rule; a field does not.
     *
     * On a field the state colour is already the whole panel, so a rule in it
     * would be invisible and a rule in anything else would be decoration. On
     * a card -- which here means the resting screen, the one a person looks
     * at for hours -- it is the only thing that says which device this is
     * rather than a dark rectangle with two words on it. */
    int top = margin;
    if (!state_is_field(state)) {
        int rule = g_height / 20;
        if (rule < 4) {
            rule = 4;
        }
        display_fill_rect(0, 0, g_width, rule, display_state_accent(state));
        top = rule + margin;
    }
    /* No bar on a message, so this owns everything below the rule. */
    const int usable_h = g_height - top - margin;

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

    /* Centred as a block in what the rule left: hung off the top it reads as
     * a card that failed to finish drawing. */
    int y = top + (usable_h - block_h) / 2;
    if (y < top) {
        y = top;
    }

    if (title_scale > 0) {
        draw_centred(y, title, title_scale, ink, bg);
        y += FONT5X7_HEIGHT * title_scale + gap;
    }
    /* The lines under a title are context -- "TAP TO VIEW", the verb an
     * outcome was about, the board name at boot -- so they take the second
     * weight. On a field there is no second weight and this is the same ink;
     * an outcome is all content. */
    if (line1 && line1[0]) {
        draw_centred(y, line1, FONT5X7_MIN_READABLE_SCALE, dim, bg);
        y += small_h + gap;
    }
    if (line2 && line2[0]) {
        draw_centred(y, line2, FONT5X7_MIN_READABLE_SCALE, dim, bg);
    }
}

void display_progress(uint16_t permille) {
    if (!display_ready()) {
        return;
    }
    if (permille > 1000) {
        permille = 1000;
    }

    /* Edge to edge along the very bottom, in the state's own colour, so the
     * hold fills the full width of the screen and the bar is the base of the
     * card rather than a detail floating on it. Below everything
     * display_note_detail() draws -- the whole point of the hold is that the
     * owner can read what they are approving while they hold it.
     *
     * White on black was the loudest possible pairing for the one element
     * that is already moving, and it belonged to no state: an approval bar
     * and a wipe bar were identical. */
    const int track_w = g_width;
    const int bar_h = progress_bar_h();
    const int y = progress_bar_top();
    if (track_w <= 2 || bar_h <= 2) {
        return; /* a panel too small to draw a meaningful bar on */
    }

    const int filled = (int)(((int32_t)track_w * permille) / 1000);

    /* Track, then fill. Repainting the whole track each call is what makes
     * this safe to call at any rate and in any order, including going
     * backwards if a hold restarts. */
    display_fill_rect(0, y, track_w, bar_h, PALETTE_TRACK);
    if (filled > 0) {
        display_fill_rect(0, y, filled, bar_h, display_state_accent(g_current_state));
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
