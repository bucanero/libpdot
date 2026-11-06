/* A tiny 16-segment stroke font, drawn with SDL lines (no SDL_ttf). */
#ifndef ASTEROIDS_FONT_H
#define ASTEROIDS_FONT_H

#include <SDL.h>

/* Draws text with its top-left at (x, y); scale 1 is 8x12 pixels a glyph. */
void font_draw(SDL_Renderer *r, float x, float y, float scale, const char *text);
/* Width text would take at scale. */
float font_width(float scale, const char *text);
/* Centered horizontally on cx. */
void font_draw_centered(SDL_Renderer *r, float cx, float y, float scale, const char *text);

#endif
