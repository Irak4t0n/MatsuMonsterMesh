/**
 * Copyright (c) 2023 Nicolai Electronics
 *
 * SPDX-License-Identifier: MIT
 */

#include "ssd1619.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_struct.h"
#include "soc/spi_reg.h"

static const char* TAG = "SSD1619";

static void IRAM_ATTR ssd1619_spi_pre_transfer_callback(spi_transaction_t* transaction) {
    ssd1619_t* handle = ((ssd1619_t*)transaction->user);
    gpio_set_level(handle->pin_dcx, handle->dc_level);
}

static esp_err_t ssd1619_send(ssd1619_t* handle, const uint8_t* data, const int len, const bool dc_level) {
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }

    if (len == 0) {
        return ESP_OK;
    }

    handle->dc_level              = dc_level;
    spi_transaction_t transaction = {
        .length    = len * 8,  // transaction length is in bits
        .rxlength  = 0,
        .tx_buffer = data,
        .rx_buffer = NULL,
        .user      = (void*)handle,
    };

    return spi_device_transmit(handle->spi_device, &transaction);
}

static esp_err_t ssd1619_send_command(ssd1619_t* handle, const uint8_t cmd) {
    return ssd1619_send(handle, &cmd, 1, false);
}

static esp_err_t ssd1619_send_data(ssd1619_t* handle, const uint8_t* data, const uint16_t length) {
    return ssd1619_send(handle, data, length, true);
}

static esp_err_t ssd1619_send_u8(ssd1619_t* handle, const uint8_t data) {
    return ssd1619_send(handle, &data, 1, true);
}

static esp_err_t ssd1619_reset(ssd1619_t* handle) {
    if (handle->pin_reset >= 0) {
        ESP_LOGI(TAG, "epaper display reset");
        esp_err_t res = gpio_set_level(handle->pin_reset, false);
        if (res != ESP_OK) return res;
        vTaskDelay(50 / portTICK_PERIOD_MS);
        res = gpio_set_level(handle->pin_reset, true);
        if (res != ESP_OK) return res;
        vTaskDelay(50 / portTICK_PERIOD_MS);
    } else {
        ESP_LOGI(TAG, "(no reset pin available)");
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    return ESP_OK;
}

bool ssd1619_busy(ssd1619_t* handle) {
    return gpio_get_level(handle->pin_busy);
}

esp_err_t ssd1619_wait(ssd1619_t* handle) {
    while (ssd1619_busy(handle)) {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    return ESP_OK;
}

static esp_err_t ssd1619_write_lut(ssd1619_t* handle) {
    esp_err_t res;
    if (handle->lut == NULL) {
        return ESP_FAIL;
    }
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }
    res = ssd1619_send_command(handle, SSD1619_CMD_WRITE_LUT_REGISTER);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_data(handle, handle->lut, 70);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_set_gate_driving_voltage(handle, handle->lut[70]);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_set_source_driving_voltage(handle, handle->lut[71], handle->lut[72], handle->lut[73]);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_set_dummy_line_period(handle, handle->lut[74]);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_set_gate_line_width(handle, handle->lut[75]);
    return res;
}

esp_err_t ssd1619_set_gate_driving_voltage(ssd1619_t* handle, uint8_t value) {
    esp_err_t res;
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }
    res = ssd1619_send_command(handle, SSD1619_CMD_GATE_DRIVING_VOLTAGE);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, value & 0x1F);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Gate driving voltage set to 0x%02x", value & 0x1F);
    }
    return res;
}

esp_err_t ssd1619_set_source_driving_voltage(ssd1619_t* handle, uint8_t vsh1, uint8_t vsh2, uint8_t vsl) {
    esp_err_t res;
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }
    res = ssd1619_send_command(handle, SSD1619_CMD_SOURCE_DRIVING_VOLTAGE_CONTROL);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, vsh1);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, vsh2);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, vsl);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Source driving voltage set to 0x%02x 0x%02x 0x%02x", vsh1, vsh2, vsl);
    }
    return res;
}

esp_err_t ssd1619_set_dummy_line_period(ssd1619_t* handle, uint8_t period) {
    esp_err_t res;
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }
    res = ssd1619_send_command(handle, SSD1619_CMD_SET_DUMMY_LINE_PERIOD);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, period & 0x7F);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Dummy line period set to 0x%02x", period & 0x7F);
    }
    return res;
}

esp_err_t ssd1619_set_gate_line_width(ssd1619_t* handle, uint8_t width) {
    esp_err_t res;
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }
    res = ssd1619_send_command(handle, SSD1619_CMD_SET_GATE_LINE_WIDTH);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, width & 0x0F);
    if (res == ESP_OK) {
        ESP_LOGI(TAG, "Gate line width set to 0x%02x", width & 0x0F);
    }
    return res;
}

