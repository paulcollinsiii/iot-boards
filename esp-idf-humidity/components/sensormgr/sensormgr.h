/*
 * ESP-IDF LTR390 Sensor Managment Package
 */

#ifndef SENSORMGR_H
#define SENSORMGR_H

#include <cJSON.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t(measure_fn)(void **sensor_data_out, size_t *len);
typedef esp_err_t(marshall_fn)(void *sensor_data, cJSON *data_array);

typedef struct {
  char *topic;  // Expected to be a format string entry so that device-id can be
                // put in
  char *discovery;  // JSON structure for Home Assistant to auto-discover
} sensormgr_registration_discovery_t;  // TODO: This needs a function to
                                       // register discovery messages, can
                                       // accept a varidict num of these structs
                                       // to publish

typedef struct {
  measure_fn *measure;
  marshall_fn *marshall;
} sensormgr_registration_t;

esp_err_t sensormgr_init();

esp_err_t sensormgr_start();
esp_err_t sensormgr_stop();

esp_err_t sensormgr_register_sensor(sensormgr_registration_t reg);

#define SENSORMGR_ISO8601(timestamp, charbuff)          \
  do {                                                  \
    struct tm ___;                                      \
    gmtime_r(&timestamp, &___);                         \
    strftime(charbuff, 32, "%Y-%m-%dT%H:%M:%SZ", &___); \
  } while (0)

#ifdef __cplusplus
extern "C" {
#endif
#endif
