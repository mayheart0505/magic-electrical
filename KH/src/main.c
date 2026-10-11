#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "sd_main.h"
#include "freertos/task.h"
#include "freertos/semphr.h"       // For semaphore

#include "i2s_mic.h"
#include "driver/i2s_std.h"
#include "audio_stream_format.h"

static const char *TAG = "audio_tx";
static SemaphoreHandle_t ShakeSem;

/* ---- Capture configuration ---------------------------------------------
 *
 * INMP441 delivers 24-bit samples MSB-justified in a 32-bit slot. Stereo is
 * captured and one slot kept, the same workaround the other i2s_mic examples
 * use for mono RX misbehaving on some ESP32-C3 / IDF combinations.
 */
#define MIC_BITS_PER_SAMPLE   32
#define MIC_CHANNEL_COUNT     2
#define KEEP_SLOT             0      /* flip to 1 if the stream is silent or hum-only */
#define SAMPLE_SHIFT          16     /* 32 -> 16 bit; try 14 for more gain */
#define MIC_DMA_BUFFER_COUNT  6      /* driver holds up to 5 unread buffers (~150 ms) */
#define MIC_FRAMES            AUDIO_FRAME_SAMPLES   /* one DMA buffer = one network frame */
#define MIC_BUF_BYTES         (MIC_FRAMES * MIC_CHANNEL_COUNT * (MIC_BITS_PER_SAMPLE / 8))  /* 3840 */

_Static_assert(MIC_BUF_BYTES <= 4092, "one DMA buffer is capped at 4092 bytes");
// _Static_assert(AUDIO_FRAME_BYTES <= UDP_LINK_MAX_PAYLOAD_LIMIT, "frame must fit in one datagram");
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
#define SHAKER_PIN GPIO_NUM_10

// Start quiet: 10% PCM amplitude
#define VOLUME_DIVISOR 10

static const char *SPEAKTAG = "MERRY_XMAS";
static i2s_chan_handle_t tx_handle = NULL;

/* ---- State ------------------------------------------------------------- */

// static udp_link_handle_t s_link;

static int32_t s_mic_buf[MIC_FRAMES * MIC_CHANNEL_COUNT];
// static int16_t s_frame[AUDIO_FRAME_SAMPLES];

/* written by the audio task, read by the reporter */
static _Atomic uint32_t s_sent;          /* frames handed to udp_link */
static _Atomic uint32_t s_dropped;       /* udp_link_send: no buffer / no route */
static _Atomic uint32_t s_send_errors;   /* udp_link_send: anything else */


/* ------------------------------------------------------------------------
 * Setup
 * ---------------------------------------------------------------------- */

static void network_up(void)
{
    esp_err_t err = nvs_flash_init();           /* Wi-Fi stores calibration data in NVS */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* protocol_examples_common: connects with the SSID/password from
     * menuconfig ("Example Connection Configuration") and waits for an IP. */
    ESP_ERROR_CHECK(example_connect());

    /* Modem power save delays and batches packets; a steady 33 packets/s
     * audio stream wants the radio awake. */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {                        /* e.g. refused under BT coexistence */
        ESP_LOGW(TAG, "Wi-Fi power save not disabled: %s", esp_err_to_name(err));
    }
}


static void mic_up(void)
{
    const i2s_mic_config_t cfg = {
        .sample_rate = AUDIO_SAMPLE_RATE,
        .bits_per_sample = MIC_BITS_PER_SAMPLE,
        .channel_count = MIC_CHANNEL_COUNT,
        .slot_mode = I2S_SLOT_MODE_STEREO,
        .gpio_bck = CONFIG_AUDIO_TX_MIC_GPIO_BCK,
        .gpio_ws = CONFIG_AUDIO_TX_MIC_GPIO_WS,
        .gpio_data = CONFIG_AUDIO_TX_MIC_GPIO_DATA,
        .port = I2S_NUM_0,
        .dma_buffer_count = MIC_DMA_BUFFER_COUNT,
        .dma_buffer_size = MIC_BUF_BYTES,
    };
    ESP_ERROR_CHECK(i2s_mic_init(&cfg));
    ESP_ERROR_CHECK(i2s_mic_start());
}

static esp_err_t http_event(esp_http_client_event_t *evt){
    if (evt->event_id == HTTP_EVENT_ON_DATA){
        int space = REPLY_MAX - 1 - s_reply_len;
        int n = evt->data_len;
        if (n > space){
            n = space;
        }
        memcpy(s_reply + s_reply_len,evt->data,n);
        s_reply_len += n;
    } return ESP_OK;
}

