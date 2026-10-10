/*
 * i2s_mic Wi-Fi audio sender — INMP441 -> ESP32 -> Wi-Fi (UDP via udp_link) -> receiver
 *
 * Captures audio with i2s_mic and sends it over Wi-Fi to one statically
 * configured IPv4 address, using the udp_link component for the UDP transport.
 * The receiver is either the i2s_spk Wi-Fi example (plays it on a speaker)
 * or tools/udp_audio_rx.py on a PC (plays or records it).
 *
 * This is one half of what becomes a Wi-Fi walkie-talkie: the sender. The
 * stream format lives in audio_stream_format.h so the receiver, and later the
 * full-duplex version, use exactly the same contract.
 *
 * Data path, one iteration per 30 ms:
 *   1. i2s_mic_read() blocks until the next DMA buffer is ready: 480 stereo
 *      frames of 32-bit samples (3840 bytes).
 *   2. Keep one slot of each frame (KEEP_SLOT) and shift the 24-in-32-bit
 *      INMP441 sample down to 16-bit: 480 mono samples (960 bytes).
 *   3. udp_link_send() sends those 960 bytes as one UDP datagram. udp_link adds its
 *      own header with a sequence number, so the receiver can detect loss.
 *
 * The audio task never waits on the network. If udp_link_send() reports no
 * buffer or no route (for example during a Wi-Fi reconnect), that frame is
 * dropped and counted, and capture carries on. If the task itself is ever
 * held up, finished mic buffers wait in the I2S driver's queue (up to 5,
 * ~150 ms); beyond that i2s_mic_get_overflow_count() reports the loss.
 *
 * Wiring (pins per target in sdkconfig.defaults.<target>, or menuconfig):
 *   INMP441 SCK  -> AUDIO_TX_MIC_GPIO_BCK
 *   INMP441 WS   -> AUDIO_TX_MIC_GPIO_WS
 *   INMP441 SD   -> AUDIO_TX_MIC_GPIO_DATA
 *   INMP441 L/R  -> GND   (selects the slot this example keeps; see KEEP_SLOT)
 *   INMP441 VDD  -> 3.3V
 *   INMP441 GND  -> GND
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "protocol_examples_common.h"
#include "sdkconfig.h"

#include "udp_link.h"
#include "i2s_mic.h"
#include "audio_stream_format.h"

static const char *TAG = "audio_tx";

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
_Static_assert(AUDIO_FRAME_BYTES <= UDP_LINK_MAX_PAYLOAD_LIMIT, "frame must fit in one datagram");
_Static_assert(AUDIO_BITS_PER_SAMPLE == 16 && AUDIO_CHANNELS == 1, "conversion below assumes 16-bit mono");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "stream format is little-endian PCM");

#define AUDIO_TASK_PRIO       6
#define AUDIO_TASK_STACK      4096
#define REPORT_MS             2000

/* ---- State ------------------------------------------------------------- */

static udp_link_handle_t s_link;

static int32_t s_mic_buf[MIC_FRAMES * MIC_CHANNEL_COUNT];
static int16_t s_frame[AUDIO_FRAME_SAMPLES];

/* written by the audio task, read by the reporter */
static _Atomic uint32_t s_sent;          /* frames handed to udp_link */
static _Atomic uint32_t s_dropped;       /* udp_link_send: no buffer / no route */
static _Atomic uint32_t s_send_errors;   /* udp_link_send: anything else */

/* ------------------------------------------------------------------------
 * Audio task: capture, convert, send.
 * ---------------------------------------------------------------------- */

static void audio_tx_task(void *arg)
{
    (void)arg;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_mic_read(s_mic_buf, MIC_BUF_BYTES, &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            /* Only happens if capture is stopped; this example never stops. */
            ESP_LOGE(TAG, "i2s_mic_read: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (size_t i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
            s_frame[i] = (int16_t)(s_mic_buf[MIC_CHANNEL_COUNT * i + KEEP_SLOT] >> SAMPLE_SHIFT);
        }

        err = udp_link_send(s_link, s_frame, AUDIO_FRAME_BYTES);
        if (err == ESP_OK) {
            atomic_fetch_add(&s_sent, 1);
        } else if (err == ESP_ERR_NO_MEM || err == ESP_ERR_UDP_LINK_NO_ROUTE) {
            /* Network momentarily unavailable: drop this frame, keep capturing.
             * Audio is real-time; resending late frames would only add delay. */
            atomic_fetch_add(&s_dropped, 1);
        } else {
            atomic_fetch_add(&s_send_errors, 1);
            ESP_LOGE(TAG, "udp_link_send: %s", udp_link_err_to_name(err));
        }
    }
}

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

static void link_up(void)
{
    esp_ip4_addr_t peer;
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(CONFIG_AUDIO_TX_PEER_IP, &peer));

    const udp_link_config_t cfg = {
        .local_port  = CONFIG_AUDIO_TX_PORT,
        .peer_ip     = peer,
        .peer_port   = CONFIG_AUDIO_TX_PORT,
        .stream_id   = AUDIO_STREAM_ID,
        .max_payload = AUDIO_FRAME_BYTES,
    };
    ESP_ERROR_CHECK(udp_link_open(&cfg, &s_link));

    ESP_LOGI(TAG, "sending to %s:%d, stream 0x%02X, %d-byte frames (%d ms of %d Hz %d-bit mono)",
             CONFIG_AUDIO_TX_PEER_IP, CONFIG_AUDIO_TX_PORT, AUDIO_STREAM_ID, AUDIO_FRAME_BYTES,
             AUDIO_FRAME_MS, AUDIO_SAMPLE_RATE, AUDIO_BITS_PER_SAMPLE);
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

void app_main(void)
{
    /* Network first, so the microphone isn't running (and overflowing)
     * while Wi-Fi connects. */
    network_up();
    link_up();
    mic_up();

    BaseType_t rc = xTaskCreate(audio_tx_task, "audio_tx", AUDIO_TASK_STACK, NULL,
                                AUDIO_TASK_PRIO, NULL);
    configASSERT(rc == pdPASS);
    (void)rc;

    /* Report once every REPORT_MS. Expect ~33 frames/s sent and both loss
     * counters at 0 on a healthy link. */
    uint32_t last_sent = 0;
    TickType_t last = xTaskGetTickCount();
    while (1) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(REPORT_MS));
        uint32_t sent = atomic_load(&s_sent);
        uint32_t rate = (sent - last_sent) * 1000u / REPORT_MS;
        last_sent = sent;
        ESP_LOGI(TAG, "sent %" PRIu32 " frames (%" PRIu32 "/s, %" PRIu32 " kbit/s) | "
                      "dropped %" PRIu32 " | send errors %" PRIu32 " | mic lost %" PRIu32,
                 sent, rate, rate * AUDIO_FRAME_BYTES * 8u / 1000u,
                 atomic_load(&s_dropped), atomic_load(&s_send_errors),
                 i2s_mic_get_overflow_count());
    }
}
