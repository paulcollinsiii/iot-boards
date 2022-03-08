#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <mqttlog.h>
#include <mqttmgr.h>
#include <stdatomic.h>
#include <string.h>

#define MQTTLOG_RINBUFFER_SIZE 512
#define MQTTLOG_BUFMAX 1024
#define MQTTLOG_TASK_LOGSEND_NAME "mqttlog-logsend"
#define MQTTLOG_TASK_LOGSEND_STACKSIZE 2 * 1024

typedef struct state_t {
  bool initialized;
  atomic_uint_fast32_t buf_msg_size;
  atomic_uint_fast16_t msgs_discarded;
  RingbufHandle_t ring_buffer;
  TaskHandle_t task_logsend;
} state_t;

static state_t state;

const char *TAG = MQTTLOG_TASK_LOGSEND_NAME;

static const char *mqtt_log_level_to_str(esp_log_level_t level) {
  switch (level) {
    case ESP_LOG_ERROR:
      return "ERROR";
    case ESP_LOG_WARN:
      return "WARN";
    case ESP_LOG_INFO:
      return "INFO";
    case ESP_LOG_DEBUG:
      return "DEBUG";
    case ESP_LOG_VERBOSE:
      return "VERBOSE";
    default:
      return "UNKONWN";
  }
}

static esp_err_t mqttlog_drop_msg() {
  mqttmgr_msg_t *buffered_msg;
  size_t msg_size;

  if (state.buf_msg_size == 0) {
    return ESP_ERR_NO_MEM;
  }

  buffered_msg =
      (mqttmgr_msg_t *)xRingbufferReceive(state.ring_buffer, &msg_size, 0);
  if (buffered_msg == NULL) {
    ESP_LOGE(TAG, "Expected msg but failed to receive!");
    return ESP_FAIL;
  }
  ESP_LOGW(TAG, "Dropping msg: %s", (char *)buffered_msg->msg);
  free(buffered_msg->msg);
  state.buf_msg_size -= buffered_msg->len;
  vRingbufferReturnItem(state.ring_buffer, buffered_msg);

  state.msgs_discarded++;
  return ESP_OK;
}

static esp_err_t mqttlog_queue_msg(mqttmgr_msg_t *msg) {
  // Is the message longer than what should be queued?
  while (state.buf_msg_size + msg->len > MQTTLOG_BUFMAX) {
    mqttlog_drop_msg();
  }
  // Queue
  while (pdTRUE !=
         xRingbufferSend(state.ring_buffer, msg, sizeof(mqttmgr_msg_t), 0)) {
    ESP_LOGW(TAG, "Log buffer full... dropping oldest and retrying");
    switch (mqttlog_drop_msg()) {
      case ESP_OK:
      case ESP_ERR_NO_MEM:
        break;
      case ESP_FAIL:
      default:
        ESP_LOGE(TAG, "Error dropping message from buffer!");
        abort();
    }
  }

  state.buf_msg_size += msg->len;
  xTaskNotifyGive(state.task_logsend);

  return ESP_OK;
}

static void mqttlog_task_logsend(void *pvParm) {
  mqttmgr_msg_t *buffered_msg;
  size_t msg_size;

  ESP_LOGI(TAG, "Starting %s", MQTTLOG_TASK_LOGSEND_NAME);
  while (1) {
    // Wait for the queue_msg function to notify
    xTaskNotifyWait(0, ULONG_MAX, NULL, portMAX_DELAY);

    while (state.buf_msg_size > 0) {
      // If we're not connected to MQTT, then we can't really send messages
      xEventGroupWaitBits(mqttmgr_events, MQTTMGR_CLIENT_CONNECTED_BIT, pdFALSE,
                          pdTRUE, portMAX_DELAY);
      if (state.msgs_discarded != 0) {
        MQTTLOG_LOGW(TAG, "Messages dropped from queue", "message_count=%u",
                     state.msgs_discarded);
        state.msgs_discarded = 0;
      }
      // Dequeue ring buffer till empty or timeout
      buffered_msg =
          (mqttmgr_msg_t *)xRingbufferReceive(state.ring_buffer, &msg_size, 0);
      if (buffered_msg == NULL) {
        ESP_LOGE(TAG, "Expected msg but failed to receive!");
        abort();
      }
      if (ESP_OK != mqttmgr_queuemsg(buffered_msg, portMAX_DELAY)) {
        ESP_LOGE(TAG, "Dropping message on the floor: %s",
                 (char *)buffered_msg->msg);
        free(buffered_msg->msg);
        state.msgs_discarded++;
      }

      state.buf_msg_size -= buffered_msg->len;
      vRingbufferReturnItem(state.ring_buffer, buffered_msg);
    }
  }
}

