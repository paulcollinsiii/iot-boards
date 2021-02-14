/*!
 * @file main.cpp
 */

#include "secrets.h"
#include "pins.h"
#include "display.h"

#include "SPI.h"
#include "WiFi.h"

#include <Arduino.h>
#include <Adafruit_SHTC3.h>
#include <CommandParser.h>
#include <TaskManagerIO.h>

const char* ssid = WIFI_SSID;
const char* password =  WIFI_PW;

Adafruit_SHTC3 shtc3 = Adafruit_SHTC3();
Adafruit_SSD1675 display(250, 122, EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);

typedef CommandParser<> MyCommandParser;

MyCommandParser parser;

void cmd_getdata(MyCommandParser::Argument *args, char *response){
  sensors_event_t humidity, temp;

  shtc3.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data

  Serial.print("Temperature: "); Serial.print(temp.temperature); Serial.println(" degrees C");
  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");
  delay(1000);
}

void log_data(){
  Serial.print("Current IP: "); Serial.println(WiFi.localIP());
  sensors_event_t humidity, temp;
  shtc3.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
  Serial.print("Humidity: "); Serial.print(humidity.relative_humidity); Serial.println("% rH");
  Serial.print("Temp: "); Serial.print(temp.temperature); Serial.println(" C");
}

void update_display(){
  display.clearBuffer();
  display.setCursor(0, 12);
  display.setTextWrap(true);

  sensors_event_t humidity, temp;
  shtc3.getEvent(&humidity, &temp);// populate temp and humidity objects with fresh data
  writeTemp(&display, temp.temperature);
  writeHumidity(&display, humidity.relative_humidity);
  writeIP(&display, WiFi.localIP().toString().c_str());

  display.display();
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
}


void setup() {
  Serial.begin(115200);
  display.begin();

  setup_shtc3();
  setup_wifi();

  update_display();

  parser.registerCommand("read", "", cmd_getdata);

  taskManager.scheduleFixedRate(2, log_data, TIME_SECONDS);
  taskManager.scheduleFixedRate(180, update_display, TIME_SECONDS);
  log_data();
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
  Serial.print("Delay to next run: "); Serial.println(delayTime);
  delay(delayTime);
  taskManager.runLoop();
}

