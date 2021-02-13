/*!
 * @file main.cpp
 */

#include "secrets.h"
#include "pins.h"

#include "SPI.h"
#include "WiFi.h"

#include <Adafruit_SHTC3.h>
#include <Adafruit_EPD.h>
#include <CommandParser.h>

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

void testdrawtext(char *text, uint16_t color) {
  display.setCursor(0, 0);
  display.setTextColor(color);
  display.setTextWrap(true);
  display.print(text);
}

void setup_wifi(){
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to WiFi..");
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

void setup_epd(){
  display.begin();
  display.clearBuffer();
  testdrawtext("Initial test output for the entire display!", EPD_BLACK);
  display.display();
  Serial.println("Display update attempted...");
}

void setup() {
  Serial.begin(115200);
  //setup_shtc3();
  setup_epd();
  parser.registerCommand("read", "", cmd_getdata);
}

void loop() {
  if (Serial.available()){
    char line[128];
    size_t lineLength = Serial.readBytesUntil('\n', line, 127);
    line[lineLength] = '\0';

    char response[MyCommandParser::MAX_RESPONSE_SIZE];
    parser.processCommand(line, response);
  }
}

