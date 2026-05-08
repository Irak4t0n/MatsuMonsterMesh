// SPDX-FileCopyrightText: 2025 Nicolai Electronics
// SPDX-License-Identifier: MIT

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Types
typedef struct mpr121 mpr121_t;
typedef mpr121_t*     mpr121_handle_t;

typedef struct {
    uint32_t touch_state;   // Bitmapped touch state
    uint32_t data[12];      // The electrode data    (10 bits)
    uint32_t baseline[12];  // The baseline          (8 bits)
    uint32_t touch[12];     // The touch threshold   (8 bits)
    uint32_t release[12];   // The release threshold (8 bits)
} mpr121_touch_info_t;

typedef void (*mpr121_touch_cb_t)(mpr121_handle_t, uint32_t previous_touch_state, uint32_t touch_state);
typedef void (*mpr121_input_cb_t)(mpr121_handle_t, uint32_t previous_input_state, uint32_t input_state);

typedef enum {
    MPR121_DISABLED         = 0x00,
    MPR121_INPUT            = 0x10,
    MPR121_INPUT_PULL_DOWN  = 0x12,
    MPR121_INPUT_PULL_UP    = 0x13,
    MPR121_OUTPUT           = 0x18,
    MPR121_OUTPUT_LOW_ONLY  = 0x1A,
    MPR121_OUTPUT_HIGH_ONLY = 0x1B,
} mpr121_gpio_mode_t;

typedef struct {
    gpio_num_t              int_io_num;             /// GPIO to which the interrupt line is connected
    i2c_master_bus_handle_t i2c_bus;                /// Handle of the I2C bus of the coprocessor
    uint8_t                 i2c_address;            /// I2C address of the coprocessor (7-bit)
    SemaphoreHandle_t       concurrency_semaphore;  /// Semaphore for making I2C bus operation thread safe
    mpr121_touch_cb_t       touch_callback;         /// Callback for touch events
    mpr121_input_cb_t       input_callback;         /// Callback for input events
    int                     i2c_timeout;            /// Timeout for I2C transfers in milliseconds
} mpr121_config_t;

// Functions

esp_err_t mpr121_initialize(const mpr121_config_t* configuration, mpr121_handle_t* out_handle);

esp_err_t mpr121_gpio_set_mode(mpr121_handle_t handle, uint8_t pin, mpr121_gpio_mode_t mode);
esp_err_t mpr121_gpio_get_mode(mpr121_handle_t handle, uint8_t pin, mpr121_gpio_mode_t* out_mode);
esp_err_t mpr121_gpio_set_level(mpr121_handle_t handle, uint8_t pin, bool level);
esp_err_t mpr121_gpio_get_level(mpr121_handle_t handle, uint8_t pin, bool* out_level);

esp_err_t mpr121_touch_get_analog(mpr121_handle_t handle, uint8_t pin, uint16_t* out_value);
esp_err_t mpr121_touch_set_baseline(mpr121_handle_t handle, uint8_t pin, uint8_t baseline);
esp_err_t mpr121_touch_set_touch_threshold(mpr121_handle_t handle, uint8_t pin, uint8_t touch_threshold);
esp_err_t mpr121_touch_set_release_threshold(mpr121_handle_t handle, uint8_t pin, uint8_t release_threshold);
esp_err_t mpr121_touch_configure(mpr121_handle_t handle, uint8_t num_touch_pins, uint8_t num_eleprox_pins,
                                 bool enable_baseline_tracking);

esp_err_t mpr121_read_gpio_state(mpr121_handle_t handle, uint8_t pin, uint8_t* out_state);
esp_err_t mpr121_read_touch_state(mpr121_handle_t handle, uint16_t* out_state);
