# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
  SRCS "shtc3.c" "shtc3mgr.c"
  INCLUDE_DIRS .
  REQUIRES "proto" "json" "i2cdev" "sensormgr"
)
