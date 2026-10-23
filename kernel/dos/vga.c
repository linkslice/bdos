/*
 * vga.c - a VGA: sequencer, graphics controller, CRTC, attribute controller
 * and DAC, the four planes of video memory behind A0000, and a renderer
 * that turns whatever state the program has left it in into pixels.
 *
 * The geometry of graphics modes is worked out from the CRTC registers, so
 * tweaked modes (mode X's 320x240, unchained 256 colours) come out right.
 * Text and CGA modes live in ordinary memory at B8000; only the A0000
 * window goes through the planar logic. VESA banked 256-colour modes up to
 * 1024x768 use the same memory, as one linear megabyte.
 */
#include "dos.h"
#include "../gate.h"
#include "font.h"

const uint8_t *vga_font16(void) { return &font8x16[0][0]; }

/* 8x8 glyphs, squeezed out of the 8x16 ones */
static uint8_t font8[256][8];
static uint8_t font14[256][14];
static int fonts_made;

static void make_fonts(void)
{
	if (fonts_made)
		return;
	for (int c = 0; c < 256; c++) {
		for (int y = 0; y < 8; y++)
			font8[c][y] = font8x16[c][y * 2] | font8x16[c][y * 2 + 1];
		for (int y = 0; y < 14; y++)
			font14[c][y] = font8x16[c][y + 1];
	}
	fonts_made = 1;
}

const uint8_t *vga_font8(void) { make_fonts(); return &font8[0][0]; }
const uint8_t *vga_font14(void) { make_fonts(); return &font14[0][0]; }

static inline uint8_t *plane(struct vga *v, int p)
{
	return v->vram + (size_t)p * VGA_PLANE;
}

/* ---- the default palettes --------------------------------------------------------- */

static const uint8_t ega64[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };

static void dac_set(struct vga *v, int i, int r, int g, int b)
{
	v->dac[i][0] = (uint8_t)r;
	v->dac[i][1] = (uint8_t)g;
	v->dac[i][2] = (uint8_t)b;
}

