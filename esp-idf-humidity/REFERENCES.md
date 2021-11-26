When writing several sections of code the following repos were extremely
helpful when figuring out how to interface with hardware

## `components/i2dev`

This is directly taken from https://github.com/UncleRus/esp-idf-lib and
modified only to remove the dependency on the helper macros, and limit it to
ESP32 only. The original license file is left intact, but file headers have
been modified.


## `components/shtc3`

References from both repos. I started implementing the driver in CPP based on
the Adafruit library and then converted it to C and based it on UncleRus's lib.

https://github.com/UncleRus/esp-idf-lib
https://github.com/adafruit/Adafruit_SHTC3


## `components/esp_cron`

Copied from https://github.com/DavidMora/esp_cron
with minor modifications to support esp-idf 4.x buildsystem in platformio

I added changes based on PR #1 in this repo because the error trapping of
checking for invalid crons and not scheduling is better behavior in my mind.
Converting it from an int return to an esp_err_t made it a bit more consistent
with the esp-idf

## `include/uuid.h`

Originally Copied from https://causlayer.orgs.hk/furrysalamander/esp-uuid
Modified to move function defs out of the header into a C file directly and
removed what I think is C++ style String function


## Provisioning

WiFi Provisioning and I2C examples are heavily influenced by the esp-idf
example code and quickstarts


## Timezone data

JSON download and then massively trimmed from https://github.com/nayarsystems/posix_tz_db
