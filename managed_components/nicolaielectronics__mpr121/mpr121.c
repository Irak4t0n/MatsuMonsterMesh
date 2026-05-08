// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include "mpr121.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/queue.h"

#define MPR121_REG_TOUCHSTATUS_L             0x00
#define MPR121_REG_TOUCHSTATUS_H             0x01
#define MPR121_REG_OUTOFRANGE_L              0x02
#define MPR121_REG_OUTOFRANGE_H              0x03
#define MPR121_REG_FILTERED_DATA_0_L         0x04
#define MPR121_REG_FILTERED_DATA_0_H         0x05
#define MPR121_REG_FILTERED_DATA_1_L         0x06
#define MPR121_REG_FILTERED_DATA_1_H         0x07
#define MPR121_REG_FILTERED_DATA_2_L         0x08
#define MPR121_REG_FILTERED_DATA_2_H         0x09
#define MPR121_REG_FILTERED_DATA_3_L         0x0A
#define MPR121_REG_FILTERED_DATA_3_H         0x0B
#define MPR121_REG_FILTERED_DATA_4_L         0x0C
#define MPR121_REG_FILTERED_DATA_4_H         0x0D
#define MPR121_REG_FILTERED_DATA_5_L         0x0E
#define MPR121_REG_FILTERED_DATA_5_H         0x0F
#define MPR121_REG_FILTERED_DATA_6_L         0x10
#define MPR121_REG_FILTERED_DATA_6_H         0x11
#define MPR121_REG_FILTERED_DATA_7_L         0x12
#define MPR121_REG_FILTERED_DATA_7_H         0x13
#define MPR121_REG_FILTERED_DATA_8_L         0x14
#define MPR121_REG_FILTERED_DATA_8_H         0x15
#define MPR121_REG_FILTERED_DATA_9_L         0x16
#define MPR121_REG_FILTERED_DATA_9_H         0x17
#define MPR121_REG_FILTERED_DATA_10_L        0x18
#define MPR121_REG_FILTERED_DATA_10_H        0x19
#define MPR121_REG_FILTERED_DATA_11_L        0x1A
#define MPR121_REG_FILTERED_DATA_11_H        0x1B
#define MPR121_REG_FILTERED_DATA_ELEPROX_L   0x1C
#define MPR121_REG_FILTERED_DATA_ELEPROX_H   0x1D
#define MPR121_REG_BASELINE_0                0x1E
#define MPR121_REG_BASELINE_1                0x1F
#define MPR121_REG_BASELINE_2                0x20
#define MPR121_REG_BASELINE_3                0x21
#define MPR121_REG_BASELINE_4                0x22
#define MPR121_REG_BASELINE_5                0x23
#define MPR121_REG_BASELINE_6                0x24
#define MPR121_REG_BASELINE_7                0x25
#define MPR121_REG_BASELINE_8                0x26
#define MPR121_REG_BASELINE_9                0x27
#define MPR121_REG_BASELINE_10               0x28
#define MPR121_REG_BASELINE_11               0x29
#define MPR121_REG_BASELINE_ELEPROX          0x2A
#define MPR121_REG_MHD_RISING                0x2B
#define MPR121_REG_NHD_RISING                0x2C
#define MPR121_REG_NCL_RISING                0x2D
#define MPR121_REG_FDL_RISING                0x2E
#define MPR121_REG_MHD_FALLING               0x2F
#define MPR121_REG_NHD_FALLING               0x30
#define MPR121_REG_NCL_FALLING               0x31
#define MPR121_REG_FDL_FALLING               0x32
#define MPR121_REG_NHD_TOUCHED               0x33
#define MPR121_REG_NCL_TOUCHED               0x34
#define MPR121_REG_FDL_TOUCHED               0x35
#define MPR121_REG_ELEPROX_MHD_RISING        0x36
#define MPR121_REG_ELEPROX_NHD_RISING        0x37
#define MPR121_REG_ELEPROX_NCL_RISING        0x38
#define MPR121_REG_ELEPROX_FDL_RISING        0x39
#define MPR121_REG_ELEPROX_MHD_FALLING       0x3A
#define MPR121_REG_ELEPROX_NHD_FALLING       0x3B
#define MPR121_REG_ELEPROX_NCL_FALLING       0x3C
#define MPR121_REG_ELEPROX_FDL_FALLING       0x3D
#define MPR121_REG_ELEPROX_NHD_TOUCHED       0x3E
#define MPR121_REG_ELEPROX_NCL_TOUCHED       0x3F
#define MPR121_REG_ELEPROX_FDL_TOUCHED       0x40
#define MPR121_REG_TOUCH_THRESHOLD_0         0x41
#define MPR121_REG_RELEASE_THRESHOLD_0       0x42
#define MPR121_REG_TOUCH_THRESHOLD_1         0x43
#define MPR121_REG_RELEASE_THRESHOLD_1       0x44
#define MPR121_REG_TOUCH_THRESHOLD_2         0x45
#define MPR121_REG_RELEASE_THRESHOLD_2       0x46
#define MPR121_REG_TOUCH_THRESHOLD_3         0x47
#define MPR121_REG_RELEASE_THRESHOLD_3       0x48
#define MPR121_REG_TOUCH_THRESHOLD_4         0x49
#define MPR121_REG_RELEASE_THRESHOLD_4       0x4A
#define MPR121_REG_TOUCH_THRESHOLD_5         0x4B
#define MPR121_REG_RELEASE_THRESHOLD_5       0x4C
#define MPR121_REG_TOUCH_THRESHOLD_6         0x4D
#define MPR121_REG_RELEASE_THRESHOLD_6       0x4E
#define MPR121_REG_TOUCH_THRESHOLD_7         0x4F
#define MPR121_REG_RELEASE_THRESHOLD_7       0x50
#define MPR121_REG_TOUCH_THRESHOLD_8         0x51
#define MPR121_REG_RELEASE_THRESHOLD_8       0x52
#define MPR121_REG_TOUCH_THRESHOLD_9         0x53
#define MPR121_REG_RELEASE_THRESHOLD_9       0x54
#define MPR121_REG_TOUCH_THRESHOLD_10        0x55
#define MPR121_REG_RELEASE_THRESHOLD_10      0x56
#define MPR121_REG_TOUCH_THRESHOLD_11        0x57
#define MPR121_REG_RELEASE_THRESHOLD_11      0x58
#define MPR121_REG_TOUCH_THRESHOLD_ELEPROX   0x59
#define MPR121_REG_RELEASE_THRESHOLD_ELEPROX 0x5A
#define MPR121_REG_DEBOUNCE                  0x5B
#define MPR121_REG_AFE_CONFIGURATION_1       0x5C
#define MPR121_REG_AFE_CONFIGURATION_2       0x5D
#define MPR121_REG_ELECTRODE_CONFIGURATION   0x5E
#define MPR121_REG_CHARGE_CURRENT_0          0x5F
#define MPR121_REG_CHARGE_CURRENT_1          0x60
#define MPR121_REG_CHARGE_CURRENT_2          0x61
#define MPR121_REG_CHARGE_CURRENT_3          0x62
#define MPR121_REG_CHARGE_CURRENT_4          0x63
#define MPR121_REG_CHARGE_CURRENT_5          0x64
#define MPR121_REG_CHARGE_CURRENT_6          0x65
#define MPR121_REG_CHARGE_CURRENT_7          0x66
#define MPR121_REG_CHARGE_CURRENT_8          0x67
#define MPR121_REG_CHARGE_CURRENT_9          0x68
#define MPR121_REG_CHARGE_CURRENT_10         0x69
#define MPR121_REG_CHARGE_CURRENT_11         0x6A
#define MPR121_REG_CHARGE_CURRENT_ELEPROX    0x6B
#define MPR121_REG_CHARGE_TIME_0_1           0x6C
#define MPR121_REG_CHARGE_TIME_2_3           0x6D
#define MPR121_REG_CHARGE_TIME_4_5           0x6E
#define MPR121_REG_CHARGE_TIME_6_7           0x6F
#define MPR121_REG_CHARGE_TIME_8_9           0x70
#define MPR121_REG_CHARGE_TIME_10_11         0x71
#define MPR121_REG_CHARGE_TIME_ELEPROX       0x72
#define MPR121_REG_GPIO_CONTROL_0            0x73
#define MPR121_REG_GPIO_CONTROL_1            0x74
#define MPR121_REG_GPIO_DATA                 0x75
#define MPR121_REG_GPIO_DIRECTION            0x76
#define MPR121_REG_GPIO_ENABLE               0x77
#define MPR121_REG_GPIO_SET                  0x78
#define MPR121_REG_GPIO_CLEAR                0x79
#define MPR121_REG_GPIO_TOGGLE               0x7A
#define MPR121_REG_AUTOCONFIG_0              0x7B
#define MPR121_REG_AUTOCONFIG_1              0x7C
#define MPR121_REG_AUTOCONFIG_USL            0x7D
#define MPR121_REG_AUTOCONFIG_LSL            0x7E
#define MPR121_REG_AUTOCONFIG_TARGET_LEVEL   0x7F
#define MPR121_REG_SOFTRESET                 0x80

