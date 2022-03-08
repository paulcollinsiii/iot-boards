/*
 * Initial driver for ESP-IDF SHTC3 Temp / Humidity sensor
 *
 * This is loosly based on the Adafruit version of the code
 * available at https://github.com/adafruit/Adafruit_SHTC3
 */

#include <sdkconfig.h>
#if CONFIG_SHT4X_ENABLED

#include <driver/i2c.h>
#include <esp32/rom/ets_sys.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "sdkconfig.h"
#include "sht4x.h"

#define I2C_FREQ_HZ 1000000  // 1MHz
#define G_POLYNOM 0x31
#define SHT4X_DATASIZE 6

#define SHT4x_DEFAULT_ADDR 0x44
#define SHT4x_NOHEAT_HIGHPRECISION 0xFD
#define SHT4x_NOHEAT_MEDPRECISION 0xF6
#define SHT4x_NOHEAT_LOWPRECISION 0xE0

// High precision measurement, high / med / low heat for specified time
#define SHT4x_HIGHHEAT_1S 0x39
#define SHT4x_HIGHHEAT_100MS 0x32
#define SHT4x_MEDHEAT_1S 0x2F
#define SHT4x_MEDHEAT_100MS 0x24
#define SHT4x_LOWHEAT_1S 0x1E
#define SHT4x_LOWHEAT_100MS 0x15

#define SHT4x_READSERIAL 0x89
#define SHT4x_SOFTRESET 0x94

static const char *TAG = "sht4x";

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

static esp_err_t sht4x_get_delay(Sht4x__ModeT mode, u_int8_t *cmd,
                                 u_int16_t *delay) {
  switch (mode) {
    case SHT4X__MODE_T__HIGH_HEATER_1S:
      *delay = 1015;
      *cmd = SHT4x_HIGHHEAT_1S;
      break;
    case SHT4X__MODE_T__MED_HEATER_1S:
      *delay = 1015;
      *cmd = SHT4x_MEDHEAT_1S;
      break;
    case SHT4X__MODE_T__LOW_HEATER_1S:
      *delay = 1015;
      *cmd = SHT4x_LOWHEAT_1S;
      break;
    case SHT4X__MODE_T__HIGH_HEATER_100MS:
      *delay = 115;
      *cmd = SHT4x_HIGHHEAT_100MS;
      break;
    case SHT4X__MODE_T__MED_HEATER_100MS:
      *delay = 115;
      *cmd = SHT4x_MEDHEAT_100MS;
      break;
    case SHT4X__MODE_T__LOW_HEATER_100MS:
      *delay = 115;
      *cmd = SHT4x_LOWHEAT_100MS;
      break;
    case SHT4X__MODE_T__NO_HEATER_HIGH:
      *delay = 10;
      *cmd = SHT4x_NOHEAT_HIGHPRECISION;
      break;
    case SHT4X__MODE_T__NO_HEATER_MED:
      *delay = 7;
      *cmd = SHT4x_NOHEAT_MEDPRECISION;
      break;
    case SHT4X__MODE_T__NO_HEATER_LOW:
      *delay = 3;
      *cmd = SHT4x_NOHEAT_LOWPRECISION;
      break;
    default:
      ESP_LOGE(TAG, "Unknown mode, returning max delay for reading...");
      *cmd = SHT4x_NOHEAT_HIGHPRECISION;
      *delay = 10;
      return ESP_FAIL;
  }

  return ESP_OK;
}

static esp_err_t sht4x_send_cmd_nolock(i2c_dev_t *dev, uint8_t cmd) {
  ESP_LOGV(TAG, "Sending cmd %02x...", cmd);
  return i2c_dev_write(dev, NULL, 0, &cmd, 1);
}

static esp_err_t sht4x_read_res_nolock(i2c_dev_t *dev, uint8_t res[]) {
  esp_err_t ret = i2c_dev_read(dev, NULL, 0, res, SHT4X_DATASIZE);
  if (ESP_OK != ret) return ret;

  if (res[2] != crc8(res, 2) || res[5] != crc8(res + 3, 2)) {
    ESP_LOGE(TAG, "Invalid CRC");
    return ESP_ERR_INVALID_CRC;
  }

  return ESP_OK;
}

