/*
 * Initial driver for ESP-IDF SHTC3 Temp / Humidity sensor
 *
 * This is loosly based on the Adafruit version of the code
 * available at https://github.com/adafruit/Adafruit_SHTC3
 */

#include <sdkconfig.h>
#if CONFIG_SHTC3_ENABLED

#include <driver/i2c.h>
#include <esp32/rom/ets_sys.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "sdkconfig.h"
#include "shtc3.h"

#define I2C_FREQ_HZ 1000000  // 1MHz
#define G_POLYNOM 0x31

// SHTC3 commands
#define SHTC3_I2C_ADDR 0x70     // Per https://www.adafruit.com/product/4636
#define SHTC3_READID 0xEFC8     // Read ID Register
#define SHTC3_SOFTRESET 0x805D  // Software reset
#define SHTC3_SLEEP 0xB098      // Enter sleep mode
#define SHTC3_WAKEUP 0x3517     // Wakeup mode

// Max wait times, powerup should delayMicroseconds(), measurement should
// vTaskDelay
#define SHTC3_POWERUP_RESET_MAX_TIME_US 240
#define SHTC3_MEASUREMENT_MAX_TIME 13 / portTICK_PERIOD_MS

// Clock stretching commands to read temp / humidity
// HFirst - Humidity First v.s. TFirst for Temp first
#define SHTC3_NORMAL_MEAS_TFIRST_STRETCH 0x7CA2
#define SHTC3_NORMAL_MEAS_HFIRST_STRETCH 0x5C24
#define SHTC3_LOWPOW_MEAS_TFIRST_STRETCH 0x6458
#define SHTC3_LOWPOW_MEAS_HFIRST_STRETCH 0x44DE

// Clock stretching disabled commands to read temp / humidity
#define SHTC3_NORMAL_MEAS_TFIRST 0x7866
#define SHTC3_NORMAL_MEAS_HFIRST 0x58E0
#define SHTC3_LOWPOW_MEAS_TFIRST 0x609C
#define SHTC3_LOWPOW_MEAS_HFIRST 0x401A

static const char *TAG = "shtc3";

static inline uint16_t shuffle(uint16_t val) { return (val >> 8) | (val << 8); }

/**
 * @brief crc8 verifier
 *
 * @param data  Array of data to verify
 * @param len   Length of the array
 * @return      crc checksum of data
 */
static uint8_t crc8(uint8_t data[], int len) {
  // initialization value
  uint8_t crc = 0xff;

  // iterate over all bytes
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    for (int i = 0; i < 8; i++) {
      bool xor = crc & 0x80;
      crc = crc << 1;
      crc = xor? crc ^ G_POLYNOM : crc;
    }
  }
  return crc;
}

/**
 * @brief CRC verifiy raw sensor data
 *
 * @param raw_data    Raw temperature and humidity data w/ CRC checksums
 * @return            `ESP_OK` on success
 *                    `ESP_ERR_INVALID_CRC` if either sensor value fails CRC
 */
static esp_err_t shtc3_check_raw_data(shtc3_raw_data_t raw_data) {
  // check temperature crc
  if (crc8(raw_data, 2) != raw_data[2]) {
    ESP_LOGW(TAG, "CRC check for temperature data failed");
    return ESP_ERR_INVALID_CRC;
  }

  // check humidity crc
  if (crc8(raw_data + 3, 2) != raw_data[5]) {
    ESP_LOGW(TAG, "CRC check for humidity data failed");
    return ESP_ERR_INVALID_CRC;
  }

  return ESP_OK;
}

/**
 * @brief Compute actual humidity and temprature values
 *
 * Pointers to temperature & humidity are optional but AT LEAST ONE must be
 * provided
 *
 * @param raw_data      Raw temperature and humidity data w/ CRC checksums
 * @param temperature   Output value to store temperature in Celsius
 * @param humidity      Output value to store humidity in percent
 * @return              `ESP_OK` on success
 *                      `ESP_ERR_INVALID_ARG` if no outputs specified
 */
