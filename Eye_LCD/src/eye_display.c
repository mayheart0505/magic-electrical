// Replaces TFT_eSPI's startWrite/setAddrWindow/pushPixels/endWrite.
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "eye_display.h"

static SemaphoreHandle_t s_done;

static bool eye_color_done_cb(esp_lcd_panel_io_handle_t io,
                              esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

void eye_display_init(esp_lcd_panel_io_handle_t io)
{
    s_done = xSemaphoreCreateBinary();
    esp_lcd_panel_io_callbacks_t cbs = { .on_color_trans_done = eye_color_done_cb };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &cbs, NULL));
}

// draw_bitmap's end coordinates are exclusive. Returns only after the
// pixels have been sent, so  caller may reuse the buffer immediately
void lcd_push_rect(int x, int y, int w, int h, const uint16_t *pixels)
{
    esp_lcd_panel_draw_bitmap(g_panel_handle, x, y, x + w, y + h, pixels);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static uint16_t s_band[EYE_PANEL_W * 8];   // 8 rows, internal RAM (DMA-capable)

static void fill_bands(uint16_t value, int y0, int y1)
{
    for (int i = 0; i < EYE_PANEL_W * 8; i++) s_band[i] = value;
    for (int y = y0; y < y1; y += 8)
        lcd_push_rect(0, y, EYE_PANEL_W, 8, s_band);
}

void eye_display_clear(void)
{
    fill_bands(0x0000, 0, EYE_PANEL_H);
}

#define SWAP16(x) ((uint16_t)(((x) >> 8) | ((x) << 8)))

// Same byte swap the eye renderer applies. Expect: red top, green middle, blue bottom.
void eye_color_test(void)
{
    fill_bands(SWAP16(0xF800), 0,   80);
    fill_bands(SWAP16(0x07E0), 80,  160);
    fill_bands(SWAP16(0x001F), 160, 240);
}
