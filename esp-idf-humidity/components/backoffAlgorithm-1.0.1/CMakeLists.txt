include("${CMAKE_CURRENT_LIST_DIR}/backoffAlgorithmFilePaths.cmake")

message(STATUS "includedirs found: ${BACKOFF_ALGORITHM_INCLUDE_PUBLIC_DIRS}")

idf_component_register(
  SRCS ${BACKOFF_ALGORITHM_SOURCES}
  INCLUDE_DIRS ${BACKOFF_ALGORITHM_INCLUDE_PUBLIC_DIRS}
)
