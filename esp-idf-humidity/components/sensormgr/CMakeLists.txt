idf_component_register(
  SRCS "sensormgr.c"
  INCLUDE_DIRS .
  REQUIRES "json" "mqttmgr" "fatfs" "nvs_flash"
)
