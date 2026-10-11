#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

// Reassigned Pins to avoid internal Octal Flash/PSRAM conflict (GPIO 17/18)
#define SD_CS   15
#define SD_SCK  16
#define SD_MOSI 17
#define SD_MISO 18

static const char *SD_TAG = "sd_card";

esp_err_t sd_up(void){
    // 1. Set up host configurations
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();

spi_bus_config_t serial_data = {
    .mosi_io_num = SD_MOSI,
    .miso_io_num = SD_MISO,
    .sclk_io_num = SD_SCK,
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .max_transfer_sz = 4096,
};

    // Use SPI_DMA_CH_AUTO for proper resource distribution on ESP32-S3
    esp_err_t err_sd = spi_bus_initialize(
        host.slot,
        &serial_data,
        SPI_DMA_CH_AUTO 
    );

    if (err_sd != ESP_OK) {
        ESP_LOGE(SD_TAG, "SPI initialization failed: %s", esp_err_to_name(err_sd));
        return err_sd;
    }    
    
    // 2. Clear device configuration defaults and assign specific parameters
    sdspi_device_config_t sd_mod = SDSPI_DEVICE_CONFIG_DEFAULT();
    sd_mod.gpio_cs = SD_CS;
    sd_mod.host_id = host.slot; 
    
    esp_vfs_fat_sdmmc_mount_config_t mounts = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_card_t* card;
    // 3. Mount the filesystem
    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &sd_mod, &mounts, &card);
    
    if (err != ESP_OK){
        ESP_LOGE(SD_TAG, "Error: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(SD_TAG, "Works! Card initialized successfully.");
        // Optional: Print card details to verify integrity
        sdmmc_card_print_info(stdout, card);
    }

    return err;
}

