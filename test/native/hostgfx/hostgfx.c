/* See hostgfx.h: the four framework functions display.c calls, against a
 * framebuffer, plus a PNG writer. */
#include "hostgfx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"

/* Forward-declared in the shim header; display.c only ever compares the
 * handle to NULL. */
struct esp_lcd_panel_t {
    int unused;
};

static struct esp_lcd_panel_t g_fake_panel;
static uint16_t g_fb[HOSTGFX_MAX_H][HOSTGFX_MAX_W];
static uint16_t g_snap[HOSTGFX_MAX_H][HOSTGFX_MAX_W];
static int g_w;
static int g_h;
static long g_offscreen;

/* The board's backlight line, as display.c last left it. A real board
 * powers up lit; so does this. */
static bool g_backlight = true;

/* Row buffers come from here, not malloc -- see esp_heap_caps.h. */
static unsigned char g_pool[64 * 1024];
static size_t g_pool_used;

void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    const size_t aligned = (size + 7u) & ~(size_t)7u;
    if (aligned > sizeof(g_pool) - g_pool_used) {
        return NULL; /* display_ready() goes false, and the test says so */
    }
    void *p = g_pool + g_pool_used;
    g_pool_used += aligned;
    return p;
}

void hostgfx_reset(int w, int h) {
    if (w < 1) {
        w = 1;
    }
    if (h < 1) {
        h = 1;
    }
    if (w > HOSTGFX_MAX_W) {
        w = HOSTGFX_MAX_W;
    }
    if (h > HOSTGFX_MAX_H) {
        h = HOSTGFX_MAX_H;
    }
    g_w = w;
    g_h = h;
    g_offscreen = 0;
    g_backlight = true;
    for (int y = 0; y < HOSTGFX_MAX_H; y++) {
        for (int x = 0; x < HOSTGFX_MAX_W; x++) {
            g_fb[y][x] = HOSTGFX_UNPAINTED;
        }
    }
    memcpy(g_snap, g_fb, sizeof(g_snap));
    g_pool_used = 0;
}

int hostgfx_width(void) {
    return g_w;
}

int hostgfx_height(void) {
    return g_h;
}

uint16_t hostgfx_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_w || y >= g_h) {
        return 0;
    }
    return g_fb[y][x];
}

long hostgfx_offscreen_pixels(void) {
    return g_offscreen;
}

/* --- the ESP-IDF surface display.c draws through --------------------------- */

board_display_t board_display_init(void) {
    board_display_t out = {.panel = &g_fake_panel, .width = g_w, .height = g_h};
    return out;
}

void board_display_backlight(bool on) {
    g_backlight = on;
}

bool hostgfx_backlight(void) {
    return g_backlight;
}

int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end,
                              int y_end, const void *color_data) {
    (void)panel;
    const uint16_t *src = (const uint16_t *)color_data;
    if (!src || x_end <= x_start || y_end <= y_start) {
        return 0;
    }
    const int span = x_end - x_start;
    for (int y = y_start; y < y_end; y++) {
        for (int x = x_start; x < x_end; x++) {
            if (x < 0 || y < 0 || x >= g_w || y >= g_h) {
                /* A real panel swallows this silently; counted so a test can
                 * insist it never happens. */
                g_offscreen++;
                continue;
            }
            g_fb[y][x] = src[(size_t)(y - y_start) * (size_t)span + (size_t)(x - x_start)];
        }
    }
    return 0;
}

int esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes) {
    (void)panel;
    (void)swap_axes;
    return 0;
}

int esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y) {
    (void)panel;
    (void)mirror_x;
    (void)mirror_y;
    return 0;
}

/* --- inspection ------------------------------------------------------------ */

long hostgfx_ink_pixels(uint16_t ink) {
    long n = 0;
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            if (g_fb[y][x] == ink) {
                n++;
            }
        }
    }
    return n;
}

static bool row_has_ink(int y, uint16_t ink) {
    for (int x = 0; x < g_w; x++) {
        if (g_fb[y][x] == ink) {
            return true;
        }
    }
    return false;
}

int hostgfx_first_ink_row(uint16_t ink) {
    for (int y = 0; y < g_h; y++) {
        if (row_has_ink(y, ink)) {
            return y;
        }
    }
    return -1;
}

int hostgfx_last_ink_row(uint16_t ink) {
    for (int y = g_h - 1; y >= 0; y--) {
        if (row_has_ink(y, ink)) {
            return y;
        }
    }
    return -1;
}

int hostgfx_first_ink_col(uint16_t ink) {
    for (int x = 0; x < g_w; x++) {
        for (int y = 0; y < g_h; y++) {
            if (g_fb[y][x] == ink) {
                return x;
            }
        }
    }
    return -1;
}

int hostgfx_last_ink_col(uint16_t ink) {
    for (int x = g_w - 1; x >= 0; x--) {
        for (int y = 0; y < g_h; y++) {
            if (g_fb[y][x] == ink) {
                return x;
            }
        }
    }
    return -1;
}

