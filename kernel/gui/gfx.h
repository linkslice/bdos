/* gfx.h - drawing on pixel surfaces (framebuffer format, 32 bpp) */
#ifndef BDOS_GFX_H
#define BDOS_GFX_H

#include <stdint.h>

struct rect {
	int x, y, w, h;
};

struct surface {
	uint32_t *px;
	int w, h, pitch;	/* pitch in pixels */
	struct rect clip;	/* drawing is limited to this */
	int owned;
};

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