esp_err_t ssd1619_init(ssd1619_t* handle) {
    esp_err_t res;

    if (handle->pin_dcx < 0) return ESP_FAIL;
    if (handle->pin_cs < 0) return ESP_FAIL;

    handle->lut = NULL;

    res = gpio_reset_pin(handle->pin_dcx);
    if (res != ESP_OK) return res;
    res = gpio_reset_pin(handle->pin_cs);
    if (res != ESP_OK) return res;

    if (handle->pin_reset >= 0) {
        res = gpio_reset_pin(handle->pin_reset);
        if (res != ESP_OK) return res;
        res = gpio_set_direction(handle->pin_reset, GPIO_MODE_OUTPUT);
        if (res != ESP_OK) return res;
    }

    // Initialize data/clock select GPIO pin
    res = gpio_set_direction(handle->pin_dcx, GPIO_MODE_OUTPUT);
    if (res != ESP_OK) return res;

    res = gpio_set_direction(handle->pin_busy, GPIO_MODE_INPUT);
    if (res != ESP_OK) return res;

    if (handle->spi_device == NULL) {
        spi_device_interface_config_t devcfg = {.command_bits     = 0,
                                                .address_bits     = 0,
                                                .dummy_bits       = 0,
                                                .mode             = 0,  // SPI mode 0
                                                .duty_cycle_pos   = 128,
                                                .cs_ena_pretrans  = 0,
                                                .cs_ena_posttrans = 0,
                                                .clock_speed_hz   = handle->spi_speed,
                                                .input_delay_ns   = 0,
                                                .spics_io_num     = handle->pin_cs,
                                                .flags            = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
                                                .queue_size       = 1,
                                                .pre_cb  = ssd1619_spi_pre_transfer_callback,  // Handles D/C line
                                                .post_cb = NULL};
        res                                  = spi_bus_add_device(handle->spi_bus, &devcfg, &handle->spi_device);
        if (res != ESP_OK) return res;
    }

    ssd1619_reset(handle);

    return ESP_OK;
}

esp_err_t ssd1619_deinit(ssd1619_t* handle) {
    esp_err_t res;
    if (handle->spi_device != NULL) {
        res                = spi_bus_remove_device(handle->spi_device);
        handle->spi_device = NULL;
        if (res != ESP_OK) {
            return res;
        }
    }
    res = gpio_set_direction(handle->pin_dcx, GPIO_MODE_INPUT);
    if (res != ESP_OK) return res;
    res = gpio_set_direction(handle->pin_cs, GPIO_MODE_INPUT);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_reset(handle);
    if (res != ESP_OK) {
        return res;
    }
    return res;
}

