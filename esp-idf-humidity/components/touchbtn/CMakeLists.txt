# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
  SRCS "touchbtn.c" "touchbtn_esp32.c" "touchbtn_esp32s2.c"
  INCLUDE_DIRS .
  REQUIRES "blinky" "mqttmgr" "sensormgr"
)
