#include <sdkconfig.h>
#if CONFIG_LTR390_ENABLED

/*
 * Initial driver for ESP-IDF LTR390 Temp / Humidity sensor
 *
 * This is loosly based on the Adafruit version of the code
 * available at https://github.com/adafruit/Adafruit_LTR390
 */

#include <driver/i2c.h>
#include <esp32/rom/ets_sys.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "ltr390.h"
#include "sdkconfig.h"

// TODO: This should be in the project config, and limited to the enabled
// modules max speed
#define I2C_FREQ_HZ 1000000  // 1MHz

#define LTR390_I2CADDR_DEFAULT 0x53  ///< I2C address
#define LTR390_MAIN_CTRL 0x00        ///< Main control register
#define LTR390_MEAS_RATE 0x04        ///< Resolution and data rate
#define LTR390_GAIN 0x05             ///< ALS and UVS gain range
#define LTR390_PART_ID 0x06          ///< Part id/revision register
#define LTR390_MAIN_STATUS 0x07      ///< Main status register
#define LTR390_ALSDATA 0x0D          ///< ALS data lowest byte
#define LTR390_UVSDATA 0x10          ///< UVS data lowest byte
#define LTR390_INT_CFG 0x19          ///< Interrupt configuration
#define LTR390_INT_PST 0x1A          ///< Interrupt persistance config
#define LTR390_THRESH_UP 0x21        ///< Upper threshold, low byte
#define LTR390_THRESH_LOW 0x24       ///< Lower threshold, low byte

static const char *TAG = "ltr390";

typedef struct {
  bool enabled;
  Ltr390__GainT gain;
  Ltr390__ModeT mode;
  Ltr390__MeasurerateT measurerate;
  Ltr390__ResolutionT resolution;
} state_t;

static state_t state;

static esp_err_t ltr390_read_reg_nolock(i2c_dev_t *dev, uint8_t reg,
                                        uint8_t *in_data) {
  ESP_LOGV(TAG, "reading register %02x...", reg);
  return i2c_dev_read_reg(dev, reg, in_data, sizeof(uint8_t));
}

static esp_err_t ltr390_write_reg_nolock(i2c_dev_t *dev, uint8_t reg,
                                         uint8_t data) {
  ESP_LOGV(TAG, "Write REG:DATA (0x%02x:0x%02x)", reg, data);
  return i2c_dev_write_reg(dev, reg, &data, sizeof(uint8_t));
}

const char *ltr390_gain_to_str(Ltr390__GainT gain) {
  switch (gain) {
    case LTR390__GAIN_T__GAIN_1:
      return "LTR390_GAIN_1";
    case LTR390__GAIN_T__GAIN_3:
      return "LTR390_GAIN_3";
    case LTR390__GAIN_T__GAIN_6:
      return "LTR390_GAIN_6";
    case LTR390__GAIN_T__GAIN_9:
      return "LTR390_GAIN_9";
    case LTR390__GAIN_T__GAIN_18:
      return "LTR390_GAIN_18";
    default:
      return "UNKNOWN!";
  }
}

const char *ltr390_mode_to_str(Ltr390__ModeT mode) {
  switch (mode) {
    case LTR390__MODE_T__ALS:
      return "LTR390_ALS";
    case LTR390__MODE_T__UVS:
      return "LTR390_UVS";
    default:
      return "UNKNOWN!";
  }
}

const char *ltr390_resolution_to_str(Ltr390__ResolutionT res) {
  switch (res) {
    case LTR390__RESOLUTION_T__RESOLUTION_20BIT:
      return "LTR390_RESOLUTION_20BIT";
    case LTR390__RESOLUTION_T__RESOLUTION_19BIT:
      return "LTR390_RESOLUTION_19BIT";
    case LTR390__RESOLUTION_T__RESOLUTION_18BIT:
      return "LTR390_RESOLUTION_18BIT";
    case LTR390__RESOLUTION_T__RESOLUTION_17BIT:
      return "LTR390_RESOLUTION_17BIT";
    case LTR390__RESOLUTION_T__RESOLUTION_16BIT:
      return "LTR390_RESOLUTION_16BIT";
    case LTR390__RESOLUTION_T__RESOLUTION_13BIT:
      return "LTR390_RESOLUTION_13BIT";
    default:
      return "UNKNOWN!";
  }
}

