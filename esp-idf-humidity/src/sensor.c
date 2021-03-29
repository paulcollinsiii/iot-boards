#include "sensor.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <mqtt.h>
#include <shtc3.h>

#include "freertos/ringbuf.h"

#define SENSOR_TASK_NAME "sensor"
#define SENSOR_TASK_STACKSIZE 2 * 1024
#define SENSOR_RINBUFFER_MAXITEMS 60 * 60

static const char *TAG = "sensor";
static TaskHandle_t sensorHandle = NULL;
static RingbufHandle_t buf_handle = NULL;
static struct state {
  bool initilized;
  i2c_dev_t humidity_sensor;
} state = {.initilized = false};

static void sensor_task(void *pvParam) {
  UBaseType_t res;
  sensor_data_t data;

  ESP_LOGD(TAG, "sensor task entering loop");
  UBaseType_t rb_count;
  for (;;) {
    time(&data.timestamp);
    ESP_ERROR_CHECK(
        shtc3_measure(&state.humidity_sensor, &data.temp, &data.humidity));

    res = xRingbufferSend(buf_handle, &data, sizeof(data), 0);
    if (res != pdTRUE) {
      ESP_LOGW(TAG, "Failed to write sensor data to ringbuffer");
    }

    ESP_LOGD(TAG, "Data written to ringbuffer");
    vRingbufferGetInfo(buf_handle, NULL, NULL, NULL, NULL, &rb_count);
    if (rb_count % 5 == 0) {
      xTaskNotifyGive(mqtt_task_handle);
      ESP_LOGI(TAG, "buffered readings: %d", rb_count);
    }
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

esp_err_t sensor_init() {
  shtc3_init_desc(&state.humidity_sensor, I2C_NUM_1, GPIO_NUM_23, GPIO_NUM_22);
  shtc3_init(&state.humidity_sensor);

  buf_handle = xRingbufferCreateNoSplit(sizeof(sensor_data_t),
                                        SENSOR_RINBUFFER_MAXITEMS);
  if (buf_handle == NULL) {
    ESP_LOGE(TAG, "Failed to create ring buffer");
    return ESP_FAIL;
  }

  state.initilized = true;

  return ESP_OK;
}

esp_err_t sensor_start() {
  BaseType_t result;

  if (!state.initilized) {
    ESP_LOGE(TAG, "Tried to start before sensors initalized.");
    return ESP_FAIL;
  }

  if (sensorHandle != NULL) {
    ESP_LOGW(TAG, "Tried to start already running sensor task");
    return ESP_OK;
  }

  mqtt_register(sensor_jsonify);

  result = xTaskCreate(sensor_task, SENSOR_TASK_NAME, SENSOR_TASK_STACKSIZE,
                       (void *)1, tskIDLE_PRIORITY, &sensorHandle);
  if (result != pdPASS) {
    ESP_LOGE(TAG, "Error starting sensor task!");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "sensor task started");
  return ESP_OK;
}

esp_err_t sensor_stop() { return ESP_OK; }
/**
 * @brief Dump sensor data struct into JSON
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t sensor_jsonify(cJSON *root) {
  sensor_data_t *sensorValues;
  cJSON *sensorData;
  size_t item_size;
  ESP_LOGD(TAG, "Reading ringbuffer");

  do {
    sensorValues =
        (sensor_data_t *)xRingbufferReceive(buf_handle, &item_size, 0);
    if (sensorValues == NULL) {
      ESP_LOGD(TAG, "Ringbuffer empty");
      return ESP_OK;  // Buffer has been drained
    }

    cJSON *sensorArray = cJSON_GetObjectItemCaseSensitive(root, "sensorData");

    if (sensorArray == NULL) {
      cJSON_AddItemToObject(root, "sensorData",
                            sensorArray = cJSON_CreateArray());
    }

    sensorData = cJSON_CreateObject();
    cJSON_AddItemToArray(sensorArray, sensorData);
    cJSON_AddStringToObject(sensorData, "name", "temp");
    cJSON_AddItemToObject(sensorData, "value",
                          cJSON_CreateNumber(sensorValues->temp));
    cJSON_AddStringToObject(sensorData, "units", "C");

    sensorData = cJSON_CreateObject();
    cJSON_AddItemToArray(sensorArray, sensorData);
    cJSON_AddStringToObject(sensorData, "name", "humidity");
    cJSON_AddItemToObject(sensorData, "value",
                          cJSON_CreateNumber(sensorValues->humidity));
    cJSON_AddStringToObject(sensorData, "units", "% Rh");

    vRingbufferReturnItem(buf_handle, (void *)sensorValues);
  } while (sensorValues != NULL);

  return ESP_OK;
}
