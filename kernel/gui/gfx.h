/* gfx.h - drawing on pixel surfaces (framebuffer format, 32 bpp) */
#ifndef BDOS_GFX_H
#define BDOS_GFX_H

#include <stdint.h>

struct rect {
	int x, y, w, h;
};

struct gfx_rec;

struct surface {
	uint32_t *px;
	int w, h, pitch;	/* pitch in pixels */
	struct rect clip;	/* drawing is limited to this */
	int owned;
	/* drawing here is also recorded, for a viewer that draws for itself */
	struct gfx_rec *rec;
	uint16_t rid;
};

/*
 * Recording: a remote viewer (bterm) that keeps its own copy of a surface
 * gets the drawing operations instead of the pixels. Each op is
 *   u8 op, u16 surface, i16 clip x y w h, then its arguments;
 * colours are 0xRRGGBB. What can't be told as an op (pixels a program
 * wrote itself) goes as GOP_PIXELS.
 */
enum {
	GOP_FILL = 1, GOP_PLOT, GOP_LINE, GOP_TEXT, GOP_BLIT, GOP_ICON, GOP_PATTERN, GOP_PIXELS,
	GOP_CREATE, GOP_FREE, GOP_SCREEN, GOP_FONT, GOP_ICONDEF,
	GOP_VERIFY,	/* (testing) what the surface should look like now: like PIXELS */
};

struct gfx_rec {
	void (*emit)(struct gfx_rec *r, const void *data, size_t n);
	uint16_t next_id;
	int font_sent;
	uint8_t icon_sent[64];		/* one bit an icon */
};

/* start mirroring s to r's viewer: it gets the surface and what's on it */
void gfx_track(struct surface *s, struct gfx_rec *r);
/* pixels were written straight into s (not with these functions): send them */
void surf_touched(struct surface *s, struct rect r);
/* (testing) send what s looks like, for the viewer to check its copy against */
void gfx_verify(struct surface *s);
/* the 8x8 one-bit pattern over r, in ink (the desktop's) */
void fill_pattern(struct surface *s, struct rect r, const uint8_t pat[8], uint32_t ink);

#define FONT_W 8
#define FONT_H 16

int surf_init(struct surface *s, int w, int h);
void surf_wrap(struct surface *s, uint32_t *px, int w, int h, int pitch);
void surf_free(struct surface *s);
void surf_noclip(struct surface *s);
void surf_clip(struct surface *s, struct rect r);	/* intersect with bounds */

uint32_t col(uint32_t rgb);			/* 0xRRGGBB -> pixel */
void fill(struct surface *s, int x, int y, int w, int h, uint32_t c);
void hline(struct surface *s, int x, int y, int w, uint32_t c);
void vline(struct surface *s, int x, int y, int h, uint32_t c);
void outline(struct surface *s, int x, int y, int w, int h, uint32_t c);
void line(struct surface *s, int x0, int y0, int x1, int y1, uint32_t c);
void plot(struct surface *s, int x, int y, uint32_t c);
void text(struct surface *s, int x, int y, const char *str, uint32_t fg);
void text_bg(struct surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg);
void glyph(struct surface *s, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg, int opaque);
void bevel(struct surface *s, int x, int y, int w, int h, int raised);
void blit(struct surface *dst, int dx, int dy, struct surface *src, int sx, int sy, int w, int h);
void draw_icon(struct surface *s, int x, int y, const char *name);	/* 32x32 */
int icon_exists(const char *name);

struct rect rect_union(struct rect a, struct rect b);
struct rect rect_isect(struct rect a, struct rect b);
int rect_empty(struct rect r);
int rect_has(struct rect r, int x, int y);

#endif