const char *ltr390_measurerate_to_str(Ltr390__MeasurerateT res) {
  switch (res) {
    case LTR390__MEASURERATE_T__MEASURE_25MS:
      return "LTR390_MEASURE_25MS";
    case LTR390__MEASURERATE_T__MEASURE_50MS:
      return "LTR390_MEASURE_50MS";
    case LTR390__MEASURERATE_T__MEASURE_100MS:
      return "LTR390_MEASURE_100MS";
    case LTR390__MEASURERATE_T__MEASURE_200MS:
      return "LTR390_MEASURE_200MS";
    case LTR390__MEASURERATE_T__MEASURE_500MS:
      return "LTR390_MEASURE_500MS";
    case LTR390__MEASURERATE_T__MEASURE_1000MS:
      return "LTR390_MEASURE_1000MS";
    case LTR390__MEASURERATE_T__MEASURE_2000MS:
      return "LTR390_MEASURE_2000MS";
    default:
      return "UNKNOWN!";
  }
}

/**
 * @brief Read sensor registers
 *
 * This just returns the raw sensor data as the unsigned int
 *
 * @param dev         Device descriptor
 * @param out         Where to write the value
 * @param reg_addr    Which register to start reading 3 bytes from
 * @return            `ESP_OK` on success
 *                    `ESP_ERR_INVALID_STATE` if no new data available
 *                    `ESP_ERR_FAIL` if sensor is in incorrect mode (e.g. read
 *                        ALS when measuring UV)
 */
static esp_err_t ltr390_read(i2c_dev_t *dev, uint32_t *out, uint8_t reg_addr) {
  int mask;
  uint8_t reg;
  uint8_t buf[3];

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MAIN_STATUS, &reg);
  if (!(reg & 0x8)) {  // No data available
    I2C_DEV_GIVE_MUTEX(dev);
    return ESP_ERR_INVALID_STATE;
  }

  i2c_dev_read_reg(dev, reg_addr, &buf, 3 * sizeof(uint8_t));
  I2C_DEV_GIVE_MUTEX(dev);

  // LTR390 has LSB first
  // yes I'm re-using reg here as loop ctr. Reg isn't needed after the "New
  // data" check
  for (reg = 0; reg < 3; reg++) {
    *out <<= 8;
    *out |= buf[3 - reg - 1];
  }

  switch (state.resolution) {
    case LTR390__RESOLUTION_T__RESOLUTION_20BIT:
      mask = 0xFFF00000;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_19BIT:
      mask = 0xFFF80000;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_18BIT:
      mask = 0xFFFC0000;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_17BIT:
      mask = 0xFFFE0000;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_16BIT:
      mask = 0xFFFF0000;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_13BIT:
      mask = 0xFFFFE000;
      break;
    default:
      ESP_LOGE(TAG, "Unknown resolution set! Aborting!");
      abort();
  }

  *out &= ~mask;  // Sanitize output

  return ESP_OK;
}

/**
 * @brief Read the Ambient Light Sensor data
 *
 * @param dev         Device descriptor
 * @param out_als     Where to write the value
 * @return            `ESP_OK` on success
 *                    `ESP_ERR_INVALID_STATE` if no new data available
 *                    `ESP_ERR_FAIL` if sensor is in incorrect mode (e.g. read
 *                        ALS when measuring UV)
 */
static esp_err_t ltr390_read_als(i2c_dev_t *dev, float *out_als) {
  float gain_multi;
  uint32_t als_data = 0;

  if (state.mode != LTR390__MODE_T__ALS || !state.enabled) {
    return ESP_FAIL;
  }
  esp_err_t ret = ltr390_read(dev, &als_data, LTR390_ALSDATA);
  if (ret != ESP_OK) {
    *out_als = -1;
    return ret;
  }

  // Calc lux
  switch (state.gain) {
    case LTR390__GAIN_T__GAIN_1:
      gain_multi = 1;
      break;
    case LTR390__GAIN_T__GAIN_3:
      gain_multi = 3;
      break;
    case LTR390__GAIN_T__GAIN_6:
      gain_multi = 6;
      break;
    case LTR390__GAIN_T__GAIN_9:
      gain_multi = 9;
      break;
    case LTR390__GAIN_T__GAIN_18:
      gain_multi = 18;
      break;
    default:
      ESP_LOGE(TAG, "Unknown gain! Aborting!");
      abort();
  }

  switch (state.resolution) {
    case LTR390__RESOLUTION_T__RESOLUTION_20BIT:
      gain_multi *= 4.0;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_19BIT:
      gain_multi *= 2.0;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_18BIT:
      gain_multi *= 1.0;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_17BIT:
      gain_multi *= 0.5;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_16BIT:
      gain_multi *= 0.25;
      break;
    case LTR390__RESOLUTION_T__RESOLUTION_13BIT:
      gain_multi *= 0.25;
      break;
    default:
      ESP_LOGE(TAG, "Unknown resolution set! Aborting!");
      abort();
  }

  *out_als = (0.6 * als_data) / gain_multi;
  ESP_LOGV(TAG, "Lux: %f", *out_als);
  return ret;
}

