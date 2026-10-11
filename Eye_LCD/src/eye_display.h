#pragma once
#include <stdint.h>
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define EYE_PANEL_W 240
#define EYE_PANEL_H 240

extern esp_lcd_panel_handle_t g_panel_handle;   // defined in main.c

void eye_display_init(esp_lcd_panel_io_handle_t io);
void lcd_push_rect(int x, int y, int w, int h, const uint16_t *pixels); // blocking
void eye_display_clear(void);        // fill whole panel black
void eye_color_test(void);           // R/G/B stripes, checks byte order
