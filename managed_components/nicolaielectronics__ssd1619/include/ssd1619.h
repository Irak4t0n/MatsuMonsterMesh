/**
 * Copyright (c) 2023-2025 Nicolai Electronics
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif  //__cplusplus

#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "stdbool.h"
#include "stdint.h"

#define SSD1619_LUT_SIZE 76

#define SSD1619_CMD_DRIVER_OUTPUT_CONTROL          0x01
#define SSD1619_CMD_GATE_DRIVING_VOLTAGE           0x03
#define SSD1619_CMD_SOURCE_DRIVING_VOLTAGE_CONTROL 0x04
#define SSD1619_CMD_BOOSTER_SOFT_START_CONTROL     0x0C
#define SSD1619_CMD_GATE_SCAN_START_POSITION       0x0F
#define SSD1619_CMD_DEEP_SLEEP_MODE                0x10
#define SSD1619_CMD_DATA_ENTRY_MODE_SETTING        0x11
#define SSD1619_CMD_SW_RESET                       0x12
#define SSD1619_CMD_SW_RESET_2                     0x13  // Not in datasheet
#define SSD1619_CMD_HV_READY_DETECTION             0x14
#define SSD1619_CMD_VCI_DETECTION                  0x15
#define SSD1619_CMD_TEMPERATURE_SENSOR_CONTROL     0x18
#define SSD1619_CMD_TEMPERATURE_SENSOR_WRITE       0x1A
#define SSD1619_CMD_TEMPERATURE_SENSOR_READ        0x1B
#define SSD1619_CMD_TEMPERATURE_SENSOR_WRITE_REG   0x1C
#define SSD1619_CMD_MASTER_ACTIVATION              0x20
#define SSD1619_CMD_DISPLAY_UPDATE_CONTROL_1       0x21
#define SSD1619_CMD_DISPLAY_UPDATE_CONTROL_2       0x22
#define SSD1619_CMD_WRITE_RAM_BLACK                0x24
#define SSD1619_CMD_WRITE_RAM_RED                  0x26
#define SSD1619_CMD_READ_RAM                       0x27
#define SSD1619_CMD_VCOM_SENSE                     0x28
#define SSD1619_CMD_VCOM_SENSE_DURATION            0x29
#define SSD1619_CMD_PROGRAM_VCOM_OTP               0x2A
#define SSD1619_CMD_UNKNOWN_1                      0x2B  // Not in datasheet
#define SSD1619_CMD_WRITE_VCOM_REGISTER            0x2C
#define SSD1619_CMD_OTP_REGISTER_READ              0x2D
#define SSD1619_CMD_USER_ID_READ                   0x2E
#define SSD1619_CMD_STATUS_BIT_READ                0x2F
#define SSD1619_CMD_PROGRAM_WS_OTP                 0x30
#define SSD1619_CMD_LOAD_WS_OTP                    0x31
#define SSD1619_CMD_WRITE_LUT_REGISTER             0x32
#define SSD1619_CMD_READ_LUT_REGISTER              0x33  // Not in datasheet
#define SSD1619_CMD_CRC_CALCULATION                0x34
#define SSD1619_CMD_CRC_STATUS_READ                0x35
#define SSD1619_CMD_PROGRAM_OTP_SELECTION          0x36
#define SSD1619_CMD_WRITE_OTP_SELECTION            0x37
#define SSD1619_CMD_WRITE_USER_ID                  0x38
#define SSD1619_CMD_OTP_PROGRAM_MODE               0x39
#define SSD1619_CMD_SET_DUMMY_LINE_PERIOD          0x3A
#define SSD1619_CMD_SET_GATE_LINE_WIDTH            0x3B
#define SSD1619_CMD_BORDER_WAVEFORM_CONTROL        0x3C
#define SSD1619_CMD_READ_RAM_OPTION                0x41
#define SSD1619_CMD_SET_RAM_X_ADDRESS_LIMITS       0x44
#define SSD1619_CMD_SET_RAM_Y_ADDRESS_LIMITS       0x45
#define SSD1619_CMD_AUTO_WRITE_RED_RAM             0x46
#define SSD1619_CMD_AUTO_WRITE_BW_RAM              0x47
#define SSD1619_CMD_SET_RAM_X_ADDRESS_COUNTER      0x4E
#define SSD1619_CMD_SET_RAM_Y_ADDRESS_COUNTER      0x4F
#define SSD1619_CMD_SET_ANALOG_BLOCK_CONTROL       0x74
#define SSD1619_CMD_SET_DIGITAL_BLOCK_CONTROL      0x7E
#define SSD1619_CMD_NOP                            0x7F

#define SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_OFF             0x01
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_ANALOG_OFF            0x02
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_USE_MODE_1            0x04
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_USE_MODE_2            0x08
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_LOAD_LUT              0x10
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_LATCH_TEMPERATURE_VAL 0x20
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_ANALOG_ON             0x40
#define SSD1619_DISPLAY_UPDATE_CONTROL_2_CLOCK_ON              0x80

typedef struct {
    uint8_t tp[4];
    uint8_t repeat;
} __attribute__((packed, aligned(1))) ssd1619_lut_group_t;

typedef struct {
    uint8_t             vs[35];     // Register 0x32
    ssd1619_lut_group_t groups[7];  // Register 0x32
    uint8_t             vgh;        // Gate level (0x03)
    uint8_t             vsh1;       // Source level (0x04)
    uint8_t             vsh2;       // Source level (0x04)
    uint8_t             vsl;        // Source level (0x04)
    uint8_t             frame1;     // Dummy line (0x3A)
    uint8_t             frame2;     // Gate line width (0x3B)
} __attribute__((packed, aligned(1))) ssd1619_lut7_t;

typedef struct {
    // Configuration
    int                 spi_bus;
    int                 pin_cs;
    int                 pin_dcx;
    int                 pin_reset;
    int                 pin_busy;
    uint32_t            spi_speed;
    uint32_t            spi_max_transfer_size;
    uint16_t            screen_width;
    uint16_t            screen_height;
    // Internal state
    spi_device_handle_t spi_device;
    bool                dc_level;
    SemaphoreHandle_t   mutex;
    const uint8_t*      lut;
} ssd1619_t;

typedef enum {
    lut_full = 0,
    lut_8s,
    lut_4s,
    lut_1s,
    lut_black,
    lut_red,
    lut_white,
    lut_red_from_black,
    lut_900ms
} ssd1619_lut_t;

esp_err_t ssd1619_init(ssd1619_t* handle);
esp_err_t ssd1619_deinit(ssd1619_t* handle);
bool      ssd1619_busy(ssd1619_t* handle);
esp_err_t ssd1619_wait(ssd1619_t* handle);
esp_err_t ssd1619_write(ssd1619_t* handle, const uint8_t* data);
esp_err_t ssd1619_set_lut(ssd1619_t* handle, const uint8_t* lut);
esp_err_t ssd1619_set_gate_driving_voltage(ssd1619_t* handle, uint8_t value);
esp_err_t ssd1619_set_source_driving_voltage(ssd1619_t* handle, uint8_t vsh1, uint8_t vsh2, uint8_t vsl);
esp_err_t ssd1619_set_dummy_line_period(ssd1619_t* handle, uint8_t period);
esp_err_t ssd1619_set_gate_line_width(ssd1619_t* handle, uint8_t width);
esp_err_t ssd1619_sleep(ssd1619_t* handle);

#ifdef __cplusplus
}
#endif  //__cplusplus