esp_err_t ssd1619_write(ssd1619_t* handle, const uint8_t* buffer) {
    if (handle->spi_device == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Reset");
    ssd1619_reset(handle);
    ssd1619_send_command(handle, SSD1619_CMD_SW_RESET);
    ssd1619_wait(handle);
    vTaskDelay(10 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Configure");
    ssd1619_send_command(handle, SSD1619_CMD_SET_ANALOG_BLOCK_CONTROL);
    ssd1619_send_u8(handle, 0x54);

    ssd1619_send_command(handle, SSD1619_CMD_SET_DIGITAL_BLOCK_CONTROL);
    ssd1619_send_u8(handle, 0x3B);

    ssd1619_send_command(handle, SSD1619_CMD_UNKNOWN_1);  // ACVCOM setting
    ssd1619_send_u8(handle, 0x04);
    ssd1619_send_u8(handle, 0x63);

    ssd1619_send_command(handle, SSD1619_CMD_BOOSTER_SOFT_START_CONTROL);
    ssd1619_send_u8(handle, 0x8F);
    ssd1619_send_u8(handle, 0x8F);
    ssd1619_send_u8(handle, 0x8F);
    ssd1619_send_u8(handle, 0x3F);

    ssd1619_send_command(handle, SSD1619_CMD_DRIVER_OUTPUT_CONTROL);
    ssd1619_send_u8(handle, ((handle->screen_height - 1) & 0xFF));
    ssd1619_send_u8(handle, ((handle->screen_height - 1) >> 8) && 0x01);
    ssd1619_send_u8(handle, 0x00);

    ssd1619_send_command(handle, SSD1619_CMD_DATA_ENTRY_MODE_SETTING);
    ssd1619_send_u8(handle, 0x01);

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_X_ADDRESS_LIMITS);
    ssd1619_send_u8(handle, 0x00);
    ssd1619_send_u8(handle, (handle->screen_width / 8) - 1);

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_Y_ADDRESS_LIMITS);
    ssd1619_send_u8(handle, (handle->screen_height - 1) & 0xFF);
    ssd1619_send_u8(handle, ((handle->screen_height - 1) >> 8));
    ssd1619_send_u8(handle, 0x00);
    ssd1619_send_u8(handle, 0x00);

    ssd1619_send_command(handle, SSD1619_CMD_BORDER_WAVEFORM_CONTROL);
    ssd1619_send_u8(handle, 0x01);  // 0 = black,1 = white,2 = Red

    ssd1619_send_command(handle, SSD1619_CMD_TEMPERATURE_SENSOR_CONTROL);
    ssd1619_send_u8(handle, 0x80);  // 0x48 = External,0x80 = Internal

    ssd1619_send_command(handle, SSD1619_CMD_DISPLAY_UPDATE_CONTROL_1);
    ssd1619_send_u8(handle, 0b00001000);  // inverse or ignore ram content

    ssd1619_send_command(handle, SSD1619_CMD_DISPLAY_UPDATE_CONTROL_2);
    ssd1619_send_u8(handle,
                    SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_ON | SSD1619_DISPLAY_UPDATE_CONTROL_2_LATCH_TEMPERATURE_VAL |
                        SSD1619_DISPLAY_UPDATE_CONTROL_2_LOAD_LUT |
                        SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_OFF);  // 0xB1

    ssd1619_send_command(handle, SSD1619_CMD_MASTER_ACTIVATION);
    ssd1619_wait(handle);

    // Red framebuffer

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_X_ADDRESS_COUNTER);
    ssd1619_send_u8(handle, 0x00);

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_Y_ADDRESS_COUNTER);
    ssd1619_send_u8(handle, (handle->screen_height - 1) & 0xFF);
    ssd1619_send_u8(handle, ((handle->screen_height - 1) >> 8));

    ssd1619_send_command(handle, SSD1619_CMD_WRITE_RAM_RED);

    for (int y = 0; y < handle->screen_height; y++) {
        for (int x = (handle->screen_width / 8) - 1; x >= 0; x--) {
            uint32_t position   = y * ((handle->screen_width / 8) * 2) + x * 2;
            uint16_t pixels     = buffer[position] | (buffer[position + 1] << 8);
            uint8_t  out        = 0;
            pixels            >>= 1;
            for (int bit = 0; bit < 8; bit++) {
                out      = (out >> 1) | ((pixels & 1) << 7);
                pixels >>= 2;
            }
            ssd1619_send_u8(handle, out);
        }
    }

    // Black framebuffer

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_X_ADDRESS_COUNTER);
    ssd1619_send_u8(handle, 0x00);

    ssd1619_send_command(handle, SSD1619_CMD_SET_RAM_Y_ADDRESS_COUNTER);
    ssd1619_send_u8(handle, (handle->screen_height - 1) & 0xFF);
    ssd1619_send_u8(handle, ((handle->screen_height - 1) >> 8));

    ssd1619_send_command(handle, SSD1619_CMD_WRITE_RAM_BLACK);

    for (int y = 0; y < handle->screen_height; y++) {
        for (int x = (handle->screen_width / 8) - 1; x >= 0; x--) {
            uint32_t position = y * ((handle->screen_width / 8) * 2) + x * 2;
            uint16_t pixels   = buffer[position] | (buffer[position + 1] << 8);
            uint8_t  out      = 0;
            for (int bit = 0; bit < 8; bit++) {
                out      = (out >> 1) | ((pixels & 1) << 7);
                pixels >>= 2;
            }
            ssd1619_send_u8(handle, out);
        }
    }

    if (handle->lut) {
        ssd1619_write_lut(handle);
    }

    ssd1619_send_command(handle, SSD1619_CMD_DISPLAY_UPDATE_CONTROL_2);
    ssd1619_send_u8(handle,
                    SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_ON | SSD1619_DISPLAY_UPDATE_CONTROL_2_ANALOG_ON |
                        SSD1619_DISPLAY_UPDATE_CONTROL_2_USE_MODE_1 | SSD1619_DISPLAY_UPDATE_CONTROL_2_USE_MODE_2 |
                        SSD1619_DISPLAY_UPDATE_CONTROL_2_ANALOG_OFF | SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_OFF);

    ssd1619_send_command(handle, SSD1619_CMD_MASTER_ACTIVATION);
    return ESP_OK;
}

esp_err_t ssd1619_sleep(ssd1619_t* handle) {
    esp_err_t res;
    ESP_LOGI(TAG, "Set display to deep sleep mode");
    res = ssd1619_send_command(handle, SSD1619_CMD_DEEP_SLEEP_MODE);
    if (res != ESP_OK) {
        return res;
    }
    res = ssd1619_send_u8(handle, 1);  // Enter deep sleep mode 1
    return res;
}

// Set the active LUT.
// Does not create a copy of the LUT.
esp_err_t ssd1619_set_lut(ssd1619_t* handle, const uint8_t* lut) {
    handle->lut = lut;
    return ESP_OK;
}
