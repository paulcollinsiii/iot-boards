#!/bin/sh
docker run -it -p 1883:1883 -v $(pwd)/mosquitto:/mosquitto/ eclipse-mosquitto
