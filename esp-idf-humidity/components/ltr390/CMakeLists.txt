# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.

idf_component_register(
  SRCS "ltr390.c" "ltr390mgr.c"
  INCLUDE_DIRS .
  REQUIRES "proto" "i2cdev" "sensormgr"
)
