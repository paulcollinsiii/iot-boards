#ifndef MQTT_MGR_H
#define MQTT_MGR_H

#include <commands.pb-c.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>

#define MQTTMGR_CLIENT_STARTED_BIT (1 << 0)
#define MQTTMGR_CLIENT_CONNECTED_BIT (1 << 1)

// This is set if the system is NOT currently connected to MQTT
// THIS IS DIFFERENT FROM DISCONNECTED!
#define MQTTMGR_CLIENT_NOTCONNECTED_BIT (1 << 2)

// MQTT was disconnected from the server. THIS SHOULD NOT BE USED FOR TESTING
// IF THE SYSTEM IS CONNECTED. It's a measurement of if the MQTT subsystem
// received a disconnection event
#define MQTTMGR_CLIENT_DISCONNECTED_BIT (1 << 3)

// Ring buffer has enough data to send via mqtt
#define SENSORMGR_LOWWATER_BIT (1 << 4)

// Ring buffer is getting full and should be drained to file or mqtt
#define SENSORMGR_HIGHWATER_BIT (1 << 5)

// File is done writing and can be read safely
#define SENSORMGR_DONEWRITING_BIT (1 << 6)

// Sensor reading can continue till buffers are completely full
#define SENSORMGR_POLLSENSORS_BIT (1 << 7)

EventGroupHandle_t mqttmgr_events;

typedef int mqttmgr_cmderr_t;

typedef enum {
  MQTTMGR_TOPIC_REQUEST = 0,
  MQTTMGR_TOPIC_RESPONSE,
  MQTTMGR_TOPIC_LOG,
  MQTTMGR_TOPIC_SENSOR
} mqttmgr_topicidx;

typedef struct {
  mqttmgr_topicidx topic;
  size_t len;
  uint8_t msg[];
} mqttmgr_msg_t;

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
 * @brief Attempt to reconnect to Wifi and MQTT server now
 *
 * Will reset the backoff algorithm as well.
 *
 * @return esp_err_t
 *    ESP_OK - Reconnect Task Notified
 *    ESP_ERR_WIFI_STATE - Already connected
 */
esp_err_t mqttmgr_reconnect_now();

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
 * @brief Queue a message to be sent
 *
 * The msg includes the size of the message to be sent and a pointer to the data
 * to send After successful sending of the msg, the pointer will be free'd
 *
 * @param msg   Message to enqueue
 * @param delay Message Enqueue timeout
 * @return
 *  - ESP_OK: Success
 *  - ESP_FAIL: Failed to data queue
 */
esp_err_t mqttmgr_queuemsg(mqttmgr_topicidx topic, size_t msg_len, void *msg,
                           TickType_t delay);

#endif
