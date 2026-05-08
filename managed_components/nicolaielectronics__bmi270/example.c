// Based on Bosch examples for using the gyroscope and accelerometer

#include <stdint.h>
#include <stdio.h>
#include "bmi270.h"
#include "bmi270_interface.h"
#include "bsp/i2c.h"
#include "esp_err.h"
#include "esp_log.h"

static int8_t set_gyro_config(struct bmi2_dev* dev) {
    int8_t                  rslt;
    struct bmi2_sens_config config;
    config.type = BMI2_GYRO;
    rslt        = bmi2_get_sensor_config(&config, 1, dev);
    bmi2_error_codes_print_result(rslt);
    rslt = bmi2_map_data_int(BMI2_DRDY_INT, BMI2_INT2, dev);
    bmi2_error_codes_print_result(rslt);
    if (rslt == BMI2_OK) {
        config.cfg.gyr.odr         = BMI2_GYR_ODR_100HZ;
        config.cfg.gyr.range       = BMI2_GYR_RANGE_2000;
        config.cfg.gyr.bwp         = BMI2_GYR_NORMAL_MODE;
        config.cfg.gyr.noise_perf  = BMI2_POWER_OPT_MODE;
        config.cfg.gyr.filter_perf = BMI2_PERF_OPT_MODE;
        rslt                       = bmi2_set_sensor_config(&config, 1, dev);
    }
    return rslt;
}

static float lsb_to_dps(int16_t val, float dps, uint8_t bit_width) {
    double power      = 2;
    float  half_scale = (float)((pow((double)power, (double)bit_width) / 2.0f));
    return (dps / (half_scale)) * (val);
}

#define GRAVITY_EARTH (9.80665f)

static int8_t set_accel_config(struct bmi2_dev* bmi) {
    int8_t rslt;

    struct bmi2_sens_config config;
    config.type = BMI2_ACCEL;

    rslt = bmi2_get_sensor_config(&config, 1, bmi);
    bmi2_error_codes_print_result(rslt);

    if (rslt == BMI2_OK) {
        config.cfg.acc.odr         = BMI2_ACC_ODR_200HZ;
        config.cfg.acc.range       = BMI2_ACC_RANGE_2G;
        config.cfg.acc.bwp         = BMI2_ACC_NORMAL_AVG4;
        config.cfg.acc.filter_perf = BMI2_PERF_OPT_MODE;

        rslt = bmi2_set_sensor_config(&config, 1, bmi);
        bmi2_error_codes_print_result(rslt);

        rslt = bmi2_map_data_int(BMI2_DRDY_INT, BMI2_INT1, bmi);
        bmi2_error_codes_print_result(rslt);
    }

    return rslt;
}

static float lsb_to_mps2(int16_t val, float g_range, uint8_t bit_width) {
    double power      = 2;
    float  half_scale = (float)((pow((double)power, (double)bit_width) / 2.0f));
    return (GRAVITY_EARTH * val * g_range) / half_scale;
}

void test_bmi270(void) {
    i2c_master_bus_handle_t handle;
    SemaphoreHandle_t       semaphore;
    bsp_i2c_primary_bus_get_handle(&handle);
    bsp_i2c_primary_bus_get_semaphore(&semaphore);
    bmi2_set_i2c_configuration(handle, 0x68, semaphore);

    int8_t                rslt;
    struct bmi2_dev       bmi;
    struct bmi2_sens_data sensor_data;
    uint8_t               sensor_list[] = {BMI2_GYRO, BMI2_ACCEL};

    memset(&bmi, 0, sizeof(struct bmi2_dev));
    memset(&sensor_data, 0, sizeof(struct bmi2_sens_data));

    printf("Interface init...\r\n");
    rslt = bmi2_interface_init(&bmi, BMI2_I2C_INTF);
    bmi2_error_codes_print_result(rslt);
    if (rslt != BMI2_OK) {
        printf("Failed to initialize interface\r\n");
        return;
    }

    printf("Init...\r\n");
    rslt = bmi270_init(&bmi);
    bmi2_error_codes_print_result(rslt);
    if (rslt != BMI2_OK) {
        printf("Failed to initialize chip\r\n");
        return;
    }

    printf("Config gyro...\r\n");
    rslt = set_gyro_config(&bmi);
    bmi2_error_codes_print_result(rslt);
    if (rslt != BMI2_OK) {
        printf("Failed to set gyro config\r\n");
        return;
    }

    printf("Config accel...\r\n");
    rslt = set_accel_config(&bmi);
    bmi2_error_codes_print_result(rslt);
    if (rslt != BMI2_OK) {
        printf("Failed to set accel config\r\n");
        return;
    }

    printf("Enable sensors...\r\n");
    rslt = bmi2_sensor_enable(sensor_list, sizeof(sensor_list), &bmi);
    bmi2_error_codes_print_result(rslt);
    if (rslt != BMI2_OK) {
        printf("Failed to enable\r\n");
        return;
    }

    while (true) {
        rslt = bmi2_get_sensor_data(&sensor_data, &bmi);
        bmi2_error_codes_print_result(rslt);

        if ((rslt == BMI2_OK) && (sensor_data.status & BMI2_DRDY_GYR) && (sensor_data.status & BMI2_DRDY_ACC)) {
            /* Converting lsb to degree per second for 16 bit gyro at 2000dps range. */
            float gyro_x = lsb_to_dps(sensor_data.gyr.x, (float)2000, bmi.resolution);
            float gyro_y = lsb_to_dps(sensor_data.gyr.y, (float)2000, bmi.resolution);
            float gyro_z = lsb_to_dps(sensor_data.gyr.z, (float)2000, bmi.resolution);

            /* Converting lsb to meter per second squared for 16 bit accelerometer at 2G range. */
            float accel_x = lsb_to_mps2(sensor_data.acc.x, (float)2, bmi.resolution);
            float accel_y = lsb_to_mps2(sensor_data.acc.y, (float)2, bmi.resolution);
            float accel_z = lsb_to_mps2(sensor_data.acc.z, (float)2, bmi.resolution);

            printf("A: %4.2f, %4.2f, %4.2f  G: %4.2f, %4.2f, %4.2f\n", accel_x, accel_y, accel_z, gyro_x, gyro_y,
                   gyro_z);
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
