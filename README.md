## What is this

IoT playground for myself and eventually my daughter to play with. Started out
as a simple project to play with hardware and learn about embedded programming.
Ballooned into doing things the hard way quickly, mainly as a learning
experience.

### Hardware used in this project
#### Microcontrollers
* [Adafruit ESP32](https://www.adafruit.com/product/3591)
* [Adafruit ESP32-S2](https://www.adafruit.com/product/4769)

#### Sensors
* [Adafruit SHTC3](https://www.adafruit.com/product/4636)
* [Adafruit SHT4X](https://www.adafruit.com/product/4885)
* [Adafruit LTR390](https://www.adafruit.com/product/4831)


## Setting up

Tools used:
* Python libraries / tools
  * PlatformIO
  * Poetry
* Vagrant - For running a local Kubernetes "cluster"
* k0sctl - Kubernetes cluster setup automation
* protobuf-compiler & [protobuf-c-compiler](https://github.com/protobuf-c/protobuf-c)


Running `pio run -t menuconfig` is needed to set several defaults depending on the board you're using.
Look in the `Components` section and read through the options, but in particular

* Set the board memory sizes
* Enable the sensors your attaching for the particular build, and the pins I2C
  is connected to
* Set the partitions to custom, and which csv to use


## Handy Commands

### Build with
`pio run -t upload && pio device monitor --raw`

Or you can do it through vscode

### Grab the crash dump off the flash of the device

`espcoredump.py -p /dev/ttyACM0 info_corefile ~/coding/iot-boards/esp-idf-humidity/.pio/build/featheresp32-s2/firmware.elf -d 1`
PlatformIO doesn't directly support doing this so you have to go into the
framework folder directly, set IDF_PATH and then go into components to find
this file

Additionally there were I couple silly mods to the python modules that had to
be made since the S2 I have needs to be set to not reset afterwards.