const char *sht4x_mode_to_str(Sht4x__ModeT mode) {
  switch (mode) {
    case SHT4X__MODE_T__HIGH_HEATER_1S:
      return "SHT4X__MODE_T__HIGH_HEATER_1S";
    case SHT4X__MODE_T__MED_HEATER_1S:
      return "SHT4X__MODE_T__MED_HEATER_1S";
    case SHT4X__MODE_T__LOW_HEATER_1S:
      return "SHT4X__MODE_T__LOW_HEATER_1S";
    case SHT4X__MODE_T__HIGH_HEATER_100MS:
      return "SHT4X__MODE_T__HIGH_HEATER_100MS";
    case SHT4X__MODE_T__MED_HEATER_100MS:
      return "SHT4X__MODE_T__MED_HEATER_100MS";
    case SHT4X__MODE_T__LOW_HEATER_100MS:
      return "SHT4X__MODE_T__LOW_HEATER_100MS";
    case SHT4X__MODE_T__NO_HEATER_HIGH:
      return "SHT4X__MODE_T__NO_HEATER_HIGH";
    case SHT4X__MODE_T__NO_HEATER_MED:
      return "SHT4X__MODE_T__NO_HEATER_MED";
    case SHT4X__MODE_T__NO_HEATER_LOW:
      return "SHT4X__MODE_T__NO_HEATER_LOW";
    default:
      return "UNKNOW MODE";
  }
}

esp_err_t sht4x_free_desc(i2c_dev_t *dev) {
  if (!dev) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_dev_delete_mutex(dev);
}

esp_err_t sht4x_init(i2c_dev_t *dev) {
  uint32_t id = 0;
  uint8_t data[6];

  ESP_LOGD(TAG, "init start");
  I2C_DEV_TAKE_MUTEX(dev);

  ESP_LOGV(TAG, "reset");
  I2C_DEV_CHECK(dev, sht4x_send_cmd_nolock(dev, SHT4x_SOFTRESET));
  vTaskDelay(10 / portTICK_PERIOD_MS);
  ESP_LOGV(TAG, "get serial");
  I2C_DEV_CHECK(dev, sht4x_send_cmd_nolock(dev, SHT4x_READSERIAL));
  vTaskDelay(10 / portTICK_PERIOD_MS);
  I2C_DEV_CHECK(dev, i2c_dev_read(dev, NULL, 0, data, sizeof(data)));

  if (crc8(data, 2) != data[2] || crc8(data + 3, 2) != data[5]) {
    ESP_LOGW(TAG, "Failed CRC check on serial number");
    return ESP_ERR_INVALID_CRC;
  }

  id = data[0];
  id <<= 8;
  id |= data[1];
  id <<= 8;
  id |= data[3];
  id <<= 8;
  id |= data[4];

  ESP_LOGD(TAG, "SHT4X Serial: 0x%4x", id);
  I2C_DEV_GIVE_MUTEX(dev);
  ESP_LOGI(TAG, "init complete");

  return ESP_OK;
}

esp_err_t sht4x_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio,
                          gpio_num_t scl_gpio) {
  if (dev == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  dev->port = port;
  dev->addr = SHT4x_DEFAULT_ADDR;
  dev->cfg.sda_io_num = sda_gpio;
  dev->cfg.scl_io_num = scl_gpio;
  dev->cfg.master.clk_speed = I2C_FREQ_HZ;
  return i2c_dev_create_mutex(dev);
}

esp_err_t sht4x_measure(i2c_dev_t *dev, Sht4x__ModeT mode, float *temperature,
                        float *humidity) {
  uint8_t cmd, data[6];
  uint16_t delay;

  if (ESP_OK != sht4x_get_delay(mode, &cmd, &delay)) {
    // Don't measure, and wait for the mode to get fixed
    return ESP_FAIL;
  }

  I2C_DEV_TAKE_MUTEX(dev);
  ESP_LOGV(TAG, "measuring...");

  I2C_DEV_CHECK(dev, sht4x_send_cmd_nolock(dev, cmd));
  vTaskDelay(delay / portTICK_PERIOD_MS);
  I2C_DEV_CHECK(dev, sht4x_read_res_nolock(dev, data));
  I2C_DEV_GIVE_MUTEX(dev);

  *temperature = ((uint16_t)data[0] << 8 | data[1]) * 175.0 / 65535.0 - 45.0;
  *humidity = ((uint16_t)data[3] << 8 | data[4]) * 125.0 / 65535.0 - 6.0;

  ESP_LOGV(TAG, "temp: %f\nhumidity: %f", *temperature, *humidity);

  return ESP_OK;
}

#endif