#define MPR121_TIMEOUT_MS 1000

#define MPR121_SOFTRESET_TRIGGER_VALUE 0x63

static char const TAG[] = "MPR121";

struct mpr121 {
    mpr121_config_t         configuration;             /// Configuration struct
    i2c_master_dev_handle_t dev_handle;                /// I2C device handle
    SemaphoreHandle_t       state_mutex;               /// Mutex for accessing the state
    SemaphoreHandle_t       interrupt_semaphore;       /// Semaphore for triggering interrupt handler thread
    TaskHandle_t            interrupt_handler_thread;  /// Handle for the interrupt handler thread
    uint32_t                touch_state;               /// Bitmapped touch state, updated in interrupt handler thread
    uint32_t                input_state;               /// Bitmapped input state, updated in interrupt handler thread
};

// Wrapping functions for making ESP-IDF I2C driver thread-safe

static void claim_i2c_bus(mpr121_handle_t handle) {
    // Claim I2C bus
    if (handle->configuration.concurrency_semaphore != NULL) {
        xSemaphoreTake(handle->configuration.concurrency_semaphore, portMAX_DELAY);
    } else {
        ESP_LOGW(TAG, "No concurrency semaphore");
    }
}

static void release_i2c_bus(mpr121_handle_t handle) {
    // Release I2C bus
    if (handle->configuration.concurrency_semaphore != NULL) {
        xSemaphoreGive(handle->configuration.concurrency_semaphore);
    }
}

