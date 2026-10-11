#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"

#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"

#include "pcm.h"

#define I2S_BCLK GPIO_NUM_12
#define I2S_WS   GPIO_NUM_10
#define I2S_DOUT GPIO_NUM_11

// Start quiet: 10% PCM amplitude
#define VOLUME_DIVISOR 10

static const char *TAG = "MERRY_XMAS";
static i2s_chan_handle_t tx_handle = NULL;

void app_main(void)
{
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
            .ws   = I2S_WS,
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

    size_t pcm_size =
        sizeof(_MERRY_CHRISTMAS_PCM);

    ESP_LOGI(
        TAG,
        "PCM size: %u bytes",
        (unsigned int)pcm_size
    );

    uint8_t *quiet_pcm = malloc(pcm_size);

    if (quiet_pcm == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        return;
    }

    const int16_t *input =
        (const int16_t *)_MERRY_CHRISTMAS_PCM;

    int16_t *output =
        (int16_t *)quiet_pcm;

    size_t sample_count =
        pcm_size / sizeof(int16_t);

    // Reduce volume to 1/10
    for (size_t i = 0; i < sample_count; i++)
    {
        output[i] = input[i] / VOLUME_DIVISOR;
    }

    ESP_LOGI(TAG, "Playing Merry Christmas...");

    size_t bytes_written = 0;

    ESP_ERROR_CHECK(
        i2s_channel_write(
            tx_handle,
            quiet_pcm,
            pcm_size,
            &bytes_written,
            portMAX_DELAY
        )
    );

    ESP_LOGI(
        TAG,
        "Finished. Wrote %u bytes.",
        (unsigned int)bytes_written
    );

    free(quiet_pcm);
}