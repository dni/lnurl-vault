/* Host stand-in, implemented in hostgfx.c against a framebuffer so display.c
 * compiles and runs unmodified. */
#ifndef LNURLVAULT_HOSTGFX_ESP_LCD_PANEL_OPS_H
#define LNURLVAULT_HOSTGFX_ESP_LCD_PANEL_OPS_H

#include <stdbool.h>

#include "esp_lcd_types.h"

/* IDF returns esp_err_t; 0 is ESP_OK and display.c ignores the value. */
int esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end,
                              int y_end, const void *color_data);
int esp_lcd_panel_swap_xy(esp_lcd_panel_handle_t panel, bool swap_axes);
int esp_lcd_panel_mirror(esp_lcd_panel_handle_t panel, bool mirror_x, bool mirror_y);

#endif
