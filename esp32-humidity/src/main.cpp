/*!
 * @file main.cpp
 */

#include <Adafruit_SHTC3.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <CommandParser.h>
#include <MQTT.h>
#include <SafeString.h>
#include <TaskManagerIO.h>
#include <ezTime.h>

#include "SPI.h"
#include "WiFi.h"
#include "display.h"
#include "pins.h"
#include "secrets.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PW;
const char* ntpServer = "192.168.2.1";

typedef CommandParser<> MyCommandParser;
MyCommandParser parser;

createSafeString(input, 5);
createSafeString(token, 5);

Adafruit_SHTC3 shtc3 = Adafruit_SHTC3();
Adafruit_SSD1675 display(250, 122, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS,
                         EPD_BUSY);
WiFiClient net;
MQTTClient client;
Timezone nyc;

sensors_event_t humidity, temp;
const char delim[] = "\r\n";

class SerialCommandEvent : public BaseEvent {
 private:
  bool skipToDelim = false;

 public:
  uint32_t timeOfNextCheck() override {
    if (input.read(Serial)) {
      markTriggeredAndNotify();
    }
    return 100UL * 1000UL;  // every 100ms check for serial data
  }

  void exec() override {
    char response[MyCommandParser::MAX_RESPONSE_SIZE];

    if (skipToDelim) {
      if (input.endsWithCharFrom(delim)) {
        skipToDelim = false;
      }
      input.clear();
      return;
    }
    if (input.nextToken(token, delim)) {
      parser.processCommand(token.c_str(), response);
      return;
    }
    if (input.isFull()) {
      skipToDelim = true;
      input.clear();
    }
  }

} serialCommandEvent;

void publish_serial() {
  Serial.print("Current IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("Humidity: ");
  Serial.print(humidity.relative_humidity);
  Serial.println("% rH");
  Serial.print("Temp: ");
  Serial.print(temp.temperature);
  Serial.println(" C");
  Serial.print("Current time: ");
  Serial.println(nyc.dateTime("Y-m-d H:i:s T"));
}

void cmd_getdata(MyCommandParser::Argument* args, char* response) {
  publish_serial();
}

void publish_mqtt() {
  StaticJsonDocument<200> doc;
  char serialized[212];

  if (!client.connected()) {
    client.connect("remote_bub");
  }

  doc["timestamp"] = nyc.dateTime(ISO8601);
  JsonObject data = doc.createNestedObject("sensorData");
  data["humidity"] = humidity.relative_humidity;
  data["temperature"] = temp.temperature;

  serializeJson(doc, serialized);
  // TODO: This should include more location data, programmable via serial cmd
  if (!client.publish("sensors", serialized)) {
    Serial.println("Failed to publish");
  }
}

void update_display() {
  display.clearBuffer();
  display.setCursor(0, 12);
  display.setTextWrap(true);

  writeTime(&display, nyc.dateTime("Y-m-d H:i T").c_str());
  writeIP(&display, WiFi.localIP().toString().c_str());
  writeTemp(&display, temp.temperature);
  writeHumidity(&display, humidity.relative_humidity);

  display.display();
}

void update_sensors() { shtc3.getEvent(&humidity, &temp); }

void update_and_publish() {
  update_sensors();
  publish_mqtt();
}

void setup_wifi() {
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print("Connecting to WiFi... ");
  }
  Serial.println(WiFi.localIP());
}

void setup_shtc3() {
  if (!shtc3.begin()) {
    Serial.println("Couldn't find SHTC3");
    while (1) delay(1);
  }
  Serial.println("Found SHTC3 sensor");
  update_sensors();
}

void setup_mqtt() { client.begin("192.168.2.164", net); }

void setup_serial() { SafeString::setOutput(Serial); }

void setup_time() {
  Serial.println("Setting up time...");
  nyc.setLocation("America/New_York");
  setServer("192.168.2.1");
  setInterval(0);  // Don't poll ntp server, we'll configure that
  updateNTP();
}

void setup() {
  Serial.begin(115200);
  display.begin();

  setup_serial();
  setup_shtc3();
  setup_wifi();
  setup_mqtt();
  setup_time();

  update_display();

  parser.registerCommand("read", "", cmd_getdata);

  taskManager.scheduleFixedRate(2, update_and_publish, TIME_SECONDS);
  taskManager.scheduleFixedRate(900, updateNTP,
                                TIME_SECONDS);  // NTP every 15 min
  taskManager.registerEvent(&serialCommandEvent);
  // taskManager.scheduleFixedRate(180, update_display, TIME_SECONDS);
  publish_serial();
}

void loop() {
  auto delayTime = taskManager.microsToNextTask() / 1000;
  delay(delayTime);
  taskManager.runLoop();
}
