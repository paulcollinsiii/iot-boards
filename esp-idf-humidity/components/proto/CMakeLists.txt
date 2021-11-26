FILE(GLOB_RECURSE app_sources ${CMAKE_CURRENT_LIST_DIR}/build/*.c)

idf_component_register(
  SRCS ${app_sources}
  INCLUDE_DIRS "./build/"
  REQUIRES "protobuf-c"
)
