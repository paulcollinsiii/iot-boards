# This file was automatically generated for projects
# without default 'CMakeLists.txt' file.
FILE(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/*.c)

idf_component_register(
  SRCS ${app_sources}
  INCLUDE_DIRS .
  REQUIRES "json" "proto" "backoffAlgorithm-1.0.1" "mqtt"
)
