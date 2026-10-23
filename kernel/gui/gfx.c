/* gfx.c - pixel surfaces, text, 3D bevels and the built-in icons */
#include "../kernel.h"
#include "gfx.h"
#include "font.h"

int surf_init(struct surface *s, int w, int h)
{
	memset(s, 0, sizeof *s);
	s->px = kzalloc((size_t)w * (size_t)h * 4);
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

/* ---- recording ---- */

static int rec_depth;	/* inside a recorded call: its inner calls aren't ops of their own */
#define REC(s) ((s)->rec && !rec_depth)

struct opbuf {
	uint8_t b[96];
	size_t n;
};

static void ob_hdr(struct opbuf *o, int op, struct surface *s)
{
	o->n = 0;
	o->b[o->n++] = (uint8_t)op;
	memcpy(o->b + o->n, &s->rid, 2);
	o->n += 2;
	int16_t c[4] = { (int16_t)s->clip.x, (int16_t)s->clip.y, (int16_t)s->clip.w, (int16_t)s->clip.h };
	memcpy(o->b + o->n, c, 8);
	o->n += 8;
}

static void ob_i16(struct opbuf *o, int v)
{
	int16_t x = (int16_t)v;
	memcpy(o->b + o->n, &x, 2);
	o->n += 2;
}

static void ob_u32(struct opbuf *o, uint32_t v)
{
	memcpy(o->b + o->n, &v, 4);
	o->n += 4;
}

static void ob_send(struct surface *s, struct opbuf *o)
{
	s->rec->emit(s->rec, o->b, o->n);
}

/* pixels of r, as runs: GOP_PIXELS x y w h, u32 runs, (u16 n, u32 rgb)... in bands */
static void rec_pixels_op(struct surface *s, struct rect r, int op);

static void rec_pixels(struct surface *s, struct rect r)
{
	rec_pixels_op(s, r, GOP_PIXELS);
}

void gfx_verify(struct surface *s)
{
	if (s->rec)
		rec_pixels_op(s, (struct rect){ 0, 0, s->w, s->h }, GOP_VERIFY);
}

static void rec_pixels_op(struct surface *s, struct rect r, int op)
{
	r = rect_isect(r, (struct rect){ 0, 0, s->w, s->h });
	if (rect_empty(r))
		return;
	uint8_t *buf = kmalloc(32 + (size_t)r.w * 64 * 6);
	if (!buf)
		return;
	for (int y0 = r.y; y0 < r.y + r.h; y0 += 64) {
		int bh = MIN(64, r.y + r.h - y0);
		struct opbuf o;
		ob_hdr(&o, op, s);
		ob_i16(&o, r.x);
		ob_i16(&o, y0);
		ob_i16(&o, r.w);
		ob_i16(&o, bh);
		memcpy(buf, o.b, o.n);
		size_t n = o.n + 4;
		uint32_t runs = 0, cur = 0;
		for (int y = y0; y < y0 + bh; y++) {
			const uint32_t *row = s->px + (size_t)y * (size_t)s->pitch + r.x;
			for (int x = 0; x < r.w; x++) {
				if (runs && row[x] == cur) {
					uint16_t c;
					memcpy(&c, buf + n - 6, 2);
					if (c < 0xFFFF) {
						c++;
						memcpy(buf + n - 6, &c, 2);
						continue;
					}
				}
				cur = row[x];
				uint16_t one = 1;
				uint32_t rgb = pixel_unpack(cur);
				memcpy(buf + n, &one, 2);
				memcpy(buf + n + 2, &rgb, 4);
				n += 6;
				runs++;
			}
		}
		memcpy(buf + o.n, &runs, 4);
		s->rec->emit(s->rec, buf, n);
	}
	kfree(buf);
}


void gfx_track(struct surface *s, struct gfx_rec *r)
{
	if (!r)
		return;
	if (!r->font_sent) {
		uint8_t *f = kmalloc(1 + 2 + 8 + sizeof font8x16);
		if (f) {
			memset(f, 0, 11);
			f[0] = GOP_FONT;
			memcpy(f + 11, font8x16, sizeof font8x16);
			r->emit(r, f, 11 + sizeof font8x16);
			kfree(f);
			r->font_sent = 1;
		}
	}
	s->rec = r;
	s->rid = ++r->next_id ? r->next_id : ++r->next_id;
	struct rect save = s->clip;
	surf_noclip(s);
	struct opbuf o;
	ob_hdr(&o, GOP_CREATE, s);
	ob_i16(&o, s->w);
	ob_i16(&o, s->h);
	ob_send(s, &o);
	s->clip = save;
	rec_pixels(s, (struct rect){ 0, 0, s->w, s->h });
}

void surf_touched(struct surface *s, struct rect r)
{
	if (REC(s))
		rec_pixels(s, r);
}

void surf_free(struct surface *s)
{
	if (s->rec) {
		struct opbuf o;
		ob_hdr(&o, GOP_FREE, s);
		ob_send(s, &o);
		s->rec = NULL;
	}
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
	if (REC(s)) {
		struct opbuf o;
		ob_hdr(&o, GOP_FILL, s);
		ob_i16(&o, x);
		ob_i16(&o, y);
		ob_i16(&o, w);
		ob_i16(&o, h);
		ob_u32(&o, pixel_unpack(c));
		ob_send(s, &o);
	}
	struct rect r = rect_isect((struct rect){ x, y, w, h }, s->clip);
	for (int j = 0; j < r.h; j++) {
		uint32_t *p = s->px + (size_t)(r.y + j) * (size_t)s->pitch + r.x;
		for (int i = 0; i < r.w; i++)
			p[i] = c;
	}
}

void plot(struct surface *s, int x, int y, uint32_t c)
{
	if (REC(s)) {
		struct opbuf o;
		ob_hdr(&o, GOP_PLOT, s);
		ob_i16(&o, x);
		ob_i16(&o, y);
		ob_u32(&o, pixel_unpack(c));
		ob_send(s, &o);
	}
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
	int rec = REC(s);
	if (rec) {
		struct opbuf o;
		ob_hdr(&o, GOP_LINE, s);
		ob_i16(&o, x0);
		ob_i16(&o, y0);
		ob_i16(&o, x1);
		ob_i16(&o, y1);
		ob_u32(&o, pixel_unpack(c));
		ob_send(s, &o);
		rec_depth++;
	}
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
	if (rec)
		rec_depth--;
}

static void rec_text(struct surface *s, int x, int y, const char *str, size_t n, uint32_t fg, uint32_t bg, int opaque)
{
	uint8_t b[11 + 4 + 9 + 2 + 256];
	struct opbuf o;
	ob_hdr(&o, GOP_TEXT, s);
	ob_i16(&o, x);
	ob_i16(&o, y);
	ob_u32(&o, pixel_unpack(fg));
	ob_u32(&o, pixel_unpack(bg));
	o.b[o.n++] = (uint8_t)opaque;
	n = MIN(n, 256u);
	uint16_t n16 = (uint16_t)n;
	memcpy(o.b + o.n, &n16, 2);
	o.n += 2;
	memcpy(b, o.b, o.n);
	memcpy(b + o.n, str, n);
	s->rec->emit(s->rec, b, o.n + n);
}

void glyph(struct surface *s, int x, int y, unsigned char ch, uint32_t fg, uint32_t bg, int opaque)
{
	if (REC(s)) {
		char c = (char)ch;
		rec_text(s, x, y, &c, 1, fg, bg, opaque);
	}
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
	int rec = REC(s) && strlen(str) <= 256;
	if (rec) {
		rec_text(s, x, y, str, strlen(str), fg, 0, 0);
		rec_depth++;
	}
	for (; *str; str++, x += FONT_W)
		glyph(s, x, y, (unsigned char)*str, fg, 0, 0);
	if (rec)
		rec_depth--;
}

void text_bg(struct surface *s, int x, int y, const char *str, uint32_t fg, uint32_t bg)
{
	int rec = REC(s) && strlen(str) <= 256;
	if (rec) {
		rec_text(s, x, y, str, strlen(str), fg, bg, 1);
		rec_depth++;
	}
	for (; *str; str++, x += FONT_W)
		glyph(s, x, y, (unsigned char)*str, fg, bg, 1);
	if (rec)
		rec_depth--;
}

void fill_pattern(struct surface *s, struct rect r, const uint8_t pat[8], uint32_t ink)
{
	if (REC(s)) {
		struct opbuf o;
		ob_hdr(&o, GOP_PATTERN, s);
		ob_i16(&o, r.x);
		ob_i16(&o, r.y);
		ob_i16(&o, r.w);
		ob_i16(&o, r.h);
		memcpy(o.b + o.n, pat, 8);
		o.n += 8;
		ob_u32(&o, pixel_unpack(ink));
		ob_send(s, &o);
	}
	r = rect_isect(r, s->clip);
	for (int y = r.y; y < r.y + r.h; y++) {
		uint8_t row = pat[y & 7];
		if (!row)
			continue;
		uint32_t *px = s->px + (size_t)y * (size_t)s->pitch;
		for (int x = r.x; x < r.x + r.w; x++)
			if (row & (0x80 >> (x & 7)))
				px[x] = ink;
	}
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
	int rec = REC(dst);
	if (rec && src->rec == dst->rec && src->rid) {
		/* the viewer has both: tell it to copy (the clip is already applied) */
		struct opbuf o;
		ob_hdr(&o, GOP_BLIT, dst);
		memcpy(o.b + o.n, &src->rid, 2);
		o.n += 2;
		ob_i16(&o, r.x);
		ob_i16(&o, r.y);
		ob_i16(&o, sx + r.x - dx);
		ob_i16(&o, sy + r.y - dy);
		ob_i16(&o, r.w);
		ob_i16(&o, r.h);
		ob_send(dst, &o);
		rec = 0;
	}
	sx += r.x - dx;
	sy += r.y - dy;
	for (int j = 0; j < r.h; j++)
		memmove(dst->px + (size_t)(r.y + j) * (size_t)dst->pitch + r.x,
		    src->px + (size_t)(sy + j) * (size_t)src->pitch + sx, (size_t)r.w * 4);
	if (rec)	/* from a surface the viewer doesn't have: the pixels themselves */
		rec_pixels(dst, r);
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
	{ "disk", {
		"................", "................", "..kkkkkkkkkkkk..", ".kggggggggggggk.",
		".kgwwwwwwwwwwgk.", ".kgwddddddddwgk.", ".kgwwwwwwwwwwgk.", ".kggggggggggggk.",
		".kggggggggggggk.", ".kkkkkkkkkkkkkk.", ".kddddddddddddk.", ".kddddddddeedek.",
		".kddddddddddddk.", ".kkkkkkkkkkkkkk.", "................", "................" } },
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
	{ "cfile", {
		"..kkkkkkkkk.....", "..kwwwwwwwkk....", "..kwwwwwwwkwk...", "..kwwwwwwwkkkk..",
		"..kwwwwwwwwwwk..", "..kwwwbbbbwwwk..", "..kwwbbwwbbwwk..", "..kwbbwwwwwwwk..",
		"..kwbbwwwwwwwk..", "..kwbbwwwwwwwk..", "..kwwbbwwbbwwk..", "..kwwwbbbbwwwk..",
		"..kwwwwwwwwwwk..", "..kwwwwwwwwwwk..", "..kkkkkkkkkkkk..", "................" } },
	{ "pulse", {
		"kkkkkkkkkkkkkkkk", "kddddddddddddddk", "kkkkkkkkkkkkkkkk", "kkkkkkkkkkkkrrkk",
		"kkkkkkkkkkkkrrkk", "kkkkkkkkyykkyykk", "kkkkkkkkyykkyykk", "kkkkeekkeekkeekk",
		"kkkkeekkeekkeekk", "kkkkeekkeekkeekk", "kkeekkeekkeekkkk", "kkeekkeekkeekeek",
		"kkeekkeekkeekeek", "kkeekkeekkeekeek", "kddddddddddddddk", "kkkkkkkkkkkkkkkk" } },
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
		"kkkkkkkkkkkkkkkk", "................", "................", "................" } },	{ "control", {
		"kkkkkkkkkkkkkkkk", "knnnnnnnnnnnnnnk", "kggggggggggggggk", "kggdgggdgggdgggk",
		"kggdgggdgggdgggk", "kgkkkggdgggdgggk", "kggdgggdgggdgggk", "kggdgggdgkkkgggk",
		"kggdgggdgggdgggk", "kggdgkkkgggdgggk", "kggdgggdgggdgggk", "kggdgggdgggdgggk",
		"kggggggggggggggk", "kkkkkkkkkkkkkkkk", "................", "................" } },
	{ "clock", {
		"................", ".....kkkkkk.....", "...kkwwwwwwkk...", "..kwwwwkwwwwwk..",
		".kwwwwwkwwwwwwk.", ".kwwwwwkwwwwwwk.", "kwwwwwwkwwwwwwwk", "kwkwwwwkwwwwwkwk",
		"kwwwwwwkkkkkwwwk", "kwwwwwwwwwwwwwwk", ".kwwwwwwwwwwwwk.", ".kwwwwwwwwwwwwk.",
		"..kwwwwkwwwwwk..", "...kkwwwwwwkk...", ".....kkkkkk.....", "................" } },
	{ "calendar", {
		"................", "..k..k....k..k..", "kkkkkkkkkkkkkkkk", "krrrrrrrrrrrrrrk",
		"krrrrrrrrrrrrrrk", "kkkkkkkkkkkkkkkk", "kwwwwwwwwwwwwwwk", "kwdwdwdwdwdwdwwk",
		"kwwwwwwwwwwwwwwk", "kwdwdwdwkkwdwwwk", "kwwwwwwwkkwwwwwk", "kwdwdwdwdwdwdwwk",
		"kwwwwwwwwwwwwwwk", "kwdwdwdwwwwwwwwk", "kwwwwwwwwwwwwwwk", "kkkkkkkkkkkkkkkk" } },
	{ "mouse", {
		"........k.......", "........k.......", ".......k........", "......kkkk......",
		".....kwwkwk.....", "....kwwwkwwk....", "....kwwwkwwk....", "....kkkkkkkk....",
		"....kwwwwwwk....", "....kwwwwwwk....", "....kwwwwwwk....", "....kwwwwwwk....",
		"....kwwwwwwk....", ".....kwwwwk.....", "......kkkk......", "................" } },
	{ "keyboard", {
		"................", "................", "................", "kkkkkkkkkkkkkkkk",
		"kggggggggggggggk", "kgwkwkwkwkwkwkgk", "kggggggggggggggk", "kgwkwkwkwkwkwkgk",
		"kggggggggggggggk", "kgwkwwwwwwwkwkgk", "kggggggggggggggk", "kkkkkkkkkkkkkkkk",
		"................", "................", "................", "................" } },
	{ "palette", {
		"................", "....kkkkkkk.....", "..kkwwwwwwwkk...", ".kwwrrwwwbbwwk..",
		".kwwrrwwwbbwwwk.", "kwwwwwwwwwwwwwk.", "kweewwwwwwwyywk.", "kweewwwkkwwyywk.",
		"kwwwwwkwwkwwwwk.", "kwwwwwkwwkwwwk..", ".kwwwwwkkwwwk...", ".kwccwwwwwwk....",
		"..kwccwwwwk.....", "...kkkkkkk......", "................", "................" } },
	{ "eyes", {
		"................", "................", "..kkkk....kkkk..", ".kwwwwk..kwwwwk.",
		"kwwwwwwkkwwwwwwk", "kwwwwwwkkwwwwwwk", "kwwwkkwkkwwwkkwk", "kwwwkkwkkwwwkkwk",
		"kwwwwwwkkwwwwwwk", "kwwwwwwkkwwwwwwk", ".kwwwwk..kwwwwk.", "..kkkk....kkkk..",
		"................", "................", "................", "................" } },
	{ "cat", {
		"................", "..k.......k.....", "..kk.....kk.....", "..kwk...kwk.....",
		"..kwwkkkwwk.....", "..kwwwwwwwk.....", ".kwwkwwwkwwk....", ".kwwwwrwwwwk....",
		".kwwwkkkwwwk....", "..kwwwwwwwk.....", "...kwwwwwk...k..", "..kwwwwwwwk..kk.",
		"..kwwwwwwwwk.kwk", "..kwwkwwkwwwkwwk", "..kkkkkkkkkkkkk.", "................" } },
	{ "mine", {
		"................", ".......k........", "...k...k...k....", "....kkkkkkk.....",
		"...kkwwkkkkk....", "...kwwkkkkkk....", ".kkkkkkkkkkkkk..", "...kkkkkkkkk....",
		"...kkkkkkkkk....", "....kkkkkkk.....", "...k...k...k....", ".......k........",
		"................", "..ggggggggggggg.", "..gdddddddddddg.", "..ggggggggggggg." } },
	{ "calc", {
		"..kkkkkkkkkkkk..", "..kggggggggggk..", "..kgkkkkkkkkgk..", "..kgkeeeeeekgk..",
		"..kgkkkkkkkkgk..", "..kggggggggggk..", "..kgwwgwwgrrgk..", "..kgwwgwwgrrgk..",
		"..kggggggggggk..", "..kgwwgwwgwwgk..", "..kgwwgwwgwwgk..", "..kggggggggggk..",
		"..kgwwgwwgwwgk..", "..kgwwgwwgwwgk..", "..kggggggggggk..", "..kkkkkkkkkkkk.." } },
	{ "charmap", {
		"kkkkkkkkkkkkkkkk", "kwwwkwwwkwwwkwwk", "kwkwkwkwkkwkkwwk", "kwkwkwkkkwwwkwwk",
		"kkkkkkkkkkkkkkkk", "kwwwkwwwkwwwkwwk", "kwkkkwwwkkkwkwwk", "kwwwkwkwkwwwkwwk",
		"kkkkkkkkkkkkkkkk", "kwwwkwwwkwwwkwwk", "kwkwkkwkkwkwkwwk", "kwwwkwwwkwwwkwwk",
		"kkkkkkkkkkkkkkkk", "................", "................", "................" } },
	{ "tasks", {
		"kkkkkkkkkkkkkkkk", "knnnnnnnnnnnnnnk", "kwwwwwwwwwwwwwwk", "kwkkkkkkkkkkkwwk",
		"kwwwwwwwwwwwwwwk", "kbbbbbbbbbbbbbbk", "kwwwwwwwwwwwwwwk", "kwkkkkkkkkwwwwwk",
		"kwwwwwwwwwwwwwwk", "kwkkkkkkkkkkwwwk", "kwwwwwwwwwwwwwwk", "kggggggggggggggk",
		"kgkkkkgggkkkkkgk", "kggggggggggggggk", "kkkkkkkkkkkkkkkk", "................" } },
	{ "phone", {
		"................", "................", "..kkkkkkkkkkkk..", ".kddddddddddddk.",
		"kdddkkkkkkkkdddk", "kddk........kddk", ".kk..kkkkkk..kk.", "....kddddddk....",
		"...kddkddkddk...", "..kddddddddddk..", "..kddkddkddkdk..", "..kddddddddddk..",
		"..kddkddkddkdk..", "..kddddddddddk..", "..kkkkkkkkkkkk..", "................" } },
	{ "help", {
		"................", ".....kkkkkk.....", "...kkyyyyyykk...", "..kyyykkkkyyyk..",
		"..kyyk....kyyk..", "..kkkk....kyyk..", "..........kyyk..", "........kkyyk...",
		"......kkyyk.....", "......kyyk......", "......kyyk......", "......kkkk......",
		"................", "......kkkk......", "......kyyk......", "......kkkk......" } },
	{ "desktop", {
		".kkkkkkkkkkkkkk.", ".kggggggggggggk.", ".kgkkkkkkkkkkgk.", ".kgkttttttttkgk.",
		".kgktnnnntttkgk.", ".kgktwwwwtttkgk.", ".kgktwwwwtttkgk.", ".kgkttttttttkgk.",
		".kgkkkkkkkkkkgk.", ".kggggggggggggk.", ".kkkkkkkkkkkkkk.", ".....kggggk.....",
		"...kkkkkkkkkk...", "..kggggggggggk..", "..kkkkkkkkkkkk..", "................" } },
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
	int rec = REC(s);
	if (rec) {
		int idx = (int)(ic - icons);
		if (idx < 512 && !(s->rec->icon_sent[idx / 8] & (1 << (idx % 8)))) {
			/* the viewer hasn't seen this one: 16x16, 0 where it's clear */
			uint8_t b[11 + 2 + 1024];
			struct opbuf o;
			ob_hdr(&o, GOP_ICONDEF, s);
			memcpy(b, o.b, o.n);
			uint16_t i16 = (uint16_t)idx;
			memcpy(b + o.n, &i16, 2);
			for (int k = 0; k < 256; k++) {
				char c = ic->rows[k / 16][k % 16];
				uint32_t v = c == '.' ? 0 : 0xFF000000u | icon_colour(c);
				memcpy(b + o.n + 2 + k * 4, &v, 4);
			}
			s->rec->emit(s->rec, b, o.n + 2 + 1024);
			s->rec->icon_sent[idx / 8] |= (uint8_t)(1 << (idx % 8));
		}
		struct opbuf o;
		ob_hdr(&o, GOP_ICON, s);
		ob_i16(&o, x);
		ob_i16(&o, y);
		ob_i16(&o, idx);
		ob_send(s, &o);
		rec_depth++;
	}
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 16; i++) {
			char c = ic->rows[j][i];
			if (c == '.')
				continue;
			fill(s, x + i * 2, y + j * 2, 2, 2, col(icon_colour(c)));
		}
	if (rec)
		rec_depth--;
}
