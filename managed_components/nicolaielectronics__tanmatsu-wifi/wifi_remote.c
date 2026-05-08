// SPDX-FileCopyrightText: 2026 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static bool initialized = false;

bool wifi_remote_get_initialized(void) {
    return initialized;
}

#if defined(CONFIG_BSP_TARGET_TANMATSU) || defined(CONFIG_BSP_TARGET_KONSOOL)
/// Implementation for Tanmatsu

#include <string.h>
#include "bsp/power.h"
#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "WiFi remote";

int hosted_reset_slave_callback(void) {
    ESP_LOGW(TAG, "Switching radio off...");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG, "Switching radio to application mode...");
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    ESP_LOGW(TAG, "Waiting for radio to start...");
    vTaskDelay(pdMS_TO_TICKS(1200));
    return 0;
}

// For backwards compatibility with the previous versions of esp-hosted-tanmatsu
esp_err_t hosted_sdio_reset_slave_callback(void) {
    return (hosted_reset_slave_callback() == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_remote_initialize(void) {
    if (initialized) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Starting ESP hosted...\r\n");
    esp_hosted_init();
    initialized = true;
    return ESP_OK;
}

#elif defined(CONFIG_IDF_TARGET_ESP32P4)
/// Generic implementation for ESP32-P4 targets
#include <string.h>
#include "esp_err.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char* TAG = "WiFi remote";

esp_err_t wifi_remote_initialize(void) {
    if (initialized) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Starting ESP hosted...\r\n");
    esp_hosted_init();
    initialized = true;
    return ESP_OK;
}

#else
/// Generic stub implementation for all devices using an ESP32 with built-in WiFi radio

#include "esp_err.h"

esp_err_t wifi_remote_initialize(void) {
    initialized = true;
    return ESP_OK;
}

#endif
