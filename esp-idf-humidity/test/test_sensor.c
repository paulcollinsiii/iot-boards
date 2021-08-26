#include <unity.h>

#include "sensor.h"

void test_sensor_jsonify(void) {
  char *formatted;

  sensor_data_t data = {.temp = 24.0, .humidity = 20.0, .timestamp = 0};

  cJSON *root = cJSON_CreateObject();
  jsonify(&data, root);

  formatted = cJSON_PrintUnformatted(root);
  printf(formatted);
}

void app_main() {
  UNITY_BEGIN();

  RUN_TEST(test_sensor_jsonify);

  UNITY_END();
}