static esp_err_t shtc3_compute_values(shtc3_raw_data_t raw_data,
                                      float *temperature, float *humidity) {
  if (!(raw_data && (temperature || humidity))) {
    return ESP_ERR_INVALID_ARG;
  }

  if (temperature)
    *temperature =
        ((((raw_data[0] * 256.0) + raw_data[1]) * 175) / 65535.0) - 45;

  if (humidity)
    *humidity = ((((raw_data[3] * 256.0) + raw_data[4]) * 100) / 65535.0);

  ESP_LOGV(TAG, "temp: %f humidity: %f", *temperature, *humidity);
  return ESP_OK;
}

/**
 * @brief CRC verifiy raw sensor data
 *
 * @param raw_data    Raw temperature and humidity data w/ CRC checksums
 * @return            `ESP_OK` on success
 *                    `ESP_ERR_INVALID_CRC` if either sensor value fails CRC
 */
static esp_err_t shtc3_send_cmd_nolock(i2c_dev_t *dev, uint16_t cmd) {
  cmd = shuffle(cmd);
  ESP_LOGV(TAG, "Sending cmd %02x...", cmd);
  return i2c_dev_write(dev, NULL, 0, &cmd, 2);
}

esp_err_t shtc3_free_desc(i2c_dev_t *dev) {
  if (!dev) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_dev_delete_mutex(dev);
}

esp_err_t shtc3_init(i2c_dev_t *dev) {
  uint16_t id = 0;
  uint8_t data[3];
  uint16_t cmd = shuffle(SHTC3_READID);

  ESP_LOGD(TAG, "init start");
  I2C_DEV_TAKE_MUTEX(dev);

  // Reset --> Wake --> ReadID --> Sleep
  /*I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_SOFTRESET));*/
  I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_WAKEUP));

  ets_delay_us(SHTC3_POWERUP_RESET_MAX_TIME_US);

  I2C_DEV_CHECK(dev, i2c_dev_read(dev, &cmd, 2, data, sizeof(data)));

  id = data[0] << 8;
  id |= data[1];
  id &= 0x083F;
  if (id != 0x807) {
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGD(TAG, "ID [0x%3x = 0x807] Verified, sleeping device", id);
  I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_SLEEP));
  I2C_DEV_GIVE_MUTEX(dev);
  ESP_LOGI(TAG, "init complete");

  return ESP_OK;
}

esp_err_t shtc3_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio,
                          gpio_num_t scl_gpio) {
  if (dev == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  dev->port = port;
  dev->addr = SHTC3_I2C_ADDR;
  dev->cfg.sda_io_num = sda_gpio;
  dev->cfg.scl_io_num = scl_gpio;
  dev->cfg.master.clk_speed = I2C_FREQ_HZ;
  return i2c_dev_create_mutex(dev);
}

esp_err_t shtc3_measure(i2c_dev_t *dev, float *temperature, float *humidity) {
  if (!(dev && (temperature || humidity))) {
    return ESP_ERR_INVALID_ARG;
  }

  shtc3_raw_data_t raw_data;

  // Wake --> wait --> Measure --> delay --> read --> sleep
  I2C_DEV_TAKE_MUTEX(dev);
  ESP_LOGV(TAG, "Wakeup device");
  I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_WAKEUP));
  ets_delay_us(SHTC3_POWERUP_RESET_MAX_TIME_US);

  ESP_LOGV(TAG, "Begin measurement...");
  I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_NORMAL_MEAS_TFIRST));
  vTaskDelay(SHTC3_MEASUREMENT_MAX_TIME * 2);  // bad timing?

  ESP_LOGV(TAG, "Read measurement...");
  I2C_DEV_CHECK(dev,
                i2c_dev_read(dev, NULL, 0, raw_data, sizeof(shtc3_raw_data_t)));

  I2C_DEV_CHECK(dev, shtc3_send_cmd_nolock(dev, SHTC3_SLEEP));
  I2C_DEV_GIVE_MUTEX(dev);

  ESP_LOGV(TAG, "Verify measurement...");
  esp_err_t x = shtc3_check_raw_data(raw_data);
  if (x != ESP_OK) {
    return x;
  }

  return shtc3_compute_values(raw_data, temperature, humidity);
}

#endif
