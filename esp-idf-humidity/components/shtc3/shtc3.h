/*
 * ESP-IDF SHTC3 Temp / Humidity sensor
 *
 * Heavily based on https://github.com/UncleRus/esp-idf-lib examples
 */

#include <sdkconfig.h>
#if CONFIG_SHTC3_ENABLED

#ifndef ESP_SHTC3_H
#define ESP_SHTC3_H

#include <esp_err.h>
#include <i2cdev.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHTC3_RAW_DATA_SIZE 6

typedef uint8_t shtc3_raw_data_t[SHTC3_RAW_DATA_SIZE];

/**
 * @brief Initialize device descriptor
 *
 * @param dev       Device descriptor
 * @param port      I2C port
 * @param sda_gpio  SDA GPIO
 * @param scl_gpio  SCL GPIO
 * @return          `ESP_OK` on success
 */
esp_err_t shtc3_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio,
                          gpio_num_t scl_gpio);

/**
 * @brief Initialize sensor
 *
 * @param dev       Device descriptor
 * @return          `ESP_OK` on success
 */
esp_err_t shtc3_init(i2c_dev_t *dev);

/**
 * @brief Free device descriptor
 *
 * @param dev       Device descriptor
 * @return          `ESP_OK` on success
 */
esp_err_t shtc3_free_desc(i2c_dev_t *dev);

/**
 * @brief High level measurement function
 *
 * Wake -> Measure -> Sleep in one shot with delays as needed to obtain
 * measurements
 *
 * @param dev         Device descriptor
 * @param temperature Output value to store temperature in Celsius
 * @param humidity    Output value to store humidity in percent
 * @return            `ESP_OK` on success
 */
esp_err_t shtc3_measure(i2c_dev_t *dev, float *temperature, float *humidity);

#ifdef __cplusplus
}
#endif
#endif
#endif