static void default_dac(struct vga *v, int ega)
{
	if (ega) {
		/* the 64 EGA colours: rgbRGB */
		for (int i = 0; i < 64; i++) {
			int r = ((i >> 2) & 1) * 0x2A + ((i >> 5) & 1) * 0x15;
			int g = ((i >> 1) & 1) * 0x2A + ((i >> 4) & 1) * 0x15;
			int b = (i & 1) * 0x2A + ((i >> 3) & 1) * 0x15;
			dac_set(v, i, r, g, b);
		}
		return;
	}
	/* the VGA BIOS's 256: CGA colours, a grey ramp, then hue rings */
	static const uint8_t cga[16][3] = {
		{0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
		{21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63},
	};
	for (int i = 0; i < 16; i++)
		dac_set(v, i, cga[i][0], cga[i][1], cga[i][2]);
	static const uint8_t grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
	for (int i = 0; i < 16; i++)
		dac_set(v, 16 + i, grey[i], grey[i], grey[i]);
	/* three intensities x three saturations of a 24-step colour wheel */
	static const uint8_t lohi[3][3][2] = {
		{ { 0, 63 }, { 31, 63 }, { 45, 63 } },
		{ { 0, 28 }, { 14, 28 }, { 20, 28 } },
		{ { 0, 16 }, { 8, 16 }, { 11, 16 } },
	};
	int i = 32;
	for (int in = 0; in < 3; in++)
		for (int sat = 0; sat < 3; sat++) {
			int lo = lohi[in][sat][0], hi = lohi[in][sat][1];
			for (int ph = 0; ph < 6; ph++)
				for (int k = 0; k < 4; k++, i++) {
					int up = lo + (hi - lo) * k / 4, down = lo + (hi - lo) * (4 - k) / 4;
					int r, g, bl;
					switch (ph) {
					case 0: r = up; g = lo; bl = hi; break;
					case 1: r = hi; g = lo; bl = down; break;
					case 2: r = hi; g = up; bl = lo; break;
					case 3: r = down; g = hi; bl = lo; break;
					case 4: r = lo; g = hi; bl = up; break;
					default: r = lo; g = down; bl = hi; break;
					}
					dac_set(v, i, r, g, bl);
				}
		}
	for (; i < 256; i++)
		dac_set(v, i, 0, 0, 0);
}

/* ---- modes ------------------------------------------------------------------------- */

enum { VK_TEXT, VK_CGA4, VK_CGA2, VK_PLANAR, VK_256 };

struct mode_info {
	int mode, kind, w, h;
	uint8_t seq4, gc5, gc6, attr10, crtc1, crtc7, crtc9, crtc12, crtc13, crtc14, crtc17;
};

static const struct mode_info modes[] = {
	{ 0x00, VK_TEXT, 40, 25, 0x02, 0x10, 0x0E, 0x08, 0x27, 0x1F, 0x4F, 0x8F, 0x14, 0x1F, 0xA3 },
	{ 0x01, VK_TEXT, 40, 25, 0x02, 0x10, 0x0E, 0x08, 0x27, 0x1F, 0x4F, 0x8F, 0x14, 0x1F, 0xA3 },
	{ 0x02, VK_TEXT, 80, 25, 0x02, 0x10, 0x0E, 0x08, 0x4F, 0x1F, 0x4F, 0x8F, 0x28, 0x1F, 0xA3 },
	{ 0x03, VK_TEXT, 80, 25, 0x02, 0x10, 0x0E, 0x08, 0x4F, 0x1F, 0x4F, 0x8F, 0x28, 0x1F, 0xA3 },
	{ 0x04, VK_CGA4, 320, 200, 0x02, 0x30, 0x0F, 0x01, 0x27, 0x1F, 0xC1, 0x8F, 0x14, 0x00, 0xA2 },
	{ 0x05, VK_CGA4, 320, 200, 0x02, 0x30, 0x0F, 0x01, 0x27, 0x1F, 0xC1, 0x8F, 0x14, 0x00, 0xA2 },
	{ 0x06, VK_CGA2, 640, 200, 0x06, 0x00, 0x0D, 0x01, 0x4F, 0x1F, 0xC1, 0x8F, 0x28, 0x00, 0xC2 },
	{ 0x07, VK_TEXT, 80, 25, 0x02, 0x10, 0x0A, 0x0E, 0x4F, 0x1F, 0x4F, 0x8F, 0x28, 0x0F, 0xA3 },
	{ 0x0D, VK_PLANAR, 320, 200, 0x06, 0x00, 0x05, 0x01, 0x27, 0x1F, 0xC0, 0x8F, 0x14, 0x00, 0xE3 },
	{ 0x0E, VK_PLANAR, 640, 200, 0x06, 0x00, 0x05, 0x01, 0x4F, 0x1F, 0xC0, 0x8F, 0x28, 0x00, 0xE3 },
	{ 0x0F, VK_PLANAR, 640, 350, 0x06, 0x00, 0x05, 0x01, 0x4F, 0x1F, 0x40, 0x5D, 0x28, 0x00, 0xE3 },
	{ 0x10, VK_PLANAR, 640, 350, 0x06, 0x00, 0x05, 0x01, 0x4F, 0x1F, 0x40, 0x5D, 0x28, 0x00, 0xE3 },
	{ 0x11, VK_PLANAR, 640, 480, 0x06, 0x00, 0x05, 0x01, 0x4F, 0x3E, 0x40, 0xDF, 0x28, 0x00, 0xE3 },
	{ 0x12, VK_PLANAR, 640, 480, 0x06, 0x00, 0x05, 0x01, 0x4F, 0x3E, 0x40, 0xDF, 0x28, 0x00, 0xE3 },
	{ 0x13, VK_256, 320, 200, 0x0E, 0x40, 0x05, 0x41, 0x4F, 0x1F, 0x41, 0x8F, 0x28, 0x40, 0xA3 },
};

static const struct mode_info *find_mode(int m)
{
	for (size_t i = 0; i < NELEM(modes); i++)
		if (modes[i].mode == m)
			return &modes[i];
	return NULL;
}

/* VESA modes: all 256 colours, banked through A0000 */
static const struct { int mode, w, h; } vbe_modes[] = {
	{ 0x100, 640, 400 }, { 0x101, 640, 480 }, { 0x103, 800, 600 }, { 0x105, 1024, 768 },
};

static void update_window(struct dosbox *b)
{
	struct vga *v = &b->vga;
	struct x86 *c = &b->cpu;
	int map = (v->gc[6] >> 2) & 3;
	int graphics = v->gc[6] & 1;
	if (v->vbe_mode || (graphics && (map == 0 || map == 1))) {
		c->vga_lo = 0xA0000;
		c->vga_hi = 0xB0000;
	} else {
		c->vga_lo = c->vga_hi = 0;	/* text and CGA memory is just memory */
	}
}

void vga_set_mode(struct dosbox *b, int mode, int clear)
{
	struct vga *v = &b->vga;
	v->vbe_mode = 0;
	v->vbe_bank = 0;
	for (size_t i = 0; i < NELEM(vbe_modes); i++)
		if (vbe_modes[i].mode == mode) {
			v->vbe_mode = mode;
			v->width = vbe_modes[i].w;
			v->height = vbe_modes[i].h;
			mode = 0x13;
			break;
		}
	const struct mode_info *m = find_mode(mode);
	if (!m) {
		m = find_mode(3);
		mode = 3;
	}
	v->mode = v->vbe_mode ? v->vbe_mode : mode;
	v->latched_at = 0;
	memset(v->seq, 0, sizeof v->seq);
	memset(v->gc, 0, sizeof v->gc);
	memset(v->crtc, 0, sizeof v->crtc);
	v->seq[0] = 3;
	v->seq[1] = m->kind == VK_TEXT && m->w == 40 ? 0x08 : 0x00;
	v->seq[2] = 0x0F;
	v->seq[4] = m->seq4;
	v->gc[5] = m->gc5;
	v->gc[6] = m->gc6;
	v->gc[7] = 0x0F;
	v->gc[8] = 0xFF;
	v->crtc[1] = m->crtc1;
	v->crtc[7] = m->crtc7;
	v->crtc[9] = m->kind == VK_TEXT ? 0x4F : m->crtc9;
	v->crtc[0x0A] = 0x0D;
	v->crtc[0x0B] = 0x0E;
	v->crtc[0x12] = m->crtc12;
	v->crtc[0x13] = m->crtc13;
	v->crtc[0x14] = m->crtc14;
	v->crtc[0x17] = m->crtc17;
	v->attr[0x10] = m->attr10;
	v->attr[0x11] = 0;
	v->attr[0x12] = 0x0F;
	v->attr[0x13] = 0;
	v->attr[0x14] = 0;
	int ega = m->kind == VK_PLANAR || m->kind == VK_TEXT;
	for (int i = 0; i < 16; i++)
		v->attr[i] = ega ? ega64[i] : (uint8_t)i;
	if (m->kind == VK_TEXT)		/* text: the attribute bytes map straight on */
		for (int i = 0; i < 16; i++)
			v->attr[i] = ega64[i];
	default_dac(v, 0);
	if (ega && m->kind == VK_PLANAR)
		default_dac(v, 1);
	if (m->kind == VK_TEXT) {
		/* text colours via the EGA palette numbers */
		default_dac(v, 1);
	}
	v->dac_mask = 0xFF;
	if (clear) {
		memset(v->vram, 0, 4 * VGA_PLANE);
		if (m->kind == VK_TEXT) {
			uint32_t base = mode == 7 ? 0xB0000 : 0xB8000;
			for (int i = 0; i < 0x4000; i += 2) {
				mbw(b, base + (uint32_t)i, ' ');
				mbw(b, base + (uint32_t)i + 1, 7);
			}
		} else if (m->kind == VK_CGA4 || m->kind == VK_CGA2) {
			for (uint32_t i = 0; i < 0x4000; i++)
				mbw(b, 0xB8000 + i, 0);
		}
	}
	update_window(b);
	v->dirty = 1;
}

int vga_is_text(struct dosbox *b)
{
	struct vga *v = &b->vga;
	return !v->vbe_mode && !(v->attr[0x10] & 1) && !(v->gc[6] & 1);
}

static int vga_kind(struct dosbox *b)
{
	struct vga *v = &b->vga;
	if (v->vbe_mode)
		return VK_256;
	if (vga_is_text(b))
		return VK_TEXT;
	if (v->mode == 4 || v->mode == 5)
		return VK_CGA4;
	if (v->mode == 6)
		return VK_CGA2;
	if (v->gc[5] & 0x40)
		return VK_256;
	return VK_PLANAR;
}

int vga_text_size(struct dosbox *b, int *cols, int *rows)
{
	*cols = mw(b, 0x44A);
	if (*cols < 1 || *cols > 132)
		*cols = 80;
	*rows = mb(b, 0x484) + 1;
	if (*rows < 2 || *rows > 60)
		*rows = 25;
	return vga_is_text(b);
}

/* ---- geometry of graphics modes --------------------------------------------------- */

static int vde(struct vga *v)
{
	return (v->crtc[0x12] | ((v->crtc[7] & 2) << 7) | ((v->crtc[7] & 0x40) << 3)) + 1;
}

static int line_rep(struct vga *v)
{
	return ((v->crtc[9] & 0x1F) + 1) * ((v->crtc[9] & 0x80) ? 2 : 1);
}

void vga_screen_size(struct dosbox *b, int *w, int *h)
{
	struct vga *v = &b->vga;
	switch (vga_kind(b)) {
	case VK_TEXT: {
		int c, r;
		vga_text_size(b, &c, &r);
		*w = c * 8;
		*h = r * (c == 40 || r <= 25 ? 16 : 8);
		if (r > 25)
			*h = r * 8;
		return;
	}
	case VK_CGA4: *w = 320; *h = 200; return;
	case VK_CGA2: *w = 640; *h = 200; return;
	case VK_256:
		if (v->vbe_mode) {
			*w = v->width;
			*h = v->height;
			return;
		}
		*w = (v->crtc[1] + 1) * 4;
		*h = vde(v) / line_rep(v);
		break;
	default:
		*w = (v->crtc[1] + 1) * 8;
		*h = vde(v) / line_rep(v);
	}
	if (*w < 8 || *w > 1280)
		*w = 320;
	if (*h < 8 || *h > 1024)
		*h = 200;
}

/* ---- the A0000 window ------------------------------------------------------------- */

static inline uint32_t rot(uint8_t v, int n)
{
	n &= 7;
	return (uint8_t)((v >> n) | (v << (8 - n)));
}

uint8_t vga_rd(struct x86 *c, uint32_t a)
{
	struct dosbox *b = BOX(c);
	struct vga *v = &b->vga;
	uint32_t off = a - 0xA0000;
	if (v->vbe_mode) {
		uint32_t l = (uint32_t)v->vbe_bank * 65536 + off;
		if (l >= 4 * VGA_PLANE)
			return 0xFF;
		return plane(v, (int)(l & 3))[l >> 2];
	}
	if (v->seq[4] & 8) {		/* chain 4 */
		return plane(v, (int)(off & 3))[(off >> 2) & (VGA_PLANE - 1)];
	}
	off &= 0xFFFF;
	for (int p = 0; p < 4; p++)
		v->latch[p] = plane(v, p)[off];
	if (v->gc[5] & 8) {		/* read mode 1: colour compare */
		uint8_t r = 0;
		for (int bit = 0; bit < 8; bit++) {
			int match = 1;
			for (int p = 0; p < 4; p++) {
				if (!(v->gc[7] & (1 << p)))
					continue;
				int pb = (v->latch[p] >> bit) & 1, want = (v->gc[2] >> p) & 1;
				if (pb != want)
					match = 0;
			}
			if (match)
				r |= (uint8_t)(1 << bit);
		}
		return r;
	}
	if (v->gc[5] & 0x10)		/* odd/even */
		return plane(v, (int)((v->gc[4] & 2) | (off & 1)))[off];
	return v->latch[v->gc[4] & 3];
}

static uint8_t logic(struct vga *v, uint8_t val, uint8_t latch)
{
	switch ((v->gc[3] >> 3) & 3) {
	case 1: return val & latch;
	case 2: return val | latch;
	case 3: return val ^ latch;
	}
	return val;
}

void vga_wr(struct x86 *c, uint32_t a, uint8_t val)
{
	struct dosbox *b = BOX(c);
	struct vga *v = &b->vga;
	uint32_t off = a - 0xA0000;
	v->dirty = 1;
	if (v->vbe_mode) {
		uint32_t l = (uint32_t)v->vbe_bank * 65536 + off;
		if (l < 4 * VGA_PLANE)
			plane(v, (int)(l & 3))[l >> 2] = val;
		return;
	}
	if (v->seq[4] & 8) {
		int p = (int)(off & 3);
		if (v->seq[2] & (1 << p))
			plane(v, p)[(off >> 2) & (VGA_PLANE - 1)] = val;
		return;
	}
	off &= 0xFFFF;
	uint8_t mask = v->gc[8];
	int wm = v->gc[5] & 3;
	if (v->gc[5] & 0x10) {		/* odd/even: even addresses to planes 0/2 */
		int p = (int)(off & 1);
		if (v->seq[2] & (1 << p))
			plane(v, p)[off] = val;
		return;
	}
	for (int p = 0; p < 4; p++) {
		if (!(v->seq[2] & (1 << p)))
			continue;
		uint8_t latch = v->latch[p], out;
		switch (wm) {
		case 0: {
			uint8_t d = (uint8_t)rot(val, v->gc[3]);
			if (v->gc[1] & (1 << p))
				d = (v->gc[0] & (1 << p)) ? 0xFF : 0;
			d = logic(v, d, latch);
			out = (uint8_t)((d & mask) | (latch & ~mask));
			break;
		}
		case 1:
			out = latch;
			break;
		case 2: {
			uint8_t d = (val & (1 << p)) ? 0xFF : 0;
			d = logic(v, d, latch);
			out = (uint8_t)((d & mask) | (latch & ~mask));
			break;
		}
		default: {
			uint8_t m3 = (uint8_t)(rot(val, v->gc[3]) & mask);
			uint8_t d = (v->gc[0] & (1 << p)) ? 0xFF : 0;
			d = logic(v, d, latch);
			out = (uint8_t)((d & m3) | (latch & ~m3));
		}
		}
		plane(v, p)[off] = out;
	}
}

/* ---- ports ---------------------------------------------------------------------------- */

uint32_t vga_in(struct dosbox *b, uint16_t port)
{
	struct vga *v = &b->vga;
	switch (port) {
	case 0x3C0: return v->attr_idx;
	case 0x3C1: return v->attr[v->attr_idx & 0x1F];
	case 0x3C2: return 0x10;
	case 0x3C4: return v->seq_idx;
	case 0x3C5: return v->seq[v->seq_idx & 7];
	case 0x3C6: return v->dac_mask;
	case 0x3C7: return 3;
	case 0x3C8: return v->dac_widx;
	case 0x3C9: {
		uint8_t r = v->dac[v->dac_ridx][v->dac_rsub];
		if (++v->dac_rsub == 3) {
			v->dac_rsub = 0;
			v->dac_ridx++;
		}
		return r;
	}
	case 0x3CC: return v->misc;
	case 0x3CE: return v->gc_idx;
	case 0x3CF: return v->gc[v->gc_idx & 15];
	case 0x3B4: case 0x3D4: return v->crtc_idx;
	case 0x3B5: case 0x3D5: return v->crtc[v->crtc_idx & 31];
	case 0x3BA: case 0x3DA: {
		v->attr_flip = 0;
		/* 70 Hz: vertical retrace for the last 5%, horizontal blanking often */
		uint64_t t = dos_now(b);
		uint64_t frame = 14286, in = t % frame;
		uint8_t s = 0;
		if (in >= frame - 700) {
			s |= 0x09;
			v->shown_start = (uint32_t)(v->crtc[0x0C] << 8 | v->crtc[0x0D]);
			v->latched_at = t;
		}
		else if ((t % 32) < 6)
			s |= 0x01;
		return s;
	}
	}
	return 0xFF;
}

void vga_out(struct dosbox *b, uint16_t port, uint8_t x)
{
	struct vga *v = &b->vga;
	switch (port) {
	case 0x3C0:
		if (!v->attr_flip) {
			v->attr_idx = x;
		} else {
			v->attr[v->attr_idx & 0x1F] = x;
			v->dirty = 1;
		}
		v->attr_flip ^= 1;
		return;
	case 0x3C2: v->misc = x; return;
	case 0x3C4: v->seq_idx = x; return;
	case 0x3C5:
		v->seq[v->seq_idx & 7] = x;
		v->dirty = 1;
		return;
	case 0x3C6: v->dac_mask = x; v->dirty = 1; return;
	case 0x3C7: v->dac_ridx = x; v->dac_rsub = 0; return;
	case 0x3C8: v->dac_widx = x; v->dac_wsub = 0; return;
	case 0x3C9:
		v->dac[v->dac_widx][v->dac_wsub] = x & 0x3F;
		if (++v->dac_wsub == 3) {
			v->dac_wsub = 0;
			v->dac_widx++;
		}
		v->dirty = 1;
		return;
	case 0x3CE: v->gc_idx = x; return;
	case 0x3CF:
		v->gc[v->gc_idx & 15] = x;
		if ((v->gc_idx & 15) == 6)
			update_window(b);
		v->dirty = 1;
		return;
	case 0x3B4: case 0x3D4: v->crtc_idx = x; return;
	case 0x3B5: case 0x3D5:
		if ((v->crtc_idx & 31) < 8 && (v->crtc[0x11] & 0x80) && (v->crtc_idx & 31) != 7)
			return;		/* write-protected */
		v->crtc[v->crtc_idx & 31] = x;
		v->dirty = 1;
		return;
	}
}

/* ---- VESA --------------------------------------------------------------------------- */

#define VBE_INFO_OFF	0x7E00	/* in the video BIOS segment: our strings and lists */

int vga_vbe(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	struct vga *v = &b->vga;
	uint8_t fn = AL(c);
	uint32_t es_di = c->s[S_ES].base + DI(c);
	switch (fn) {
	case 0x00: {		/* controller information */
		for (int i = 0; i < 256; i++)
			x86_wr8(c, es_di + (uint32_t)i, 0);
		x86_wr8(c, es_di, 'V');
		x86_wr8(c, es_di + 1, 'E');
		x86_wr8(c, es_di + 2, 'S');
		x86_wr8(c, es_di + 3, 'A');
		x86_wr16(c, es_di + 4, 0x0102);
		x86_wr16(c, es_di + 6, VBE_INFO_OFF);		/* OEM string */
		x86_wr16(c, es_di + 8, VBIOS_SEG);
		x86_wr32(c, es_di + 10, 0);
		x86_wr16(c, es_di + 14, VBE_INFO_OFF + 0x20);	/* mode list */
		x86_wr16(c, es_di + 16, VBIOS_SEG);
		x86_wr16(c, es_di + 18, 16);			/* 1 MB in 64K units */
		break;
	}
	case 0x01: {		/* mode information */
		int w = 0, h = 0;
		for (size_t i = 0; i < NELEM(vbe_modes); i++)
			if (vbe_modes[i].mode == (CX(c) & 0x1FF)) {
				w = vbe_modes[i].w;
				h = vbe_modes[i].h;
			}
		if (!w) {
			set16(c, R_EAX, 0x014F);
			return 1;
		}
		for (int i = 0; i < 256; i++)
			x86_wr8(c, es_di + (uint32_t)i, 0);
		x86_wr16(c, es_di + 0, 0x001B);		/* supported, colour, graphics, BIOS output */
		x86_wr8(c, es_di + 2, 0x07);		/* window A: there, readable, writable */
		x86_wr16(c, es_di + 4, 64);		/* granularity, KB */
		x86_wr16(c, es_di + 6, 64);		/* window size, KB */
		x86_wr16(c, es_di + 8, 0xA000);
		x86_wr16(c, es_di + 12, ROM_VBE_BANK);	/* window function */
		x86_wr16(c, es_di + 14, BIOS_SEG);
		x86_wr16(c, es_di + 16, (uint16_t)w);	/* bytes per line */
		x86_wr16(c, es_di + 18, (uint16_t)w);
		x86_wr16(c, es_di + 20, (uint16_t)h);
		x86_wr8(c, es_di + 22, 8);
		x86_wr8(c, es_di + 23, 16);
		x86_wr8(c, es_di + 24, 1);		/* planes */
		x86_wr8(c, es_di + 25, 8);		/* bits per pixel */
		x86_wr8(c, es_di + 26, 1);		/* banks */
		x86_wr8(c, es_di + 27, 4);		/* packed pixel */
		x86_wr8(c, es_di + 29, (uint8_t)(1048576 / (w * h) - 1));	/* image pages */
		x86_wr8(c, es_di + 30, 1);
		break;
	}
	case 0x02:
		if ((BX(c) & 0x1FF) < 0x100)
			vga_set_mode(b, BX(c) & 0x7F, !(BX(c) & 0x8000));
		else {
			int ok = 0;
			for (size_t i = 0; i < NELEM(vbe_modes); i++)
				ok |= vbe_modes[i].mode == (BX(c) & 0x1FF);
			if (!ok) {
				set16(c, R_EAX, 0x014F);
				return 1;
			}
			vga_set_mode(b, BX(c) & 0x1FF, !(BX(c) & 0x8000));
			mbw(b, 0x449, 0x13);
		}
		break;
	case 0x03:
		set16(c, R_EBX, (uint16_t)(v->vbe_mode ? v->vbe_mode : v->mode));
		break;
	case 0x05:
		if (BH(c) == 0)
			v->vbe_bank = DL(c) & 15;
		else
			set16(c, R_EDX, (uint16_t)v->vbe_bank);
		v->dirty = 1;
		break;
	case 0x06:
		set16(c, R_EBX, (uint16_t)(v->vbe_mode ? v->width : 320));
		set16(c, R_ECX, (uint16_t)(v->vbe_mode ? v->width : 320));
		set16(c, R_EDX, (uint16_t)(v->vbe_mode ? (int)(1048576 / v->width) : 200));
		break;
	case 0x07:
		if (BL(c) == 0 || BL(c) == 0x80) {
			uint32_t start = (uint32_t)DX(c) * (uint32_t)(v->vbe_mode ? v->width : 320) + CX(c);
			v->crtc[0x0C] = (uint8_t)(start >> 10);
			v->crtc[0x0D] = (uint8_t)(start >> 2);
			v->shown_start = start >> 2;
			v->latched_at = dos_now(b);
			v->dirty = 1;
		} else {
			set16(c, R_ECX, 0);
			set16(c, R_EDX, 0);
		}
		break;
	case 0x08:
		set16(c, R_EBX, 0x0600);	/* the DAC is 6 bits */
		break;
	default:
		set16(c, R_EAX, 0x014F);
		return 1;
	}
	set16(c, R_EAX, 0x004F);
	return 1;
}

void vga_vbe_rom(struct dosbox *b)
{
	const char *oem = "bdos VGA";
	for (int i = 0; oem[i]; i++)
		mbw(b, sl(VBIOS_SEG, (uint16_t)(VBE_INFO_OFF + i)), (uint8_t)oem[i]);
	uint32_t list = sl(VBIOS_SEG, VBE_INFO_OFF + 0x20);
	for (size_t i = 0; i < NELEM(vbe_modes); i++)
		mww(b, list + (uint32_t)i * 2, (uint16_t)vbe_modes[i].mode);
	mww(b, list + (uint32_t)NELEM(vbe_modes) * 2, 0xFFFF);
}

/* ---- rendering ------------------------------------------------------------------------ */

static uint32_t dac_px(struct vga *v, int i)
{
	i &= v->dac_mask;
	uint32_t r = v->dac[i][0], g = v->dac[i][1], bl = v->dac[i][2];
	r = (r << 2) | (r >> 4);
	g = (g << 2) | (g >> 4);
	bl = (bl << 2) | (bl >> 4);
	return col(r << 16 | g << 8 | bl);
}

/* a 4-bit colour through the attribute controller to the DAC */
static int attr_to_dac(struct vga *v, int c4)
{
	int a = v->attr[c4 & 15] & 0x3F;
	if (v->attr[0x10] & 0x80)
		return (a & 0x0F) | ((v->attr[0x14] & 0x0F) << 4);
	return a | ((v->attr[0x14] & 0x0C) << 4);
}

/* draw a w x h frame through get(x, y) -> pixel, scaled to fit the area */
struct frame {
	int w, h;
	uint32_t *px;		/* w * h pixels */
};

/*
 * Where a w x h picture goes in an area: as big as it fits, the same shape.
 * Whole multiples when they come near filling it (crisp pixels, fullscreen
 * and windows sized for the mode), any size when the window was resized to
 * something else.
 */
int vga_scale_mode;	/* dos.scale: 0 sharp, 1 smooth, 2 stretch */

struct rect vga_fit(struct rect area, int w, int h)
{
	if (w < 1 || h < 1 || area.w < 1 || area.h < 1)
		return area;
	if (vga_scale_mode == 2)
		return area;
	int dw, dh;
	if ((int64_t)area.w * h <= (int64_t)area.h * w) {
		dw = area.w;
		dh = (int)((int64_t)area.w * h / w);
	} else {
		dh = area.h;
		dw = (int)((int64_t)area.h * w / h);
	}
	int si = MIN(area.w / w, area.h / h);
	if (vga_scale_mode == 0 && si >= 1 && si * w * 100 >= dw * 85) {
		dw = si * w;
		dh = si * h;
	}
	return (struct rect){ area.x + (area.w - dw) / 2, area.y + (area.h - dh) / 2, dw, dh };
}

static void blit_scaled(struct surface *s, struct rect area, struct frame *f)
{
	struct rect r = vga_fit(area, f->w, f->h);
	/* the bars around it */
	if (r.y > area.y)
		fill(s, area.x, area.y, area.w, r.y - area.y, col(0));
	if (r.y + r.h < area.y + area.h)
		fill(s, area.x, r.y + r.h, area.w, area.y + area.h - r.y - r.h, col(0));
	if (r.x > area.x)
		fill(s, area.x, r.y, r.x - area.x, r.h, col(0));
	if (r.x + r.w < area.x + area.w)
		fill(s, r.x + r.w, r.y, area.x + area.w - r.x - r.w, r.h, col(0));
	int xs = r.w / f->w, ys = r.h / f->h;
	if (xs >= 1 && xs * f->w == r.w && ys * f->h == r.h) {
		/* whole multiples */
		for (int y = 0; y < f->h; y++) {
			uint32_t *src = f->px + y * f->w;
			for (int k = 0; k < ys; k++) {
				int dy = r.y + y * ys + k;
				if (dy < 0 || dy >= s->h)
					continue;
				uint32_t *row = s->px + (size_t)dy * (size_t)s->pitch + r.x;
				if (xs == 1) {
					memcpy(row, src, (size_t)f->w * 4);
				} else {
					for (int x = 0; x < f->w; x++)
						for (int q = 0; q < xs; q++)
							row[x * xs + q] = src[x];
				}
			}
		}
		return;
	}
	/* any other size: each pixel of the window from the nearest of the picture */
	for (int y = 0; y < r.h; y++) {
		int dy = r.y + y;
		if (dy < 0 || dy >= s->h)
			continue;
		const uint32_t *src = f->px + (size_t)(y * f->h / r.h) * (size_t)f->w;
		uint32_t *row = s->px + (size_t)dy * (size_t)s->pitch + r.x;
		for (int x = 0; x < r.w; x++)
			row[x] = src[x * f->w / r.w];
	}
}

static uint32_t *frame_alloc(int n)
{
	return kmalloc((size_t)n * 4);
}

void vga_render(struct dosbox *b, struct surface *s, struct rect area)
{
	struct vga *v = &b->vga;
	int w, h;
	vga_screen_size(b, &w, &h);
	struct frame f = { w, h, frame_alloc(w * h) };
	if (!f.px)
		return;
	uint32_t pal[256];
	for (int i = 0; i < 256; i++)
		pal[i] = dac_px(v, i);
	int kind = vga_kind(b);
	uint32_t start = (uint32_t)(v->crtc[0x0C] << 8 | v->crtc[0x0D]);
	/* a program that waits for retrace sees its flips at retrace; one that
	 * doesn't (or hasn't lately) gets the start address as it is */
	if (v->latched_at && dos_now(b) - v->latched_at < 100000)
		start = v->shown_start;
	switch (kind) {
	case VK_TEXT: {
		make_fonts();
		int cols, rows;
		vga_text_size(b, &cols, &rows);
		int ch_h = h / rows;
		uint32_t base = v->mode == 7 ? 0xB0000 : 0xB8000;
		base += (uint32_t)mw(b, 0x44E);		/* the active page */
		int blink_on = (uptime_ms() / 267) & 1;
		int cursor = (v->crtc[0x0E] << 8 | v->crtc[0x0F]) - (int)(mw(b, 0x44E) / 2);
		int cs = v->crtc[0x0A] & 0x1F, ce = v->crtc[0x0B] & 0x1F;
		for (int r = 0; r < rows; r++)
			for (int cc = 0; cc < cols; cc++) {
				uint32_t a = base + (uint32_t)(r * cols + cc) * 2;
				uint8_t ch = mb(b, a), at = mb(b, a + 1);
				int fg = at & 15, bg = (at >> 4) & 15;
				if (v->attr[0x10] & 8) {	/* blink instead of bright backgrounds */
					bg &= 7;
					if ((at & 0x80) && !blink_on)
						fg = bg;
				}
				uint32_t pf = pal[attr_to_dac(v, fg)], pb = pal[attr_to_dac(v, bg)];
				const uint8_t *g = ch_h >= 16 ? font8x16[ch] : ch_h >= 14 ? font14[ch] : font8[ch];
				int gh = ch_h >= 16 ? 16 : ch_h >= 14 ? 14 : 8;
				for (int y = 0; y < ch_h; y++) {
					uint8_t bits = y < gh ? g[y] : 0;
					if (r * cols + cc == cursor && !(cs & 0x20) && blink_on) {
						int sl_ = y * 16 / MAX(1, ch_h);
						if (sl_ >= cs && sl_ <= ce)
							bits = 0xFF;
					}
					uint32_t *row = f.px + (r * ch_h + y) * w + cc * 8;
					for (int x = 0; x < 8; x++)
						row[x] = (bits & (0x80 >> x)) ? pf : pb;
				}
			}
		break;
	}
	case VK_CGA4: {
		static const int pals[2][2][4] = {
			{ { 0, 2, 4, 6 }, { 0, 10, 12, 14 } },
			{ { 0, 3, 5, 7 }, { 0, 11, 13, 15 } },
		};
		uint8_t sel = mb(b, 0x466);		/* the CGA colour select byte */
		int p = (sel >> 5) & 1, bright = (sel >> 4) & 1;
		if (v->mode == 5)
			p = 1;
		int cmap[4];
		for (int i = 0; i < 4; i++)
			cmap[i] = pals[p][bright][i];
		cmap[0] = sel & 15;
		static const uint32_t cga[16] = { 0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA,
		    0xAA5500, 0xAAAAAA, 0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF,
		    0xFFFF55, 0xFFFFFF };
		for (int y = 0; y < 200; y++)
			for (int x = 0; x < 320; x++) {
				uint8_t by = mb(b, 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 4));
				int px = (by >> (6 - (x & 3) * 2)) & 3;
				f.px[y * w + x] = col(cga[cmap[px]]);
			}
		break;
	}
	case VK_CGA2: {
		uint32_t fg = col(0xFFFFFF), bg = col(0);
		for (int y = 0; y < 200; y++)
			for (int x = 0; x < 640; x++) {
				uint8_t by = mb(b, 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 8));
				f.px[y * w + x] = (by & (0x80 >> (x & 7))) ? fg : bg;
			}
		break;
	}
	case VK_256: {
		if (v->vbe_mode) {
			uint32_t st = start * 4;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint32_t l = st + (uint32_t)(y * w + x);
					if (l >= 4 * VGA_PLANE)
						l = 0;
					f.px[y * w + x] = pal[plane(v, (int)(l & 3))[l >> 2]];
				}
			break;
		}
		int stride = v->crtc[0x13] * 2;
		if (v->seq[4] & 8) {		/* chain 4: mode 13h */
			uint32_t st = start * 4;
			int lstride = stride * 4;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint32_t l = (st + (uint32_t)(y * lstride + x)) & (4 * VGA_PLANE - 1);
					f.px[y * w + x] = pal[plane(v, (int)(l & 3))[(l >> 2) & (VGA_PLANE - 1)]];
				}
		} else {			/* unchained: mode X and friends */
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++) {
					uint32_t idx = (start + (uint32_t)(y * stride + x / 4)) & (VGA_PLANE - 1);
					f.px[y * w + x] = pal[plane(v, x & 3)[idx]];
				}
		}
		break;
	}
	default: {			/* 16-colour planar */
		int stride = v->crtc[0x13] * 2;
		uint32_t dacmap[16];
		for (int i = 0; i < 16; i++)
			dacmap[i] = pal[attr_to_dac(v, i & v->attr[0x12])];
		uint8_t *p0 = plane(v, 0), *p1 = plane(v, 1), *p2 = plane(v, 2), *p3 = plane(v, 3);
		for (int y = 0; y < h; y++) {
			uint32_t rowoff = start + (uint32_t)(y * stride);
			for (int x = 0; x < w; x += 8) {
				uint32_t i = (rowoff + (uint32_t)(x >> 3)) & 0xFFFF;
				uint8_t a0 = p0[i], a1 = p1[i], a2 = p2[i], a3 = p3[i];
				for (int k = 0; k < 8 && x + k < w; k++) {
					int bit = 7 - k;
					int cidx = ((a0 >> bit) & 1) | (((a1 >> bit) & 1) << 1) |
					    (((a2 >> bit) & 1) << 2) | (((a3 >> bit) & 1) << 3);
					f.px[y * w + x + k] = dacmap[cidx];
				}
			}
		}
	}
	}
	blit_scaled(s, area, &f);
	kfree(f.px);
	v->dirty = 0;
}

int vga_init(struct dosbox *b)
{
	b->vga.vram = kzalloc(4 * VGA_PLANE);
	if (!b->vga.vram)
		return -1;
	make_fonts();
	vga_set_mode(b, 3, 0);
	return 0;
}

void vga_free(struct dosbox *b)
{
	kfree(b->vga.vram);
	b->vga.vram = NULL;
}
