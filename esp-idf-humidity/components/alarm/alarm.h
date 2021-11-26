/*
 * ESP-IDF SHTC3 Temp / Humidity sensor
 *
 * Heavily based on https://github.com/UncleRus/esp-idf-lib examples
 */

#ifndef ALARM
#define ALARM

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  bool oneshot;
  char *crontab;
} alarm_t;

/**
 * @brief Add an alarm
 *
 * Err Codes:
 * ESP_ERR_INVALID_STATE  - Alarm is duplicate of existing alarm, discarded
 * ESP_ERR_NO_MEM         - Max number of alarms already being tracked
 *
 * @param   alarm     Alarm struct to add
 * @return            `ESP_OK` on success
 */
esp_err_t alarm_add(alarm_t *alarm);

/**
 * @brief Delete an alarm
 *
 *
 * Err Codes:
 * ESP_ERR_NOT_FOUND   - Alarm with matching crontab not found
 *
 * @param   crontab    Alarm crontab to delete
 * @return            `ESP_OK` on success
 */
esp_err_t alarm_delete(char *crontab);

/**
 * @brief Initalize the Alarm and register command handlers
 *
 * @return
 *  - ESP_OK: Success
 */
esp_err_t alarm_init();

#ifdef __cplusplus
}
#endif
#endif
