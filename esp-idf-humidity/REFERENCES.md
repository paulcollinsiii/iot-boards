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

## Provisioning

WiFi Provisioning and I2C examples are heavily influenced by the esp-idf
example code and quickstarts
