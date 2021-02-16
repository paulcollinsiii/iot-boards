/*!
 * @file main.cpp
 */

#include "secrets.h"
#include "pins.h"
#include "display.h"

#include "SPI.h"
#include "WiFi.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Adafruit_SHTC3.h>
#include <ezTime.h>
#include <CommandParser.h>
#include <TaskManagerIO.h>
#include <MQTT.h>

const char* ssid = WIFI_SSID;
const char* password =  WIFI_PW;
const char* ntpServer = "192.168.2.1";


typedef CommandParser<> MyCommandParser;
MyCommandParser parser;

Adafruit_SHTC3 shtc3 = Adafruit_SHTC3();
Adafruit_SSD1675 display(250, 122, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
WiFiClient net;
MQTTClient client;
Timezone nyc;

sensors_event_t humidity, temp;



void cmd_getdata(MyCommandParser::Argument *args, char *response){
  Serial.print("Temperature: "); Serial.print(temp.temperature); Serial.println(" degrees C");
  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");
}

void publish_serial(){
  Serial.print("Current IP: "); Serial.println(WiFi.localIP());
  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");
  Serial.print("Temp: "); Serial.print(temp.temperature); Serial.println(" C");
  Serial.print("Current time: "); Serial.println(nyc.dateTime());
}

void publish_mqtt(){
  StaticJsonDocument<200> doc;
  char serialized[212];

  if(!client.connected()){
    client.connect("remote_bub");
  }

  doc["timestamp"] = nyc.dateTime(ISO8601);
  JsonObject data = doc.createNestedObject("sensorData");
  data["humidity"] = humidity.relative_humidity;
  data["temperature"] = temp.temperature;

  serializeJson(doc, serialized);
  //TODO: This should include more location data, programmable via serial cmd
  client.publish("sensors", serialized);
}


void update_display(){
  display.clearBuffer();
  display.setCursor(0, 12);
  display.setTextWrap(true);

  writeTemp(&display, temp.temperature);
  writeHumidity(&display, humidity.relative_humidity);
  writeIP(&display, WiFi.localIP().toString().c_str());

  display.display();
}

void update_sensors(){
  shtc3.getEvent(&humidity, &temp);
}

void update_and_publish(){
  update_sensors();
  publish_mqtt();
  publish_serial();
}

void setup_wifi(){
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.print("Connecting to WiFi... ");
  }
  Serial.println(WiFi.localIP());
}

void setup_shtc3(){
  if (! shtc3.begin()) {
    Serial.println("Couldn't find SHTC3");
    while (1) delay(1);
  }
  Serial.println("Found SHTC3 sensor");
  update_sensors();
}

void setup_mqtt(){
  client.begin("192.168.2.164", net);
}

void setup_time(){
  Serial.println("Setting up time...");
  nyc.setLocation("America/New_York");
  setServer("192.168.2.1");
  setInterval(0);  // Don't poll ntp server, we'll configure that
  Serial.println("waiting for sync");
  updateNTP();
  Serial.println("Timehacked");
}

void setup() {
  Serial.begin(115200);
  display.begin();

  setup_shtc3();
  setup_wifi();
  setup_mqtt();
  setup_time();

  //update_display();

  parser.registerCommand("read", "", cmd_getdata);

  taskManager.scheduleFixedRate(2, update_and_publish, TIME_SECONDS);
  //taskManager.scheduleFixedRate(180, update_display, TIME_SECONDS);
  publish_serial();
}

void loop() {
  /*
  if (Serial.available()){
    char line[128];
    size_t lineLength = Serial.readBytesUntil('\n', line, 127);
    line[lineLength] = '\0';

    char response[MyCommandParser::MAX_RESPONSE_SIZE];
    parser.processCommand(line, response);
  }
  */
  auto delayTime = taskManager.microsToNextTask() / 1000;
  delay(delayTime);
  taskManager.runLoop();
}
