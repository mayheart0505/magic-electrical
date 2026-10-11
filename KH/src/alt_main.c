//FOR ESP32 2 the output one

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "sd_main.h"

#include "i2s_mic.h"
#include "driver/i2s_std.h"
#include "audio_stream_format.h"
#include "freertos/semphr.h"

SemaphoreHandle_t spi_bus_mutex = NULL;


static const char *TAG = "audio_tx";

#define MIC_BITS_PER_SAMPLE   32
#define MIC_CHANNEL_COUNT     2
#define KEEP_SLOT             0      /* flip to 1 if the stream is silent or hum-only */
#define SAMPLE_SHIFT          16     /* 32 -> 16 bit; try 14 for more gain */
#define MIC_DMA_BUFFER_COUNT  6      /* driver holds up to 5 unread buffers (~150 ms) */
#define MIC_FRAMES            AUDIO_FRAME_SAMPLES   /* one DMA buffer = one network frame */
#define MIC_BUF_BYTES         (MIC_FRAMES * MIC_CHANNEL_COUNT * (MIC_BITS_PER_SAMPLE / 8))  /* 3840 */

_Static_assert(MIC_BUF_BYTES <= 4092, "one DMA buffer is capped at 4092 bytes");
_Static_assert(AUDIO_BITS_PER_SAMPLE == 16 && AUDIO_CHANNELS == 1, "conversion below assumes 16-bit mono");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "stream format is little-endian PCM");

#define AUDIO_TASK_PRIO       3
#define AUDIO_TASK_STACK      4096
#define REPORT_MS             2000

#define RECORD_SECONDS 6
#define RECORD_FRAMES (RECORD_SECONDS * 1000/AUDIO_FRAME_MS)
#define PCM_BYTES (RECORD_FRAMES * AUDIO_FRAME_BYTES)

#define REPLY_MAX 512
static char s_reply[REPLY_MAX];
static size_t s_reply_len;

#define I2S_BCLK GPIO_NUM_5   // ESP32 -> amp BCL
#define I2S_LRC  GPIO_NUM_6   // ESP32 -> amp LRC / WS
#define I2S_DOUT GPIO_NUM_7   // ESP32 data out -> amp DIN

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"

#include "eye_display.h"
#include "eye_renderer.h"
#include "shaker.h"

#define PIN_NUM_MOSI 11
#define PIN_NUM_CLK  12
#define PIN_NUM_CS   10
#define PIN_NUM_DC   9
#define PIN_NUM_RST  8

#define LCD_HOST SPI3_HOST

// ---- bring-up switches ------------------------------------------------
#define RUN_COLOR_TEST   0
#define EYE_STATIC_TEST  0
#define PANEL_RGB_ORDER  LCD_RGB_ELEMENT_ORDER_RGB
// -----------------------------------------------------------------------

#define VOLUME_DIVISOR 25
static i2s_chan_handle_t tx_handle = NULL;
static const char *LCD = "LCD";

esp_lcd_panel_handle_t g_panel_handle = NULL;   // used by eye_display.c

static void eye_task(void *arg)
{
    eyes_init();

#if EYE_STATIC_TEST
    eye_draw_static();
    ESP_LOGI(LCD, "static eye drawn");
    vTaskDelay(portMAX_DELAY);
#else
   while (1) {
    updateEye();
    vTaskDelay(pdMS_TO_TICKS(16));
}
#endif
}
static esp_err_t play_file(const char *name)
{
    char path[128];
    int path_len = snprintf(path, sizeof(path), "/sdcard/%s", name);

    if (path_len < 0 || path_len >= sizeof(path)) {
        ESP_LOGE(TAG, "File path is too long");
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d)", path, errno);
        return ESP_FAIL;
    }

    if (fseek(f, 44, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to skip WAV header: %s", path);
        fclose(f);
        return ESP_FAIL;
    }

    int16_t chunk[512];

    while (1) {
        size_t n = 0; // Declare and initialize directly inside the loop context

        // Take the mutex before touching the shared SPI SD card
        if (xSemaphoreTake(spi_bus_mutex, portMAX_DELAY) == pdTRUE) {
            n = fread(chunk, 1, sizeof(chunk), f);
            xSemaphoreGive(spi_bus_mutex); // Release immediately so the display can have the bus
        }

        if (n == 0) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "Error reading %s", path);
            }
            break;
        }

        // Scale only complete 16-bit samples
        for (size_t i = 0; i < n / sizeof(int16_t); i++) {
            chunk[i] /= VOLUME_DIVISOR;
        }

        // Send exactly the number of bytes read down the I2S channels
        size_t written = 0;
        esp_err_t err = i2s_channel_write(tx_handle, chunk, n, &written, portMAX_DELAY);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(err));
            fclose(f);
            return err;
        }

        if (written != n) {
            ESP_LOGE(TAG, "Short I2S write: %u of %u bytes", (unsigned)written, (unsigned)n);
            fclose(f);
            return ESP_FAIL;
        }
    }

    bool read_error = ferror(f);
    fclose(f);
    if (read_error) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Finished playing %s", path);
    return ESP_OK;
}

