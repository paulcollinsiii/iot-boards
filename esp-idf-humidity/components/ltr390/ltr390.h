/*
 * ESP-IDF LTR390 Ambient / UV Light Sensor
 *
 * Heavily based on https://github.com/UncleRus/esp-idf-lib examples
 */

#include <sdkconfig.h>
#if CONFIG_LTR390_ENABLED

#ifndef ESP_LTR390_H
#define ESP_LTR390_H

#include <esp_err.h>
#include <i2cdev.h>
#include <stdbool.h>

#include "modules/ltr390.pb-c.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ltr390* enums are from protobuf defs and reused here to keep type consistency
 * while this isn't exactly ideal, it makes the command interface a LOT easier
 * and gives the dry-ness I'm looking for here
 */

const char *ltr390_gain_to_str(Ltr390__GainT gain);
const char *ltr390_mode_to_str(Ltr390__ModeT mode);
const char *ltr390_resolution_to_str(Ltr390__ResolutionT res);
const char *ltr390_measurerate_to_str(Ltr390__MeasurerateT res);

/**
 * @brief Initialize device descriptor
 *
 * @param dev       Device descriptor
 * @param port      I2C port
 * @param sda_gpio  SDA GPIO
 * @param scl_gpio  SCL GPIO
 * @return          `ESP_OK` on success
 */
esp_err_t ltr390_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio,
                           gpio_num_t scl_gpio);

/**
 * @brief Initialize sensor
 *
 * @param dev       Device descriptor
 * @return          `ESP_OK` on success
 */
esp_err_t ltr390_init(i2c_dev_t *dev);

/**
 * @brief Enable the sensor (start measuring)
 *
 * @param dev         Device descriptor
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_enable(i2c_dev_t *dev);

/**
 * @brief Free device descriptor
 *
 * @param dev       Device descriptor
 * @return          `ESP_OK` on success
 */
esp_err_t ltr390_free_desc(i2c_dev_t *dev);

/**
 * @brief Get the current cached settings for the device
 *
 * @param out_enabled       Out ptr for if sensor is enabled
 * @param out_gain          Out ptr for gain
 * @param out_measurerate   Out ptr for measurement rate
 * @param out_mode          Out ptr for mode
 * @param out_resolution    Out ptr for resolution
 */
void ltr390_get_cached_state(bool *out_enabled, Ltr390__GainT *out_gain,
                             Ltr390__MeasurerateT *out_measurerate,
                             Ltr390__ModeT *out_mode,
                             Ltr390__ResolutionT *out_resolution);

/**
 * @brief Get the gain of the sensor
 *
 * @param dev         Device descriptor
 * @param out_gain    Where to write the value
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_get_gain(i2c_dev_t *dev, Ltr390__GainT *out_gain);

/**
 * @brief Measure whichever sensor is enabled
 *
 * This will measure either the ALS or UVS, and set the int pointer of the
 * enabled sensor while setting the other to -1
 *
 * @param dev             Device descriptor
 * @param measurement     Output value to store the measurement in
 * @param out_mode        Output value to out_mode in, which determins if the
 *                          measurement is in Lux or UVI
 * @return                `ESP_OK` on success
 *                        `ESP_ERR_INVALID_STATE` if no new data available
 *                        `ESP_ERR_FAIL` Mode setting is invalid or sensor not
 *                          enabled
 */
esp_err_t ltr390_measure(i2c_dev_t *dev, float *measurement,
                         Ltr390__ModeT *out_mode);

/**
 * @brief Set the gain of the sensor
 *
 * @param dev         Device descriptor
 * @param gain        Gain setting to set sensor to
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_set_gain(i2c_dev_t *dev, Ltr390__GainT gain);

/**
 * @brief Get the mode of the sensor (ALS / UVS)
 *
 * @param dev         Device descriptor
 * @param out_mode    Pointer to store the current mode in
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_get_mode(i2c_dev_t *dev, Ltr390__ModeT *out_mode);

/**
 * @brief Set the mode of the sensor
 *
 * @param dev         Device descriptor
 * @param mode        ALS / UVS mode
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_set_mode(i2c_dev_t *dev, Ltr390__ModeT mode);

/**
 * @brief Get the resolution and measurement rate of the sensor
 *
 * @param dev         Device descriptor
 * @param out_resolution Pointer to store the current resolution in
 * @param out_rate       Pointer to store the current resolution in
 * @return               `ESP_OK` on success
 */
esp_err_t ltr390_get_resolution(i2c_dev_t *dev, Ltr390__MeasurerateT *out_rate,
                                Ltr390__ResolutionT *out_resolution);

/**
 * @brief Set the resolution and measurement rate of the sensor
 *
 * @param dev         Device descriptor
 * @param rate        Measurement rate to set
 * @param resolution  Measurement resolution, higher res requires slower
 *                      measurements
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_set_resolution(i2c_dev_t *dev, Ltr390__MeasurerateT rate,
                                Ltr390__ResolutionT resolution);

/**
 * @brief Set the sensor mode (ALS / UVS)
 *
 * @param dev         Device descriptor
 * @param mode        ALS / UVS
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_set_mode(i2c_dev_t *dev, Ltr390__ModeT mode);

/**
 * @brief Set the sensor to standby
 *
 * @param dev         Device descriptor
 * @return            `ESP_OK` on success
 */
esp_err_t ltr390_standby(i2c_dev_t *dev);

#ifdef __cplusplus
}
#endif
#endif
#endif  // CONFIG_LTR390_ENABLED
