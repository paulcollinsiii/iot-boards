# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
  SRCS "otamgr.c"
  INCLUDE_DIRS .
  REQUIRES "app_update" "esp_https_ota" "blinky" "mqttmgr" "proto"
)
