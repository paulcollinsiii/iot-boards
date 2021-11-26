#ifndef SENSOR_H
#define SENSOR_H

#include <cJSON.h>
#include <esp_event.h>
#include <time.h>

typedef struct {
  float temp;
  float humidity;
  time_t timestamp;
} sensor_data_t;

/**
 * @brief Dump sensor data struct into JSON
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t sensor_jsonify(cJSON *root);
esp_err_t sensor_start();
esp_err_t sensor_stop();
esp_err_t sensor_init();

#endif
