/* Host stand-in for ESP-IDF's esp_lcd_types.h -- see hostgfx.h for why these
 * shims exist at all. Only the panel handle is needed: nothing on the host
 * ever dereferences it. */
#ifndef LNURLVAULT_HOSTGFX_ESP_LCD_TYPES_H
#define LNURLVAULT_HOSTGFX_ESP_LCD_TYPES_H

typedef struct esp_lcd_panel_t *esp_lcd_panel_handle_t;

#endif
