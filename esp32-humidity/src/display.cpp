#include "display.h"

#include <Adafruit_EPD.h>
#include <gfxfont.h>
#include <Fonts/FreeMono9pt7b.h>


void writeTemp(Adafruit_EPD *d, float temp){
  char t[16];
  snprintf(t, 16, "Temp: %.2f C", temp);
  write(d, t, EPD_BLACK, &FreeMono9pt7b);
}

void writeHumidity(Adafruit_EPD *d, float humidity){
  char t[32];
  snprintf(t, 32, "Humidity: %.2f%%", humidity);
  write(d, t, EPD_BLACK, &FreeMono9pt7b);
}

void writeIP(Adafruit_EPD *d, const char *ip){
  char t[32];
  snprintf(t, 32, "IP: %s", ip);
  write(d, t, EPD_BLACK, &FreeMono9pt7b);
}

void write(Adafruit_EPD *d, const char *text, uint16_t color, const GFXfont *font){
  d->setFont(font);
  d->setTextColor(color);
  d->setTextWrap(true);
  d->println(text);

}
