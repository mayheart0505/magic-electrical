#include <stdio.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"

#include "eye_display.h"
#include "eye_renderer.h"

#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_DC   9
#define PIN_NUM_RST  8

#define LCD_HOST SPI2_HOST

// ---- bring-up switches ------------------------------------------------
// 1 = draw R/G/B stripes and stop (checks byte order / channel order)
#define RUN_COLOR_TEST   0
// 1 = draw one still eye and stop; 0 = full animation
#define EYE_STATIC_TEST  0
// If red/blue are swapped in the color test, change to LCD_RGB_ELEMENT_ORDER_BGR
#define PANEL_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
// -----------------------------------------------------------------------

static const char *TAG = "LCD";

esp_lcd_panel_handle_t g_panel_handle = NULL;   // used by eye_display.c

static void eye_task(void *arg)
{
    eyes_init();

#if EYE_STATIC_TEST
    eye_draw_static();
    ESP_LOGI(TAG, "static eye drawn");
    vTaskDelay(portMAX_DELAY);
#else
    while (1) {
        updateEye();
    }
#endif
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting LCD setup");

    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EYE_PANEL_W * 20 * sizeof(uint16_t)
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_NUM_DC,
        .cs_gpio_num = PIN_NUM_CS,
        .pclk_hz = 20 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = PANEL_RGB_ORDER,
        .bits_per_pixel = 16
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
    ESP_LOGI(TAG, "GC9A01 initialized successfully");

    g_panel_handle = panel_handle;
    eye_display_init(io_handle);   // registers the done-semaphore callback

#if RUN_COLOR_TEST
    eye_color_test();
    ESP_LOGI(TAG, "color test drawn");
    return;
#endif

    eye_display_clear();           // black border around the 128x128 eye
    xTaskCreate(eye_task, "eye", 8192, NULL, 5, NULL);

      // TEMPORARY TEST: 10 s blinking, 10 s  no blinking, repeat
      //call EYE_MODE_STANDBY if you want blinking and EYE_MODE_ANSWERING if you don't want blinking
    while (1) {
        eye_set_mode(EYE_MODE_STANDBY);
        ESP_LOGI(TAG, "mode: STANDBY");
        vTaskDelay(pdMS_TO_TICKS(10000));

        eye_set_mode(EYE_MODE_ANSWERING);
        ESP_LOGI(TAG, "mode: ANSWERING");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
