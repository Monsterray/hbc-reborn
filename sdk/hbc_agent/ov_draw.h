/* Software drawing for the overlay, straight into a Wii external framebuffer
 * (YUYV: each 4 bytes hold Y0 Cb Y1 Cr for two pixels). Portable C, so the
 * host preview (tests/overlay_preview) draws exactly what the Wii shows. */

#ifndef OV_DRAW_H
#define OV_DRAW_H

#include <stdbool.h>
#include <stdint.h>

#include "ov_font.h"

typedef struct {
	uint8_t *fb;     /* YUYV pairs */
	int w, h;        /* pixels; w is even */
	int stride;      /* bytes per row */
	int clip_x0, clip_y0, clip_x1, clip_y1;
} ov_canvas;

typedef struct {
	uint8_t y, cb, cr;
} ov_color;

ov_color ov_rgb(int r, int g, int b);
void ov_clip(ov_canvas *c, int x, int y, int w, int h);
void ov_noclip(ov_canvas *c);

/* dst = src darkened to level/256 (0 black, 256 unchanged), whole frame. */
void ov_dim_copy(ov_canvas *dst, const uint8_t *src, int level);
void ov_fill(ov_canvas *c, int x, int y, int w, int h, ov_color col, int alpha);
/* A rounded box with a vertical gradient (top, middle, bottom colors) and
 * a 1-pixel border. */
void ov_panel(ov_canvas *c, int x, int y, int w, int h, int radius, ov_color top,
			  ov_color mid, ov_color bot, ov_color border, int alpha);
void ov_disc(ov_canvas *c, int cx, int cy, int r, ov_color col);
/* An RGBA image (8 bits each, rows top to bottom) with its top-left at x, y. */
void ov_image(ov_canvas *c, int x, int y, int w, int h, const uint8_t *rgba);
int ov_text_width(const ov_font *f, const char *s);
/* Draws s with its line top at y; returns the pen position after it. */
int ov_text(ov_canvas *c, const ov_font *f, int x, int y, const char *s, ov_color col);

#endif
