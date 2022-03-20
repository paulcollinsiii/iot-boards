#ifndef MQTTLOG_H
#define MQTTLOG_H

#include <cJSON.h>
#include <esp_err.h>
#include <esp_log.h>
#include <sdkconfig.h>

esp_err_t mqttlog_init();
esp_err_t mqttlog_log_render(const char *tag, esp_log_level_t level,
                             const char *event, const char *tag_format, ...);

#define MQTTLOG_LOGE(tag, event, tag_format, ...) \
  MQTTLOG_LOG_LEVEL_LOCAL(ESP_LOG_ERROR, tag, event, tag_format, ##__VA_ARGS__)
#define MQTTLOG_LOGW(tag, event, tag_format, ...) \
  MQTTLOG_LOG_LEVEL_LOCAL(ESP_LOG_WARN, tag, event, tag_format, ##__VA_ARGS__)
#define MQTTLOG_LOGI(tag, event, tag_format, ...) \
  MQTTLOG_LOG_LEVEL_LOCAL(ESP_LOG_INFO, tag, event, tag_format, ##__VA_ARGS__)
#define MQTTLOG_LOGD(tag, event, tag_format, ...) \
  MQTTLOG_LOG_LEVEL_LOCAL(ESP_LOG_DEBUG, tag, event, tag_format, ##__VA_ARGS__)
#define MQTTLOG_LOGV(tag, event, tag_format, ...)                  \
  MQTTLOG_LOG_LEVEL_LOCAL(ESP_LOG_VERBOSE, tag, event, tag_format, \
                          ##__VA_ARGS__)

#define MQTTLOG_LOG_LEVEL_LOCAL(level, tag, event, tag_format, ...)     \
  do {                                                                  \
    if (LOG_LOCAL_LEVEL >= level)                                       \
      mqttlog_log_render(tag, level, event, tag_format, ##__VA_ARGS__); \
  } while (0)

#define MQTTLOG_ISO8601(timestamp, charbuff)            \
  do {                                                  \
    struct tm ___;                                      \
    gmtime_r(&timestamp, &___);                         \
    strftime(charbuff, 32, "%Y-%m-%dT%H:%M:%SZ", &___); \
  } while (0)

#endif