static esp_err_t ts_i2c_master_transmit_receive(mpr121_handle_t handle, const uint8_t* write_buffer, size_t write_size,
                                                uint8_t* read_buffer, size_t read_size) {
    claim_i2c_bus(handle);
    esp_err_t res = i2c_master_transmit_receive(handle->dev_handle, write_buffer, write_size, read_buffer, read_size,
                                                handle->configuration.i2c_timeout);
    release_i2c_bus(handle);
    return res;
}

static esp_err_t ts_i2c_master_transmit(mpr121_handle_t handle, const uint8_t* write_buffer, size_t write_size) {
    claim_i2c_bus(handle);
    esp_err_t res =
        i2c_master_transmit(handle->dev_handle, write_buffer, write_size, handle->configuration.i2c_timeout);
    release_i2c_bus(handle);
    return res;
}

// Register read/write functions

static esp_err_t mpr121_read_reg(mpr121_handle_t handle, uint8_t reg, uint8_t* out_value) {
    ESP_RETURN_ON_ERROR(ts_i2c_master_transmit_receive(handle, (uint8_t[]){reg}, 1, out_value, 1), TAG,
                        "Communication fault");
    return ESP_OK;
}

static esp_err_t mpr121_read_regs(mpr121_handle_t handle, uint8_t reg, uint8_t* out_value, uint8_t length) {
    ESP_RETURN_ON_ERROR(ts_i2c_master_transmit_receive(handle, (uint8_t[]){reg}, 1, out_value, length), TAG,
                        "Communication fault");
    return ESP_OK;
}

static esp_err_t mpr121_write_reg8(mpr121_handle_t handle, uint8_t reg, uint8_t value) {
    ESP_RETURN_ON_ERROR(ts_i2c_master_transmit(handle, (uint8_t[]){reg, value}, 2), TAG, "Communication fault");
    return ESP_OK;
}

static esp_err_t mpr121_write_reg16(mpr121_handle_t handle, uint8_t reg, uint16_t value) {
    ESP_RETURN_ON_ERROR(ts_i2c_master_transmit(handle, (uint8_t[]){reg, value & 0xFF, value >> 8}, 3), TAG,
                        "Communication fault");
    return ESP_OK;
}

// Interrupt handler

IRAM_ATTR static void mpr121_interrupt_handler(void* pvParameters) {
    mpr121_handle_t handle = (mpr121_handle_t)pvParameters;
    xSemaphoreGiveFromISR(handle->interrupt_semaphore, NULL);
}

