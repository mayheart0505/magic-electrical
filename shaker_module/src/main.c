/*Arduino to Espressif Notes
    digitalRead -> gpio_get_level
*/
#include <stdio.h>
#include <stdbool.h>
#include "esp_log.h"

#include "driver/gpio.h"

#define SENSOR_PIN GPIO_NUM_4

int lastState = 0;
int currentState;

void app_main() {
    
    gpio_set_direction(SENSOR_PIN, GPIO_MODE_INPUT);
    ESP_LOGI("TEST", "app_main started");
    while(1){
        currentState = gpio_get_level(SENSOR_PIN);
        if (lastState == 0 && currentState == 1){
            ESP_LOGI("SW420", "Vibration ");
        }else if (lastState == 1 && currentState ==0 ){
            ESP_LOGI("SW420", "Vibration Stopped");
        }

        lastState = currentState; 
    }
 
}