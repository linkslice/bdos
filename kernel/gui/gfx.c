/* gfx.c - pixel surfaces, text, 3D bevels and the built-in icons */
#include "../kernel.h"
#include "gfx.h"
#include "font.h"

int surf_init(struct surface *s, int w, int h)
{
	memset(s, 0, sizeof *s);
	s->px = kmalloc((size_t)w * (size_t)h * 4);
	if (!s->px)
		return -1;
	s->w = w;
	s->h = h;
	s->pitch = w;
	s->owned = 1;
	surf_noclip(s);
	return 0;
}

void surf_wrap(struct surface *s, uint32_t *px, int w, int h, int pitch)
{
	memset(s, 0, sizeof *s);
	s->px = px;
	s->w = w;
	s->h = h;
	s->pitch = pitch;
	surf_noclip(s);
}

void surf_free(struct surface *s)
{
	if (s->owned)
		kfree(s->px);
	s->px = NULL;
}

void surf_noclip(struct surface *s)
{
	s->clip = (struct rect){ 0, 0, s->w, s->h };
}

struct rect rect_isect(struct rect a, struct rect b)
{
	int x0 = MAX(a.x, b.x), y0 = MAX(a.y, b.y);
	int x1 = MIN(a.x + a.w, b.x + b.w), y1 = MIN(a.y + a.h, b.y + b.h);
	if (x1 <= x0 || y1 <= y0)
		return (struct rect){ 0, 0, 0, 0 };
	return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

struct rect rect_union(struct rect a, struct rect b)
{
	if (rect_empty(a))
		return b;
	if (rect_empty(b))
		return a;
	int x0 = MIN(a.x, b.x), y0 = MIN(a.y, b.y);
	int x1 = MAX(a.x + a.w, b.x + b.w), y1 = MAX(a.y + a.h, b.y + b.h);
	return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

int rect_empty(struct rect r)
{
	return r.w <= 0 || r.h <= 0;
}

int rect_has(struct rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

void surf_clip(struct surface *s, struct rect r)
{
	s->clip = rect_isect(r, (struct rect){ 0, 0, s->w, s->h });
}

uint32_t col(uint32_t rgb)
{
	return pixel_pack(rgb);
}

void fill(struct surface *s, int x, int y, int w, int h, uint32_t c)
{
	struct rect r = rect_isect((struct rect){ x, y, w, h }, s->clip);
	for (int j = 0; j < r.h; j++) {
		uint32_t *p = s->px + (size_t)(r.y + j) * (size_t)s->pitch + r.x;
		for (int i = 0; i < r.w; i++)
			p[i] = c;
	}
}

void plot(struct surface *s, int x, int y, uint32_t c)
{
	if (rect_has(s->clip, x, y))
		s->px[(size_t)y * (size_t)s->pitch + x] = c;
}

void hline(struct surface *s, int x, int y, int w, uint32_t c)
{
	fill(s, x, y, w, 1, c);
}

void vline(struct surface *s, int x, int y, int h, uint32_t c)
{
	fill(s, x, y, 1, h, c);
}

void outline(struct surface *s, int x, int y, int w, int h, uint32_t c)
{
	hline(s, x, y, w, c);
	hline(s, x, y + h - 1, w, c);
	vline(s, x, y, h, c);
	vline(s, x + w - 1, y, h, c);
}

void line(struct surface *s, int x0, int y0, int x1, int y1, uint32_t c)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	for (int guard = 0; guard < 100000; guard++) {
		plot(s, x0, y0, c);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

void glyph(struct surface *s, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg, int opaque)
{
	struct rect cell = rect_isect((struct rect){ x, y, FONT_W, FONT_H }, s->clip);
	if (rect_empty(cell))
		return;
	const uint8_t *g = font8x16[ch];
	for (int j = cell.y - y; j < cell.y - y + cell.h; j++) {
		uint32_t *p = s->px + (size_t)(y + j) * (size_t)s->pitch + x;
		uint8_t bits = g[j];
		for (int i = cell.x - x; i < cell.x - x + cell.w; i++) {
			if (bits & (0x80 >> i))
				p[i] = fg;
			else if (opaque)
				p[i] = bg;
		}
	}
}

void text(struct surface *s, int x, int y, const char *str, uint32_t fg)
{
	for (; *str; str++, x += FONT_W)
		glyph(s, x, y, (unsigned char)*str, fg, 0, 0);
}

void text_bg(struct surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
	for (; *str; str++, x += FONT_W)
		glyph(s, x, y, (unsigned char)*str, fg, bg, 1);
}

/* The classic two-pixel 3D edge: light top-left, dark bottom-right. */
void bevel(struct surface *s, int x, int y, int w, int h, int raised)
{
	uint32_t hi = col(0xFFFFFF), mid = col(0x808080), lo = col(0x404040);
	uint32_t a = raised ? hi : mid, b = raised ? lo : hi;
	hline(s, x, y, w, a);
	vline(s, x, y, h, a);
	hline(s, x, y + h - 1, w, b);
	vline(s, x + w - 1, y, h, b);
	if (raised) {
		hline(s, x + 1, y + h - 2, w - 2, mid);
		vline(s, x + w - 2, y + 1, h - 2, mid);
	} else {
		hline(s, x + 1, y + 1, w - 2, lo);
		vline(s, x + 1, y + 1, h - 2, lo);
	}
}

void blit(struct surface *dst, int dx, int dy, struct surface *src, int sx, int sy, int w, int h)
{
	/* clip against the source, then the destination clip */
	if (sx < 0) { dx -= sx; w += sx; sx = 0; }
	if (sy < 0) { dy -= sy; h += sy; sy = 0; }
	w = MIN(w, src->w - sx);
	h = MIN(h, src->h - sy);
	struct rect r = rect_isect((struct rect){ dx, dy, w, h }, dst->clip);
	if (rect_empty(r))
		return;
	sx += r.x - dx;
	sy += r.y - dy;
	for (int j = 0; j < r.h; j++)
		memcpy(dst->px + (size_t)(r.y + j) * (size_t)dst->pitch + r.x,
		    src->px + (size_t)(sy + j) * (size_t)src->pitch + sx, (size_t)r.w * 4);
}

/*
 * Icons: 16x16 pixel art drawn at 2x. Palette characters:
 *   .  transparent   k black   w white   g light grey   d dark grey
 *   b blue  n navy  t teal  c cyan  y yellow  o orange  r red  e green  m brown
 */
struct icon {
	const char *name;
	const char *rows[16];
};

static const struct icon icons[] = {
	{ "prompt", {
		"kkkkkkkkkkkkkkkk", "kddddddddddddddk", "kkkkkkkkkkkkkkkk", "kkkkkkkkkkkkkkkk",
		"kkgkkkkkkkkkkkkk", "kkkgkkkkkkkkkkkk", "kkkkgkkkkkkkkkkk", "kkkgkkkkkkkkkkkk",
		"kkgkkgggggkkkkkk", "kkkkkkkkkkkkkkkk", "kkkkkkkkkkkkkkkk", "kkkkkkkkkkkkkkkk",
		"kkkkkkkkkkkkkkkk", "kkkkkkkkkkkkkkkk", "kddddddddddddddk", "kkkkkkkkkkkkkkkk" } },
	{ "folder", {
		"................", "................", ".kkkkk..........", "kyyyyyk.........",
		"kyyyyyykkkkkkkk.", "kyyyyyyyyyyyyyyk", "kokkkkkkkkkkkkkk", "kyyyyyyyyyyyyyyk",
		"kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk",
		"kyyyyyyyyyyyyyyk", "kooooooooooooook", "kkkkkkkkkkkkkkkk", "................" } },
	{ "file", {
		"..kkkkkkkkk.....", "..kwwwwwwwkk....", "..kwwwwwwwkwk...", "..kwwwwwwwkkkk..",
		"..kwwwwwwwwwwk..", "..kwddddddddwk..", "..kwwwwwwwwwwk..", "..kwddddddddwk..",
		"..kwwwwwwwwwwk..", "..kwddddddddwk..", "..kwwwwwwwwwwk..", "..kwdddddwwwwk..",
		"..kwwwwwwwwwwk..", "..kwwwwwwwwwwk..", "..kkkkkkkkkkkk..", "................" } },
	{ "edit", {
		"..kkkkkkkkkk....", "..kwwwwwwwwk....", "..kwddddddwk....", "..kwwwwwwwwk..k.",
		"..kwddddddwk.kyk", "..kwwwwwwwwkkyyk", "..kwddddddwkyyk.", "..kwwwwwwwkyyk..",
		"..kwdddddkyyk...", "..kwwwwwwkrk....", "..kwddddkkk.....", "..kwwwwwwwwk....",
		"..kwddddddwk....", "..kwwwwwwwwk....", "..kkkkkkkkkk....", "................" } },
	{ "basic", {
		"kkkkkkkkkkkkkkkk", "knnnnnnnnnnnnnnk", "knwwwwnnnnnnnnnk", "knwnnnwnnnnnnnnk",
		"knwnnnwnnnnnnnnk", "knwwwwnnnnnnnnnk", "knwnnnwnnnnnnnnk", "knwnnnwnnnnnnnnk",
		"knwwwwnnnnnnnnnk", "knnnnnnnnnnnnnnk", "knnyynnyynnyynnk", "knnnnnnnnnnnnnnk",
		"knnyyyyyynnnnnnk", "knnnnnnnnnnnnnnk", "kkkkkkkkkkkkkkkk", "................" } },
	{ "gear", {
		"......kkkk......", "......kggk......", "..kk.kggggk.kk..", "..kgkggggggkgk..",
		"...kggggggggk...", ".kkgggkkkkgggkk.", "kggggk....kggggk", "kgggk......kgggk",
		"kgggk......kgggk", "kggggk....kggggk", ".kkgggkkkkgggkk.", "...kggggggggk...",
		"..kgkggggggkgk..", "..kk.kggggk.kk..", "......kggk......", "......kkkk......" } },
	{ "network", {
		"..kkkkkkkkkkkk..", "..kttttttttttk..", "..ktcccccccctk..", "..ktcccccccctk..",
		"..ktcccccccctk..", "..kttttttttttk..", "..kkkkkkkkkkkk..", "......kddk......",
		"....kkkkkkkk....", "........k.......", "........k.......", "kkkkkkkkkkkkkkkk",
		"........k.......", "......kkkkk.....", "......keeek.....", "......kkkkk....." } },
	{ "game", {
		"................", "................", "................", "..kkkkkkkkkkkk..",
		".kddddddddddddk.", "kddkdddddddrddk.", "kdkkkddddrdddrdk", "kddkdddddddrdddk",
		"kddddddddddddddk", "kdddkkkkkkkkdddk", "kddk........kddk", ".kk..........kk.",
		"................", "................", "................", "................" } },
	{ "book", {
		"................", ".kkkkkkk.kkkkkkk", "kwwwwwwwkwwwwwwk", "kwddddwwkwddddwk",
		"kwwwwwwwkwwwwwwk", "kwdddddwkwdddddk", "kwwwwwwwkwwwwwwk", "kwddddwwkwddddwk",
		"kwwwwwwwkwwwwwwk", "kwdddddwkwdddddk", "kwwwwwwwkwwwwwwk", "kwwwwwwwkwwwwwwk",
		"kbbbbbbbkbbbbbbk", "kkkkkkkkkkkkkkkk", "................", "................" } },
	{ "chip", {
		"...k.k.k.k.k....", "..kkkkkkkkkkk...", ".kkdddddddddkk..", "..kdeeeeeeedk...",
		".kkdeeeeeeedkk..", "..kdeeeeeeedk...", ".kkdeeeeeeedkk..", "..kdeeeeeeedk...",
		".kkdeeeeeeedkk..", "..kdeeeeeeedk...", ".kkdddddddddkk..", "..kkkkkkkkkkk...",
		"...k.k.k.k.k....", "................", "................", "................" } },
	{ "computer", {
		".kkkkkkkkkkkkkk.", ".kggggggggggggk.", ".kgkkkkkkkkkkgk.", ".kgkttttttttkgk.",
		".kgkttttttttkgk.", ".kgkttttttttkgk.", ".kgkttttttttkgk.", ".kgkkkkkkkkkkgk.",
		".kggggggggggggk.", ".kkkkkkkkkkkkkk.", ".....kggggk.....", "...kkkkkkkkkk...",
		"..kggggggggggk..", "..kgdddggggggk..", "..kkkkkkkkkkkk..", "................" } },
	{ "trash", {
		"......kkkk......", "..kkkkkkkkkkkk..", "..kggggggggggk..", "..kkkkkkkkkkkk..",
		"...kgdgdgdgdk...", "...kgdgdgdgdk...", "...kgdgdgdgdk...", "...kgdgdgdgdk...",
		"...kgdgdgdgdk...", "...kgdgdgdgdk...", "...kgdgdgdgdk...", "...kgdgdgdgdk...",
		"...kkkkkkkkkk...", "................", "................", "................" } },
	{ "group", {
		"................", "kkkkkkkkkkkkkkkk", "knnnnnnnnnnnnnnk", "kggggggggggggggk",
		"kgwwwwwwwwwwwwgk", "kgwkkwwkkwwkkwgk", "kgwkkwwkkwwkkwgk", "kgwwwwwwwwwwwwgk",
		"kgwkkwwkkwwwwwgk", "kgwkkwwkkwwwwwgk", "kgwwwwwwwwwwwwgk", "kggggggggggggggk",
		"kkkkkkkkkkkkkkkk", "................", "................", "................" } },
};

static uint32_t icon_colour(char c)
{
	switch (c) {
	case 'k': return 0x000000;
	case 'w': return 0xFFFFFF;
	case 'g': return 0xC3C3C3;
	case 'd': return 0x707070;
	case 'b': return 0x3060D0;
	case 'n': return 0x1D3A8A;
	case 't': return 0x2F6F73;
	case 'c': return 0x7FD6D6;
	case 'y': return 0xE8C050;
	case 'o': return 0xB08020;
	case 'r': return 0xD03030;
	case 'e': return 0x40A050;
	case 'm': return 0x8B5A2B;
	}
	return 0xFF00FF;
}

int icon_exists(const char *name)
{
	for (size_t i = 0; i < NELEM(icons); i++)
		if (!strcasecmp(icons[i].name, name))
			return 1;
	return 0;
}

void draw_icon(struct surface *s, int x, int y, const char *name)
{
	const struct icon *ic = &icons[0];
	for (size_t i = 0; i < NELEM(icons); i++)
		if (!strcasecmp(icons[i].name, name))
			ic = &icons[i];
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 16; i++) {
			char c = ic->rows[j][i];
			if (c == '.')
				continue;
			fill(s, x + i * 2, y + j * 2, 2, 2, col(icon_colour(c)));
		}
}
