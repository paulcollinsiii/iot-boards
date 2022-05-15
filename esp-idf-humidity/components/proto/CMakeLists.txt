set(src_files
  "build/commands.pb-c.c"
  "build/modules/alarm.pb-c.c"
  "build/modules/blinky.pb-c.c"
  "build/modules/ltr390.pb-c.c"
  "build/modules/otamgr.pb-c.c"
  "build/modules/sensormgr.pb-c.c"
  "build/modules/sht4x.pb-c.c"
  "build/modules/shtc3.pb-c.c"
)

idf_component_register(
  SRCS ${src_files}
  INCLUDE_DIRS "./build/"
  REQUIRES "protobuf-c"
)