esp_err_t mqttlog_log_render(const char *tag, esp_log_level_t level,
                             const char *event, const char *tag_format, ...) {
  va_list args;
  cJSON *json, *tags;
  mqttmgr_msg_t msg;
  time_t now;
  char *token = NULL, *sub_token = NULL, *tag_name = NULL;
  char *token_state = NULL, *sub_token_state = NULL, local_tag_format[256];
  char *json_rendered, timestamp[32];

  strncpy(local_tag_format, tag_format, sizeof(local_tag_format));

  // create json with tag, level and event msg
  json = cJSON_CreateObject();
  time(&now);
  MQTTLOG_ISO8601(now, timestamp);
  cJSON_AddStringToObject(json, "timestamp", timestamp);
  cJSON_AddStringToObject(json, "source", tag);
  cJSON_AddStringToObject(json, "level", mqtt_log_level_to_str(level));
  cJSON_AddStringToObject(json, "event", event);
  cJSON_AddItemToObject(json, "tags", tags = cJSON_CreateObject());

  // render tag data into json
  va_start(args, tag_format);

  token = strtok_r(local_tag_format, " ", &token_state);
  while (token != NULL) {
    tag_name = strtok_r(token, "=", &sub_token_state);
    sub_token = strtok_r(NULL, "=", &sub_token_state);
    if (*sub_token != '%') {
      cJSON_AddStringToObject(tags, tag_name, sub_token);
      token = strtok_r(NULL, " ", &token_state);
      continue;
    }
    sub_token++;
    switch (*sub_token) {
      case 's':
      case 'S':
        cJSON_AddStringToObject(tags, tag_name, va_arg(args, char *));
        break;
      case 'b':
      case 'B':
        cJSON_AddBoolToObject(tags, tag_name, (bool)va_arg(args, int));
        break;
      case 'i':
      case 'I':
        cJSON_AddNumberToObject(tags, tag_name, va_arg(args, long));
        break;
      case 'u':
      case 'U':
        cJSON_AddNumberToObject(tags, tag_name, va_arg(args, ulong));
        break;
      case 'l':
      case 'L':
        cJSON_AddNumberToObject(tags, tag_name, va_arg(args, long long));
        break;
      case 'F':
      case 'f':
        cJSON_AddNumberToObject(tags, tag_name, va_arg(args, double));
        break;
      default:
        ESP_LOGE(TAG, "Unknown format string: '%s' in %s", sub_token,
                 tag_format);
        abort();
    }
    token = strtok_r(NULL, " ", &token_state);
  }
  va_end(args);
  json_rendered = cJSON_PrintUnformatted(json);
  ESP_LOG_LEVEL_LOCAL(level, tag, "%s", json_rendered);
  msg = (mqttmgr_msg_t){
      .len = strlen(json_rendered),
      .msg = json_rendered,
      .topic = MQTTMGR_TOPIC_LOG,
  };
  cJSON_Delete(json);

  return mqttlog_queue_msg(&msg);
}

esp_err_t mqttlog_init() {
  if (state.initialized) {
    ESP_LOGE(TAG, "Attempt to re-initalize mqttlog");
    abort();
  }

  state = (state_t){
      .buf_msg_size = 0,
      .initialized = true,
      .msgs_discarded = 0,
      .ring_buffer =
          xRingbufferCreate(MQTTLOG_RINBUFFER_SIZE, RINGBUF_TYPE_NOSPLIT),
  };

  if (pdPASS != xTaskCreate(mqttlog_task_logsend, MQTTLOG_TASK_LOGSEND_NAME,
                            MQTTLOG_TASK_LOGSEND_STACKSIZE, (void *)1,
                            tskIDLE_PRIORITY, &state.task_logsend)) {
    ESP_LOGE(TAG, "Error starting %s", MQTTLOG_TASK_LOGSEND_NAME);
    abort();
  }
  if (state.ring_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to create ring buffer");
    return ESP_FAIL;
  }
  return ESP_OK;
}
