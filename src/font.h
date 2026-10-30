// Bitmap fonts baked by tools/mkassets.py (font_*.png + .fnt)
#pragma once
#include <stdint.h>
#include "gfx.h"

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct Font Font;

Font * font_load(const char * png, const char * fnt);
// y is the top of the line; size is the line's height in pixels on screen. Returns the width drawn.
float font_draw(Font * f, const char * text, float x, float y, float size, uint32_t color, int align);
float font_width(Font * f, const char * text, float size);
// drawn with a dark drop shadow
float font_draw_shadow(Font * f, const char * text, float x, float y, float size, uint32_t color, int align);
// cut to fit max_w, ending in "..."
float font_draw_fit(Font * f, const char * text, float x, float y, float size, float max_w, uint32_t color, int align);
