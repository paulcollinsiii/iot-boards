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
* [Adafruit LTR390](https://www.adafruit.com/product/4831) (Not yet enabled, I'll get there)


## Setting up

Tools used:
* Python libraries / tools
  * PlatformIO
  * Poetry
* Vagrant - For running a local Kubernetes "cluster"
* k0sctl - Kubernetes cluster setup automation
* protobuf-compiler & protobuf-c-compiler


## Handy Commands

Build the
`pio run -t upload && pio device monitor --raw`

Or you can do it through vscode
