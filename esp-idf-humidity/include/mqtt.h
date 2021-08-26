#ifndef MQTT_H
#define MQTT_H

#include <cJSON.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <mqtt_client.h>

TaskHandle_t mqtt_task_handle;  // TODO: Convert to getter?

/**
 * @brief Register an handler to append JSON to messages
 *
 * Tasks that register a handler are expected to maintain their own Ringbuffer
 * of messages and the JSON generator will drain that buffer of some number (or
 * all) messages when called. External tasks need to notify mqtt of pending
 * messages by calling xTaskNotifyGive(mqtt_task_handle);
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to add handler
 */
esp_err_t mqtt_register(esp_err_t (*fn)(cJSON *));

/**
 * @brief Initalize MQTT config and internal state
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqtt_init();

/**
 * @brief Start ESP-MQTT-Client, MQTT Task handler
 *
 * Once MQTT task is running it waits for ESP-MQTT-Client to be fully connected
 * before getting messages from registered handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to start MQTT task
 */
esp_err_t mqtt_start();

/**
 * @brief Stop all MQTT related task handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqtt_stop();

#endif
