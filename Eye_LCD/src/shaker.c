// Shake detection for the SW-420 vibration sensor (digital output, GPIO4).
// Based on the original polling test; changed to edge interrupts plus a hold
// time, because the sensor chatters on/off while vibrating.
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "eye_renderer.h"
#include "shaker.h"

#define SENSOR_PIN      GPIO_NUM_4
#define SHAKE_HOLD_US   300000   // stay "shaking" this long after the last pulse
#define POLL_MS         20

// 1 = also stop blinking while shaking (and return to STANDBY afterwards).
// Leave 0 if your answer logic controls eye_set_mode() itself.
#define SHAKE_STOPS_BLINK 0

static const char *TAG = "SHAKER";
static volatile uint32_t s_last_edge_us = 0;
static volatile bool     s_seen_edge    = false;

// Any edge (rising or falling) means the sensor is vibrating.
static void IRAM_ATTR sensor_isr(void *arg)
{
    s_last_edge_us = (uint32_t)esp_timer_get_time();
    s_seen_edge    = true;
}

static void shaker_task(void *arg)
{
    bool shaking = false;

    while (1) {
        bool now_shaking = s_seen_edge &&
            ((uint32_t)esp_timer_get_time() - s_last_edge_us) < SHAKE_HOLD_US;

        if (now_shaking != shaking) {
            shaking = now_shaking;
            ESP_LOGI(TAG, "%s", shaking ? "Vibration" : "Vibration stopped");
            eye_set_shake(shaking);
#if SHAKE_STOPS_BLINK
            eye_set_mode(shaking ? EYE_MODE_ANSWERING : EYE_MODE_STANDBY);
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void shaker_start(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << SENSOR_PIN,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(SENSOR_PIN, sensor_isr, NULL));

    xTaskCreate(shaker_task, "shaker", 3072, NULL, 4, NULL);
}