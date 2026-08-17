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
        if (!g_rows[0] || !g_rows[1]) {
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
    return g_panel != NULL && g_rows[0] != NULL && g_rows[1] != NULL;
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

void display_progress(uint16_t permille) {
    if (!display_ready()) {
        return;
    }
    if (permille > 1000) {
        permille = 1000;
    }

    /* A band across the middle, inset from the edges so it reads as a bar
     * rather than as the screen changing colour. Geometry is derived from the
     * panel at runtime -- see display_width()'s comment on why nothing here
     * hardcodes a size. */
    int margin = g_width / 8;
    int track_w = g_width - 2 * margin;
    int bar_h = g_height / 6;
    int y = (g_height - bar_h) / 2;
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
