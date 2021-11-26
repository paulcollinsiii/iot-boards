#ifndef MQTT_MGR_H
#define MQTT_MGR_H

#include <cJSON.h>
#include <commands.pb-c.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

typedef int mqttmgr_cmderr_t;

typedef void(dealloc_cb_fn)(CommandResponse *resp_out);

/**
 * @brief A handler for a CommandRequest the fills in a CommandResponse
 *
 * Since CommandResponse object may have their own malloc's happening, the
 * dealloc_cb function pointer is expected to be set if the sub function needs
 * to de-allocate any nested structures. If left as null then no callback
 * happens.
 */
typedef CommandResponse__RetCodeT(cmdhandler)(CommandRequest *message,
                                              CommandResponse *resp_out,
                                              dealloc_cb_fn **dealloc_cb_out);
typedef esp_err_t(jsonhandler)(cJSON *);
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
esp_err_t mqttmgr_register_sensor_encoder(esp_err_t (*fn)(cJSON *));

/**
 * @brief Register command handler for received commands
 *
 * Tasks that register a command handler must a message type defined in the
 * commands.proto file
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to add handler
 */
esp_err_t mqttmgr_register_cmd_handler(cmdhandler *);

/**
 * @brief Initalize MQTT config and internal state
 *
 * @param device_id UUID to use for device specific channels
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqttmgr_init(char *device_id);

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
esp_err_t mqttmgr_start();

/**
 * @brief Stop all MQTT related task handlers
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqttmgr_stop();

/**
 * @brief Notify MQTT to publish pending messages
 *
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqttmgr_notify();

#endif
