#ifndef DISPLAY_H

#include <Adafruit_EPD.h>
#include <gfxfont.h>

void writeTemp(Adafruit_EPD *d, float temp);
void writeTime(Adafruit_EPD *d, const char *time);
void writeHumidity(Adafruit_EPD *d, float humidity);
void writeIP(Adafruit_EPD *d, const char *ip);
void write(Adafruit_EPD *d, const char *text, uint16_t color, const GFXfont *font = NULL);

#endif