void hostgfx_snapshot(void) {
    memcpy(g_snap, g_fb, sizeof(g_fb));
}

int hostgfx_first_changed_row(void) {
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            if (g_fb[y][x] != g_snap[y][x]) {
                return y;
            }
        }
    }
    return -1;
}

/* --- PNG ------------------------------------------------------------------- */

static uint32_t crc_update(uint32_t crc, const uint8_t *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc;
}

static void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char type[4], const uint8_t *data, size_t len) {
    uint8_t head[4];
    be32(head, (uint32_t)len);
    if (fwrite(head, 1, 4, f) != 4 || fwrite(type, 1, 4, f) != 4) {
        return -1;
    }
    if (len && fwrite(data, 1, len, f) != len) {
        return -1;
    }
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t tail[4];
    be32(tail, crc);
    return fwrite(tail, 1, 4, f) == 4 ? 0 : -1;
}

/* 5-6-5 to 8-8-8, replicating high bits low so 0x1F is 0xFF, not 0xF8. */
static void rgb565_to_888(uint16_t c, uint8_t out[3]) {
    const uint8_t r = (uint8_t)((c >> 11) & 0x1Fu);
    const uint8_t g = (uint8_t)((c >> 5) & 0x3Fu);
    const uint8_t b = (uint8_t)(c & 0x1Fu);
    out[0] = (uint8_t)((r << 3) | (r >> 2));
    out[1] = (uint8_t)((g << 2) | (g >> 4));
    out[2] = (uint8_t)((b << 3) | (b >> 6));
}

int hostgfx_write_png(const char *path, int zoom) {
    if (!path || g_w < 1 || g_h < 1) {
        return -1;
    }
    if (zoom < 1) {
        zoom = 1;
    }

    const size_t ow = (size_t)g_w * (size_t)zoom;
    const size_t oh = (size_t)g_h * (size_t)zoom;
    const size_t stride = 1 + ow * 3; /* one filter byte per scanline, filter 0 */
    const size_t raw_len = stride * oh;

    uint8_t *raw = malloc(raw_len);
    if (!raw) {
        return -1;
    }
    for (size_t oy = 0; oy < oh; oy++) {
        uint8_t *row = raw + oy * stride;
        row[0] = 0; /* filter: none */
        for (size_t ox = 0; ox < ow; ox++) {
            rgb565_to_888(g_fb[oy / (size_t)zoom][ox / (size_t)zoom], row + 1 + ox * 3);
        }
    }

    /* Stored (uncompressed) deflate blocks: a legal zlib stream, and no
     * dependency. */
    const size_t blocks = (raw_len + 65534u) / 65535u;
    const size_t z_len = 2 + blocks * 5 + raw_len + 4;
    uint8_t *z = malloc(z_len);
    if (!z) {
        free(raw);
        return -1;
    }
    size_t zp = 0;
    z[zp++] = 0x78; /* CMF: deflate, 32K window */
    z[zp++] = 0x01; /* FLG: no dict, fastest -- (0x78<<8|0x01) % 31 == 0 */
    size_t done = 0;
    for (size_t b = 0; b < blocks; b++) {
        const size_t n = (raw_len - done) > 65535u ? 65535u : (raw_len - done);
        z[zp++] = (b + 1 == blocks) ? 1 : 0; /* BFINAL, BTYPE=00 (stored) */
        z[zp++] = (uint8_t)(n & 0xFFu);
        z[zp++] = (uint8_t)(n >> 8);
        z[zp++] = (uint8_t)(~n & 0xFFu);
        z[zp++] = (uint8_t)((~n >> 8) & 0xFFu);
        memcpy(z + zp, raw + done, n);
        zp += n;
        done += n;
    }
    uint32_t a = 1;
    uint32_t bsum = 0;
    for (size_t i = 0; i < raw_len; i++) {
        a = (a + raw[i]) % 65521u;
        bsum = (bsum + a) % 65521u;
    }
    be32(z + zp, (bsum << 16) | a);
    zp += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(z);
        return -1;
    }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    int rc = fwrite(sig, 1, 8, f) == 8 ? 0 : -1;

    uint8_t ihdr[13];
    be32(ihdr, (uint32_t)ow);
    be32(ihdr + 4, (uint32_t)oh);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 2;  /* colour type: truecolour */
    ihdr[10] = 0; /* compression: deflate */
    ihdr[11] = 0; /* filter method 0 */
    ihdr[12] = 0; /* no interlace */
    if (rc == 0) {
        rc = write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    }
    if (rc == 0) {
        rc = write_chunk(f, "IDAT", z, zp);
    }
    if (rc == 0) {
        rc = write_chunk(f, "IEND", NULL, 0);
    }
    free(z);
    if (fclose(f) != 0) {
        rc = -1;
    }
    return rc;
}