static void put16(uint8_t *p, uint16_t value){
    p[0] = value; //gets bottom 8 bits of value
    p[1] = (value >> 8); //gets top 8 bits of value
}

static void put32(uint8_t *p, uint32_t value){
    p[0] = value; 
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void wav_header(uint8_t *h, uint32_t data_bytes){
    const uint32_t bytes_per_sample = AUDIO_BITS_PER_SAMPLE / 8;
    //mem cpy for text 
    //put16 for data of 2 bytes
    //put32 for data of 4 bytes 
    memcpy(h + 0, "RIFF", 4); //the text RIFF
    put32(h + 4, 36 + data_bytes); //36 + number of audio bytes 
    memcpy(h + 8,"WAVE",4); //the text wave
    memcpy(h + 12,"fmt ",4); //the text fmt 
    put32(h + 16,16); //size of the format section
    put16(h + 20,1); //1 meaning plain pcm 
    put16(h + 22, AUDIO_CHANNELS); //channel count
    put32(h + 24,AUDIO_SAMPLE_RATE);//sample rate 
    put32(h + 28,AUDIO_SAMPLE_RATE*AUDIO_CHANNELS*bytes_per_sample); //byte rate (sample rate * channels * bytes per sample)
    put16(h + 32, AUDIO_CHANNELS*bytes_per_sample); //blaock align (channel * bytes per sample)
    put16(h + 34,AUDIO_BITS_PER_SAMPLE); //bits per sample 
    memcpy(h + 36,"data",4); //the text data
    put32(h + 40,data_bytes); //the number of audio bytes 
}

static uint8_t *record_wav(size_t *out_len){
    size_t total = 44 + PCM_BYTES; 
    uint8_t *buf = heap_caps_malloc(total, MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT); //try psam
    if (buf == NULL){
        buf = heap_caps_malloc(total, MALLOC_CAP_8BIT); //fall back to normal ram
    }
    if (buf == NULL){
        ESP_LOGE(TAG, "allocation failed");
        return NULL;
    }

    wav_header(buf,PCM_BYTES);
    int16_t* pcm = (int16_t*)(buf + 44); 
    
    for (int f = 0; f < RECORD_FRAMES; f++){
        size_t bytes_read = 0;
        if ((i2s_mic_read(s_mic_buf, MIC_BUF_BYTES, &bytes_read, portMAX_DELAY)) != ESP_OK){
            free(buf);
            return NULL; 
        }   
        //3 convert    
        int16_t *dst = pcm + f * AUDIO_FRAME_SAMPLES;
        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++){
            dst[i] = (int16_t)(s_mic_buf[MIC_CHANNEL_COUNT*i + KEEP_SLOT] >> SAMPLE_SHIFT);
        }
    }
    *out_len = total;
    return buf;
}

static esp_err_t upload_wav(const uint8_t *wav, size_t len){
    esp_http_client_config_t web_config = {
    .url = CONFIG_AUDIO_UPLOAD_URL,
    .method = HTTP_METHOD_POST,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .timeout_ms = 20000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&web_config);
    if (client == NULL){
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "X-Device-Key", CONFIG_AUDIO_DEVICE_KEY);
    esp_http_client_set_post_field(client, (const char*)wav, len);

    s_reply_len = 0;
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "upload status %d", esp_http_client_get_status_code(client));
        int status = esp_http_client_get_status_code(client);    
       
    } else {
        ESP_LOGE(TAG, "upload failed: %s", esp_err_to_name(err));
    }
    esp_err_to_name(err);
    esp_http_client_cleanup(client);
    return err;
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
            ESP_LOGI(TAG,"heard: %s", s_reply);
        }
       
    } else {
        ESP_LOGE(TAG, "failed to get text because: %s", esp_err_to_name(err));
    }
    esp_err_to_name(err);
    esp_http_client_cleanup(client);
}

void IRAM_ATTR SHAKE_ISR(void *parameter){
    BaseType_t xHigherPrioTaskWoken = pdFALSE;
    xSemaphoreGiveFromISR(xPIRSem, &xHigherPrioTaskWoken);
    if (xHigherPrioTaskWoken == pdTRUE){
        portYIELD_FROM_ISR();
    }
}