static void mpr121_interrupt_thread_entry(void* pvParameters) {
    mpr121_handle_t handle = (mpr121_handle_t)pvParameters;

    while (true) {
        // Wait for interrupt
        xSemaphoreTake(handle->interrupt_semaphore, pdMS_TO_TICKS(1000));

        uint16_t touch_state;
        if (mpr121_read_regs(handle, MPR121_REG_TOUCHSTATUS_L, (uint8_t*)&touch_state, 2) == ESP_OK) {
            if (handle->touch_state != touch_state) {
                if (handle->configuration.touch_callback) {
                    handle->configuration.touch_callback(handle, handle->touch_state, touch_state);
                }
                handle->touch_state = touch_state;
            }
        }

        uint8_t input_state;
        if (mpr121_read_regs(handle, MPR121_REG_GPIO_DATA, &input_state, 1) == ESP_OK) {
            if (handle->input_state != input_state) {
                if (handle->configuration.input_callback) {
                    handle->configuration.input_callback(handle, handle->input_state, input_state);
                }
                handle->input_state = input_state;
            }
        }
    }
}

// Public functions

esp_err_t mpr121_initialize(const mpr121_config_t* configuration, mpr121_handle_t* out_handle) {
    ESP_RETURN_ON_FALSE(configuration, ESP_ERR_INVALID_ARG, TAG, "invalid argument: configuration");
    ESP_RETURN_ON_FALSE(out_handle, ESP_ERR_INVALID_ARG, TAG, "invalid argument: handle");
    ESP_RETURN_ON_FALSE(configuration->i2c_bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument: i2c bus");
    ESP_RETURN_ON_FALSE(configuration->i2c_address, ESP_ERR_INVALID_ARG, TAG, "invalid argument: i2c address");

    ESP_RETURN_ON_ERROR(i2c_master_probe(configuration->i2c_bus, configuration->i2c_address, MPR121_TIMEOUT_MS), TAG,
                        "MPR121 not detected on I2C bus");

    mpr121_t* handle = heap_caps_calloc(1, sizeof(mpr121_t), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_NO_MEM, TAG, "no memory for handle struct");

    memcpy(&handle->configuration, configuration, sizeof(mpr121_config_t));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = configuration->i2c_address,
        .scl_speed_hz    = 400000,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(configuration->i2c_bus, &dev_cfg, &handle->dev_handle), TAG,
                        "Failed to add device to I2C bus");

    handle->interrupt_semaphore = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(handle->interrupt_semaphore, ESP_ERR_NO_MEM, TAG, "no memory for interrupt semaphore");

    esp_err_t res = mpr121_write_reg8(handle, MPR121_REG_SOFTRESET, MPR121_SOFTRESET_TRIGGER_VALUE);
    if (res != ESP_OK) {
        return res;
    }

    if (configuration->int_io_num >= 0) {
        assert(xTaskCreate(mpr121_interrupt_thread_entry, "MPR121 interrupt task", 4096, (void*)handle, 0,
                           &handle->interrupt_handler_thread) == pdTRUE);

        gpio_config_t int_pin_cfg = {
            .pin_bit_mask = BIT64(configuration->int_io_num),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = false,
            .pull_down_en = false,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        ESP_RETURN_ON_ERROR(gpio_config(&int_pin_cfg), TAG, "Failed to configure interrupt GPIO");
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(configuration->int_io_num, mpr121_interrupt_handler, (void*)handle),
                            TAG, "Failed to add interrupt handler for MPR121");
    }

    *out_handle = handle;
    return ESP_OK;
}

esp_err_t mpr121_gpio_set_mode(mpr121_handle_t handle, uint8_t pin, mpr121_gpio_mode_t mode) {
    if (pin < 4 || pin >= 12) {
        return ESP_ERR_INVALID_ARG;
    }
    pin -= 4;

    uint8_t data[6];  // Array used to hold address of first register (for direct write) and the values of the control
                      // 0, control 1, data, direction and enable registers
    data[0] = MPR121_REG_GPIO_CONTROL_0;

    esp_err_t res = mpr121_read_regs(handle, MPR121_REG_GPIO_CONTROL_0, (uint8_t*)&data[1], 5);
    if (res != ESP_OK) {
        return res;
    }

    for (uint8_t i = 0; i < 5; i++) {
        if (mode & (1 << i)) {
            data[i + 1] |= 1 << pin;
        } else {
            data[i + 1] &= ~(1 << pin);
        }
    }

    ESP_RETURN_ON_ERROR(ts_i2c_master_transmit(handle, data, 6), TAG, "Communication fault");
    return ESP_OK;
}

