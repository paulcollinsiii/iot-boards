# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
  SRCS "sht4x.c" "sht4xmgr.c"
  INCLUDE_DIRS .
  REQUIRES "proto" "json" "i2cdev" "sensormgr"
)