// Background Audio Task to loop playback without starving the CPU or screen
static void audio_task(void *arg)
{
    while (1) {
        play_file("fahh.WAV");
        vTaskDelay(pdMS_TO_TICKS(10000)); // Delay 10 seconds between playbacks
    }
}

static esp_err_t http_event(esp_http_client_event_t *evt){
    if (evt->event_id == HTTP_EVENT_ON_DATA){
        int space = REPLY_MAX - 1 - s_reply_len;
        int n = evt->data_len;
        if (n > space){
            n = space;
        }
        memcpy(s_reply + s_reply_len, evt->data, n);
        s_reply_len += n;
    } return ESP_OK;
}

static esp_err_t receive(const uint8_t *wav, size_t len){
    esp_http_client_config_t web_config = {
        .url = CONFIG_AUDIO_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&web_config);
    if (client == NULL){
        return ESP_FAIL;
    };

    s_reply_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);    
        if (status == 200){
            s_reply[s_reply_len] = '\0';
            ESP_LOGI(TAG, "heard: %s", s_reply);
        }
    } else {
        ESP_LOGE(TAG, "failed to get text because: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

static void speaker_up(void){
    ESP_LOGI(TAG, "Starting I2S audio");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);

    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = false,
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCLK,
            .ws   = I2S_LRC,
            .dout = I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
}



void app_main(void)
{
      // Create the mutex before doing any hardware setup
    spi_bus_mutex = xSemaphoreCreateMutex();
    if (spi_bus_mutex == NULL) {
        ESP_LOGE("MAIN", "Failed to create SPI Bus Mutex!");
        return;
    }
    // 1. Initialize SD Card (Brings up SPI2_HOST first)
    sd_up();
    
    // 2. Initialize Speaker
    speaker_up();
    
    ESP_LOGI(LCD, "Starting LCD setup");

    // 3. Define SPI configuration for the shared bus
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EYE_PANEL_W * 20 * sizeof(uint16_t)
    };
    
    // 4. Safely cross-check if the SPI bus is already open
    esp_err_t bus_err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (bus_err == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(LCD, "SPI bus already initialized by SD card. Sharing the bus path...");
    } else {
        ESP_ERROR_CHECK(bus_err);
    }

    // 5. Setup the Panel IO Configuration
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
    
    // Attach the LCD device onto the shared SPI host
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle));

    // 6. Setup GC9A01 Display Driver Configurations
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
    ESP_LOGI(LCD, "GC9A01 initialized successfully");

    g_panel_handle = panel_handle;
    eye_display_init(io_handle);   // Handles internal renderer done-semaphores

#if RUN_COLOR_TEST
    eye_color_test();
    ESP_LOGI(LCD, "color test drawn");
    return;
#endif

    eye_display_clear();           // Black out border areas surrounding the eye asset

    // 7. Multi-threaded Worker Initialization
    // Pin the animation rendering loop to Core 1
    xTaskCreatePinnedToCore(eye_task, "eye_render", 8192, NULL, 5, NULL, 1);
    
    // Pin the audio disk streaming playback task to Core 0
    xTaskCreatePinnedToCore(audio_task, "audio_playback", 4096, NULL, 5, NULL, 0);

    shaker_start();                // Start reading shake logic signals
    
    while(1) {
vTaskDelay(portMAX_DELAY); // Keeps main safe without stealing cycles
}
}
