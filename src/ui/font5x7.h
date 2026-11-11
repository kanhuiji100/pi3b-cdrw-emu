#ifndef CDRW_FONT5X7_H
#define CDRW_FONT5X7_H

#include <stdint.h>

#define FONT5X7_FIRST 32
#define FONT5X7_LAST 126
#define FONT5X7_CHARS (FONT5X7_LAST - FONT5X7_FIRST + 1)
#define FONT5X7_WIDTH 5
#define FONT5X7_HEIGHT 7

extern const uint8_t font5x7[FONT5X7_CHARS][5];

#endif
