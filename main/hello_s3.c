#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_timer.h"

static const char *TAG = "hello_s3";

void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    ESP_LOGI(TAG, "=== ESP32-S3 Hello Demo ===");
    ESP_LOGI(TAG, "Chip: %s rev%d, cores: %d",
             CONFIG_IDF_TARGET, chip.revision, chip.cores);
    ESP_LOGI(TAG, "Flash: %lu MB %s", flash_size / (1024 * 1024),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? "(embedded)" : "(external)");

    int count = 0;
    while (1) {
        ESP_LOGI(TAG, "Running... uptime %lu s, free heap %lu bytes, count=%d",
                 (unsigned long)(esp_timer_get_time() / 1000000ULL),
                 (unsigned long)esp_get_free_heap_size(), count++);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
