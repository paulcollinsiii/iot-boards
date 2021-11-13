## What is this

IoT playground for myself and eventually my daughter to play with. Started out
as a simple project to play with hardware and learn about embedded programming.
Ballooned into doing things the hard way quickly, mainly as a learning
experience.

Hardware used in this project
* [Adafruit ESP32](https://www.adafruit.com/product/3591)
* [Adafruit SHTC3](https://www.adafruit.com/product/4636)
* [Adafruit LTR390](https://www.adafruit.com/product/4831) (Not yet enabled, I'll get there)

Later I'll compile & test this out on an [Adafruit
FeatherS2](https://www.adafruit.com/product/4769) which _should_ work with
everything but I'll probably be setting up several macros to make it sane


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
*