static void speaker_up(void){
    ESP_LOGI(TAG, "Starting I2S audio");

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(
            I2S_NUM_1,
            I2S_ROLE_MASTER
        );

    ESP_ERROR_CHECK(
        i2s_new_channel(
            &chan_cfg,
            &tx_handle,
            NULL
        )
    );

    i2s_std_config_t std_cfg = {
        .clk_cfg =
            I2S_STD_CLK_DEFAULT_CONFIG(16000),

        .slot_cfg = {
            .data_bit_width =
                I2S_DATA_BIT_WIDTH_16BIT,

            .slot_bit_width =
                I2S_SLOT_BIT_WIDTH_AUTO,

            .slot_mode =
                I2S_SLOT_MODE_MONO,

            .slot_mask =
                I2S_STD_SLOT_BOTH,

            .ws_width =
                I2S_DATA_BIT_WIDTH_16BIT,

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

    ESP_ERROR_CHECK(
        i2s_channel_init_std_mode(
            tx_handle,
            &std_cfg
        )
    );

    ESP_ERROR_CHECK(
        i2s_channel_enable(tx_handle)
    );
}


static esp_err_t play_file(const char *name)
{
    char path[128];

    // Build the full path to the WAV file.
    int path_len = snprintf(path, sizeof(path),
                            "/sdcard/%s", name);

    if (path_len < 0 || path_len >= sizeof(path)) {
        ESP_LOGE(TAG, "File path is too long");
        return ESP_ERR_INVALID_ARG;
    }

    // Open the file in binary read mode.
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s (errno=%d)",
                 path, errno);
        return ESP_FAIL;
    }

    // Skip the standard 44-byte PCM WAV header.
    if (fseek(f, 44, SEEK_SET) != 0) {
        ESP_LOGE(TAG, "Failed to skip WAV header: %s", path);
        fclose(f);
        return ESP_FAIL;
    }

    // 512 int16_t samples = 1024 bytes per chunk.
    int16_t chunk[512];

    while (1) {
        // Read up to sizeof(chunk) bytes of audio.
        size_t n = fread(chunk, 1, sizeof(chunk), f);

        if (n == 0) {
            if (ferror(f)) {
                ESP_LOGE(TAG, "Error reading %s", path);
            }
            break;
        }

        // Scale only complete 16-bit samples.
        for (size_t i = 0; i < n / sizeof(int16_t); i++) {
            chunk[i] /= VOLUME_DIVISOR;
        }

        // Send exactly the number of bytes read.
        size_t written = 0;
        esp_err_t err = i2s_channel_write(
            tx_handle,
            chunk,
            n,
            &written,
            portMAX_DELAY
        );

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2S write failed: %s",
                     esp_err_to_name(err));
            fclose(f);
            return err;
        }

        // Detect an incomplete write rather than silently
        // dropping audio bytes.
        if (written != n) {
            ESP_LOGE(TAG, "Short I2S write: %u of %u bytes",
                     (unsigned)written, (unsigned)n);
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

void shake_watch_task(void *parameters){
      TickType_t last_pir_sent_time = 0;
    TickType_t last_telemetry_sent_time = 0;
    
    while(1){
                TickType_t now = xTaskGetTickCount();
        if (xSemaphoreTake(ShakeSem,0) == pdTRUE){
            if (last_pir_sent_time == 0 ||
                (now - last_pir_sent_time) > pdMS_TO_TICKS(300)){
                printf("Reached\n");
                size_t len = 0;
                uint8_t *wav = record_wav(&len);
                    if (wav != NULL){
                        upload_wav(wav,len);
                        free(wav);
                    }
            }
        }

        
    }
}

void app_main(void)
{

    network_up();
    mic_up();
    sd_up();
    speaker_up();
    ShakeSem = xSemaphoreCreateBinary();
    if( ShakeSem != NULL ){
       // The semaphore was created successfully.
       // The semaphore can now be used.
       printf("Sema made\n");
    }
    assert(ShakeSem);
    gpio_install_isr_service(0);
    ESP_LOGI(TAG,"configuring PIR sensor\n");
    printf("configuration done\n");
    gpio_reset_pin(SHAKER_PIN);
    gpio_set_direction(SHAKER_PIN, GPIO_MODE_INPUT); //PIR Sensor will input a signal
    gpio_pullup_en(SHAKER_PIN);
    gpio_set_intr_type(SHAKER_PIN, GPIO_INTR_POSEDGE);
    printf("we are getting stuck\n");
    gpio_isr_handler_add(SHAKER_PIN, SHAKE_ISR, NULL);
    esp_err_t err = play_file("fahh.WAV");

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Playback failed");
    }
    
}
