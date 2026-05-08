/**
 * Copyright (c) 2023-2025 Nicolai Electronics
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif  //__cplusplus

#include "ssd1619.h"

esp_err_t ssd1619_read_lut(int pin_data, int pin_clk, int pin_cs, int pin_dc, int pin_reset, int pin_busy);
esp_err_t ssd1619_reset_lut();
bool      ssd1619_get_lut_populated();
esp_err_t ssd1619_get_lut(uint8_t temperature, uint8_t* target_buffer);
esp_err_t ssd1619_apply_lut(ssd1619_t* handle, ssd1619_lut_t lut_type);

#ifdef __cplusplus
}
#endif  //__cplusplus