esp_err_t mpr121_gpio_get_mode(mpr121_handle_t handle, uint8_t pin, mpr121_gpio_mode_t* out_mode) {
    if (out_mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (pin < 4 || pin >= 12) {
        return ESP_ERR_INVALID_ARG;
    }
    pin -= 4;

    uint8_t mode = 0;

    uint8_t data[6];  // Array used to hold address of first register (for direct write) and the values of the control
                      // 0, control 1, data, direction and enable registers
    data[0] = MPR121_REG_GPIO_CONTROL_0;

    esp_err_t res = mpr121_read_regs(handle, MPR121_REG_GPIO_CONTROL_0, (uint8_t*)&data[1], 5);
    if (res != ESP_OK) {
        return res;
    }

    for (uint8_t i = 0; i < 5; i++) {
        if (data[i] & (1 << pin)) {
            mode |= 1 << i;
        }
    }

    *out_mode = (mpr121_gpio_mode_t)mode;

    return ESP_OK;
}

esp_err_t mpr121_gpio_set_level(mpr121_handle_t handle, uint8_t pin, bool state) {
    if (pin < 4 || pin >= 12) {
        return ESP_ERR_INVALID_ARG;
    }
    pin -= 4;
    return mpr121_write_reg8(handle, state ? MPR121_REG_GPIO_SET : MPR121_REG_GPIO_CLEAR, (1 << pin));
}

esp_err_t mpr121_gpio_get_level(mpr121_handle_t handle, uint8_t pin, bool* out_state) {
    if (pin < 4 || pin >= 12) {
        return ESP_ERR_INVALID_ARG;
    }
    pin -= 4;
    uint8_t   value;
    esp_err_t res = mpr121_read_regs(handle, MPR121_REG_GPIO_DATA, &value, 1);
    if (res != ESP_OK) {
        return res;
    }
    *out_state = (value >> pin) & 1;
    return res;
}

esp_err_t mpr121_touch_get_analog(mpr121_handle_t handle, uint8_t pin, uint16_t* out_value) {
    if (pin > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t  value;
    esp_err_t res = mpr121_read_regs(handle, MPR121_REG_FILTERED_DATA_0_L + (pin * 2), (uint8_t*)&value, 2);
    if (res != ESP_OK) {
        return res;
    }
    *out_value = value;
    return res;
}

esp_err_t mpr121_touch_set_baseline(mpr121_handle_t handle, uint8_t pin, uint8_t baseline) {
    if (pin > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    return mpr121_write_reg8(handle, MPR121_REG_BASELINE_0 + pin, baseline);
}

esp_err_t mpr121_touch_set_touch_threshold(mpr121_handle_t handle, uint8_t pin, uint8_t touch_threshold) {
    if (pin > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    return mpr121_write_reg8(handle, MPR121_REG_TOUCH_THRESHOLD_0 + (pin * 2), touch_threshold);
}

esp_err_t mpr121_touch_set_release_threshold(mpr121_handle_t handle, uint8_t pin, uint8_t release_threshold) {
    if (pin > 13) {
        return ESP_ERR_INVALID_ARG;
    }
    return mpr121_write_reg8(handle, MPR121_REG_RELEASE_THRESHOLD_0 + (pin * 2), release_threshold);
}

esp_err_t mpr121_touch_configure(mpr121_handle_t handle, uint8_t num_touch_pins, uint8_t num_eleprox_pins,
                                 bool enable_baseline_tracking) {
    return mpr121_write_reg8(
        handle, MPR121_REG_ELECTRODE_CONFIGURATION,
        (num_touch_pins & 0xF) | (num_eleprox_pins & 0x3) << 4 | (enable_baseline_tracking ? 0x80 : 0x40) << 6);
}

esp_err_t mpr121_read_gpio_state(mpr121_handle_t handle, uint8_t pin, uint8_t* out_state) {
    if (pin < 4 || pin >= 12) {
        return ESP_ERR_INVALID_ARG;
    }
    pin -= 4;
    uint8_t   value;
    esp_err_t res = mpr121_read_regs(handle, MPR121_REG_GPIO_DATA, &value, 1);
    if (res != ESP_OK) {
        return res;
    }
    *out_state = value;
    return res;
}

esp_err_t mpr121_read_touch_state(mpr121_handle_t handle, uint16_t* out_state) {
    return mpr121_read_regs(handle, MPR121_REG_TOUCHSTATUS_L, (uint8_t*)out_state, 2);
}

void mpr121_set_on_touch_handler(mpr121_handle_t handle, mpr121_touch_cb_t callback) {
    handle->configuration.touch_callback = callback;
}

void mpr121_set_on_input_handler(mpr121_handle_t handle, mpr121_input_cb_t callback) {
    handle->configuration.input_callback = callback;
}