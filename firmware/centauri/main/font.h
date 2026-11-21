#pragma once
#include <stdint.h>
typedef struct { uint16_t cp; uint8_t w; uint8_t rows[9]; } glyph_t;
extern const glyph_t FONT[];
extern const int FONT_COUNT, FONT_H, FONT_BASE;