/**
 * @brief Read the UV Light Sensor data
 *
 * @param dev         Device descriptor
 * @param out_uvs     Where to write the value
 * @return            `ESP_OK` on success
 *                    `ESP_ERR_INVALID_STATE` if no new data available
 *                    `ESP_ERR_FAIL` if sensor is in incorrect mode (e.g. read
 *                        ALS when measuring UV)
 */
static esp_err_t ltr390_read_uvs(i2c_dev_t *dev, float *out_uvs) {
  uint32_t uvs_data = 0;

  if (state.mode != LTR390__MODE_T__UVS || !state.enabled) {
    return ESP_FAIL;
  }

  esp_err_t ret = ltr390_read(dev, &uvs_data, LTR390_UVSDATA);

  // Calc UVI
  *out_uvs = uvs_data / 2300.0;
  return ret;
}

esp_err_t ltr390_init_desc(i2c_dev_t *dev, i2c_port_t port, gpio_num_t sda_gpio,
                           gpio_num_t scl_gpio) {
  if (dev == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  dev->port = port;
  dev->addr = LTR390_I2CADDR_DEFAULT;
  dev->cfg.sda_io_num = sda_gpio;
  dev->cfg.scl_io_num = scl_gpio;
  dev->cfg.master.clk_speed = I2C_FREQ_HZ;
  return i2c_dev_create_mutex(dev);
}

esp_err_t ltr390_init(i2c_dev_t *dev) {
  uint8_t reg;

  ESP_LOGD(TAG, "init start");
  I2C_DEV_TAKE_MUTEX(dev);

  // Check ID --> Soft Reset --> Enable
  I2C_DEV_CHECK(dev, ltr390_read_reg_nolock(dev, LTR390_PART_ID, &reg));
  reg >>= 4;
  if (reg != 0xB) {
    ESP_LOGE(TAG, "DEVICE:ID (0x%2x:0x%1x) != 0x%2x:0xB Not Verified",
             LTR390_I2CADDR_DEFAULT, reg, LTR390_I2CADDR_DEFAULT);
    I2C_DEV_GIVE_MUTEX(dev);
    return ESP_FAIL;
  }

  ESP_LOGD(TAG, "DEVICE:ID (0x%2x:0x%1x) = 0x%2x:0xB Verified",
           LTR390_I2CADDR_DEFAULT, reg, LTR390_I2CADDR_DEFAULT);

  // Resetting the device apparently resets before ack
  I2C_DEV_CHECK_LOGE(dev,
                     ltr390_write_reg_nolock(dev, LTR390_MAIN_CTRL, 1 << 4),
                     "reset write failed");

  // Delay 10ms at least before re-reading the register
  vTaskDelay(10 / portTICK_PERIOD_MS);
  I2C_DEV_CHECK_LOGE(dev, ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, &reg),
                     "reset read failed");
  if (((reg >> 4) & 0x01) == 0x01) {  // Reset failed?
    I2C_DEV_GIVE_MUTEX(dev);
    return ESP_FAIL;
  }
  ESP_LOGD(TAG, "soft reset complete");

  // Read current state from sensor to init our state
  ltr390_read_reg_nolock(dev, LTR390_GAIN, &reg);
  state.gain = reg & 0x7;  // Gain is in the lower 3 bits

  ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, &reg);
  state.mode = (reg >> 3) & 0x1;

  ltr390_read_reg_nolock(dev, LTR390_MEAS_RATE, &reg);
  state.resolution = (reg >> 4) & 0x7;
  state.measurerate = reg & 0x7;  // Rate is lower 3 bits

  ESP_LOGD(TAG, "gain %s", ltr390_gain_to_str(state.gain));
  ESP_LOGD(TAG, "mode %s", ltr390_mode_to_str(state.mode));
  ESP_LOGD(TAG, "rate %s", ltr390_measurerate_to_str(state.measurerate));
  ESP_LOGD(TAG, "resolution %s", ltr390_resolution_to_str(state.resolution));

  I2C_DEV_GIVE_MUTEX(dev);

  ESP_LOGI(TAG, "init complete");

  // TODO: Would be super cool to get interrupt based handling for this in here.
  // requires extra wiring though, since I2C doesn't do interrupts really.
  // https://learn.adafruit.com/adafruit-ltr390-uv-sensor/pinouts-2 see INT pin

  return ESP_OK;
}

esp_err_t ltr390_enable(i2c_dev_t *dev) {
  uint8_t reg;

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, &reg);
  reg |= 0x2;
  ltr390_write_reg_nolock(dev, LTR390_MAIN_CTRL, reg);
  I2C_DEV_GIVE_MUTEX(dev);

  state.enabled = true;

  return ESP_OK;
}

esp_err_t ltr390_get_gain(i2c_dev_t *dev, Ltr390__GainT *out_gain) {
  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_GAIN, (uint8_t *)out_gain);
  I2C_DEV_GIVE_MUTEX(dev);

  *out_gain &= 0x7;  // Don't expose reserved bits
  return ESP_OK;
}

esp_err_t ltr390_get_mode(i2c_dev_t *dev, Ltr390__ModeT *out_mode) {
  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, (uint8_t *)out_mode);
  I2C_DEV_GIVE_MUTEX(dev);
  *out_mode = (*out_mode >> 3) & 0x1;
  return ESP_OK;
}

esp_err_t ltr390_get_resolution(i2c_dev_t *dev, Ltr390__MeasurerateT *out_rate,
                                Ltr390__ResolutionT *out_resolution) {
  uint8_t reg;

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MEAS_RATE, &reg);
  I2C_DEV_GIVE_MUTEX(dev);

  *out_resolution = ((reg >> 4) & 0x7);
  *out_rate = (0x0F & reg);
  return ESP_OK;
}

esp_err_t ltr390_measure(i2c_dev_t *dev, float *measurement,
                         Ltr390__ModeT *mode) {
  if (!state.enabled) {
    return ESP_ERR_INVALID_STATE;
  }

  *mode = state.mode;
  switch (state.mode) {
    case LTR390__MODE_T__ALS:
      return ltr390_read_als(dev, measurement);
    case LTR390__MODE_T__UVS:
      return ltr390_read_uvs(dev, measurement);
    default:
      return ESP_FAIL;
  }
}

esp_err_t ltr390_set_gain(i2c_dev_t *dev, Ltr390__GainT gain) {
  uint8_t reg;  // Don't wreck the rest of the register

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_GAIN, &reg);
  reg &= 0xF8;  // gain is in lower 3 bits
  reg |= gain;
  ltr390_write_reg_nolock(dev, LTR390_GAIN, reg);
  I2C_DEV_GIVE_MUTEX(dev);

  return ESP_OK;
}

esp_err_t ltr390_set_resolution(i2c_dev_t *dev, Ltr390__MeasurerateT rate,
                                Ltr390__ResolutionT resolution) {
  uint8_t reg = resolution << 4;
  reg |= rate;

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_write_reg_nolock(dev, LTR390_MEAS_RATE, reg);
  I2C_DEV_GIVE_MUTEX(dev);

  state.measurerate = rate;
  state.resolution = resolution;

  return ESP_OK;
}

esp_err_t ltr390_set_mode(i2c_dev_t *dev, Ltr390__ModeT mode) {
  uint8_t reg;

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, &reg);
  reg &= 0xF7;
  reg |= mode << 3;
  ltr390_write_reg_nolock(dev, LTR390_MAIN_CTRL, reg);
  I2C_DEV_GIVE_MUTEX(dev);

  state.mode = mode;

  return ESP_OK;
}

esp_err_t ltr390_standby(i2c_dev_t *dev) {
  uint8_t reg;

  I2C_DEV_TAKE_MUTEX(dev);
  ltr390_read_reg_nolock(dev, LTR390_MAIN_CTRL, &reg);
  reg &= ~(0x2);  // Clear just the 2nd bit
  ltr390_write_reg_nolock(dev, LTR390_MAIN_CTRL, reg);
  I2C_DEV_GIVE_MUTEX(dev);

  state.enabled = false;

  return ESP_OK;
}

esp_err_t ltr390_free_desc(i2c_dev_t *dev) {
  if (!dev) {
    return ESP_ERR_INVALID_ARG;
  }
  return i2c_dev_delete_mutex(dev);
}

void ltr390_get_cached_state(bool *out_enabled, Ltr390__GainT *out_gain,
                             Ltr390__MeasurerateT *out_measurerate,
                             Ltr390__ModeT *out_mode,
                             Ltr390__ResolutionT *out_resolution) {
  *out_enabled = state.enabled;
  *out_gain = state.gain;
  *out_measurerate = state.measurerate;
  *out_mode = state.mode;
  *out_resolution = state.resolution;
}

#endif
