/*
 * bios.c - the ROM BIOS: the stubs the interrupt vectors point at, the BIOS
 * data area, and the services (video, keyboard, clock, system, mouse).
 *
 * Most vectors point at F000:n*4, which holds "F1 nn CF": a call into the
 * C code here, then IRET. Results that BIOSes return in flags (CF, ZF) are
 * written into the caller's FLAGS on the stack, so the IRET hands them
 * back. The timer and keyboard interrupts are real machine code, as on a
 * PC, so programs that hook them and chain to the old handler work.
 */
#include "dos.h"
#include "../gate.h"

const uint8_t *vga_font16(void);
const uint8_t *vga_font8(void);
const uint8_t *vga_font14(void);
void vga_vbe_rom(struct dosbox *b);

#define BDA 0x400

static void rom(struct dosbox *b, uint16_t off, const uint8_t *code, size_t n)
{
	for (size_t i = 0; i < n; i++)
		mbw(b, sl(BIOS_SEG, (uint16_t)(off + i)), code[i]);
}

static void set_vec(struct dosbox *b, int n, uint16_t seg, uint16_t off)
{
	mww(b, (uint32_t)n * 4, off);
	mww(b, (uint32_t)n * 4 + 2, seg);
}

/* the caller's FLAGS, pushed by INT */
void bios_set_flag(struct x86 *c, uint32_t f, int on)
{
	int big = c->s[S_SS].big;
	uint32_t sp = big ? c->r[R_ESP] : c->r[R_ESP] & 0xFFFF;
	int pm32 = (c->cr0 & CR0_PE) && c->s[S_CS].big;
	uint32_t a = c->s[S_SS].base + sp + (pm32 ? 8 : 4);
	uint16_t v = x86_rd16(c, a);
	v = on ? (uint16_t)(v | f) : (uint16_t)(v & ~f);
	x86_wr16(c, a, v);
	x86_set_flags(c, on ? f : 0, f);
}

static void cf(struct x86 *c, int on)
{
	bios_set_flag(c, FL_CF, on);
}

/* ---- the ROM ------------------------------------------------------------------------ */

static const uint8_t int8_code[] = {
	0x50,				/* push ax */
	0x1E,				/* push ds */
	0x31, 0xC0,			/* xor ax,ax */
	0x8E, 0xD8,			/* mov ds,ax */
	0xFF, 0x06, 0x6C, 0x04,		/* inc word [046C] */
	0x75, 0x04,			/* jnz +4 */
	0xFF, 0x06, 0x6E, 0x04,		/* inc word [046E] */
	0xCD, 0x1C,			/* int 1Ch */
	0xB0, 0x20,			/* mov al,20h */
	0xE6, 0x20,			/* out 20h,al */
	0x1F,				/* pop ds */
	0x58,				/* pop ax */
	0xCF,				/* iret */
};

static const uint8_t int9_code[] = {
	0x50,				/* push ax */
	0xE4, 0x60,			/* in al,60h */
	0xF1, HLE_INT9,			/* the C keyboard handler */
	0xB0, 0x20,			/* mov al,20h */
	0xE6, 0x20,			/* out 20h,al */
	0x58,				/* pop ax */
	0xCF,				/* iret */
};

void bios_init(struct dosbox *b)
{
	/* every vector: F1 nn IRET */
	for (int n = 0; n < 256; n++) {
		uint8_t stub[4] = { 0xF1, (uint8_t)n, 0xCF, 0x90 };
		rom(b, (uint16_t)(ROM_VECTORS + n * 4), stub, 4);
		set_vec(b, n, BIOS_SEG, (uint16_t)(ROM_VECTORS + n * 4));
	}
	rom(b, ROM_INT8, int8_code, sizeof int8_code);
	rom(b, ROM_INT9, int9_code, sizeof int9_code);
	static const uint8_t iret[] = { 0xCF };
	rom(b, ROM_INT1C, iret, 1);
	static const uint8_t mret[] = { 0xF1, HLE_MOUSE_RET };
	rom(b, ROM_MOUSE_RET, mret, 2);
	static const uint8_t xms[] = { 0xF1, HLE_XMS, 0xCB };			/* ... RETF */
	rom(b, ROM_XMS, xms, 3);
	static const uint8_t dsw[] = { 0xF1, HLE_DPMI_SW, 0xCB };
	rom(b, ROM_DPMI_SW, dsw, 3);
	static const uint8_t dret[] = { 0xF1, HLE_DPMI_RET };
	rom(b, ROM_DPMI_RET, dret, 2);
	static const uint8_t ex[] = { 0xF1, HLE_EXIT };
	rom(b, ROM_EXIT, ex, 2);
	static const uint8_t idle[] = { 0xFB, 0xF4, 0xEB, 0xFC };		/* sti; hlt; jmp $-2 */
	rom(b, ROM_IDLE, idle, 4);
	static const uint8_t vbe[] = { 0xF1, HLE_VBE_BANK, 0xCB };
	rom(b, ROM_VBE_BANK, vbe, 3);
	dpmi_rom(b);
	/* INT 15h C0h's table: AT-class, extended BIOS area, RTC, keyboard intercept */
	static const uint8_t conf[] = { 0x08, 0x00, 0xFC, 0x01, 0x00, 0x74, 0x40, 0x00, 0x00, 0x00 };
	rom(b, ROM_SYSCONF, conf, sizeof conf);
	/* the reset vector, the date and the model byte */
	static const uint8_t reset[] = { 0xCD, 0x19 };	/* INT 19h: back to bdos */
	rom(b, 0xFFF0, reset, 2);
	rom(b, 0xFFF5, (const uint8_t *)"01/01/94", 8);
	mbw(b, sl(BIOS_SEG, 0xFFFE), 0xFC);

	set_vec(b, 0x08, BIOS_SEG, ROM_INT8);
	set_vec(b, 0x09, BIOS_SEG, ROM_INT9);
	set_vec(b, 0x1C, BIOS_SEG, ROM_INT1C);
	/*
	 * Vectors nobody services all point at one IRET, F000:FF53, as on a real
	 * PC: DOS extenders (DOS/4GW, so Doom) find free vectors by looking for
	 * ones equal to their neighbours, and search forever if none are.
	 */
	rom(b, ROM_DUMMY_IRET, iret, 1);
	for (int n = 0x30; n < 0x100; n++)
		if (n != 0x33 && !(n >= 0x70 && n <= 0x77))
			set_vec(b, n, BIOS_SEG, ROM_DUMMY_IRET);

	/* the video BIOS: signature, then fonts for INT 10h 1130h and the 1Fh/43h vectors */
	mbw(b, sl(VBIOS_SEG, 0), 0x55);
	mbw(b, sl(VBIOS_SEG, 1), 0xAA);
	mbw(b, sl(VBIOS_SEG, 2), 0x40);
	const uint8_t *f16 = vga_font16(), *f8 = vga_font8(), *f14 = vga_font14();
	for (int i = 0; i < 256 * 16; i++)
		mbw(b, sl(VBIOS_SEG, (uint16_t)(0x1000 + i)), f16[i]);
	for (int i = 0; i < 256 * 8; i++)
		mbw(b, sl(VBIOS_SEG, (uint16_t)(0x2000 + i)), f8[i]);
	for (int i = 0; i < 256 * 14; i++)
		mbw(b, sl(VBIOS_SEG, (uint16_t)(0x2800 + i)), f14[i]);
	set_vec(b, 0x43, VBIOS_SEG, 0x2000);
	set_vec(b, 0x1F, VBIOS_SEG, 0x2000 + 128 * 8);
	vga_vbe_rom(b);

	/* the BIOS data area */
#ifdef __x86_64__
	mww(b, BDA + 0x10, 0x0027);		/* equipment: floppy, FPU, mouse, 80x25 colour */
#else
	mww(b, BDA + 0x10, 0x0025);		/* no FPU here (see x87.c) */
#endif
	mww(b, BDA + 0x13, CONV_TOP_SEG / 64);	/* conventional memory, KB */
	mww(b, BDA + 0x0E, CONV_TOP_SEG);	/* extended BIOS data area */
	mww(b, BDA + 0x1A, 0x1E);
	mww(b, BDA + 0x1C, 0x1E);
	mww(b, BDA + 0x80, 0x1E);
	mww(b, BDA + 0x82, 0x3E);
	mbw(b, BDA + 0x49, 3);
	mww(b, BDA + 0x4A, 80);
	mww(b, BDA + 0x4C, 0x1000);
	mww(b, BDA + 0x60, 0x0607);
	mww(b, BDA + 0x63, 0x3D4);
	mbw(b, BDA + 0x65, 0x29);
	mbw(b, BDA + 0x66, 0x30);
	mbw(b, BDA + 0x84, 24);
	mww(b, BDA + 0x85, 16);
	mbw(b, BDA + 0x87, 0x60);
	mbw(b, BDA + 0x88, 0x09);
	mbw(b, BDA + 0x89, 0x11);
	mbw(b, BDA + 0x96, 0x10);		/* a 101-key keyboard */
	uint32_t ticks = (uint32_t)((rtc_epoch() % 86400) * 182 / 10);	/* 18.2 a second since midnight */
	mdw(b, BDA + 0x6C, ticks);
	mww(b, sl(CONV_TOP_SEG, 0), 1);		/* EBDA: 1 KB */
}

/* ---- text output ---------------------------------------------------------------- */

static int cur_page(struct dosbox *b) { return mb(b, BDA + 0x62) & 7; }

static void get_cursor(struct dosbox *b, int page, int *x, int *y)
{
	uint16_t v = mw(b, BDA + 0x50 + (uint32_t)page * 2);
	*x = v & 0xFF;
	*y = v >> 8;
}

void bios_set_cursor(struct dosbox *b, int x, int y)
{
	int page = cur_page(b);
	mww(b, BDA + 0x50 + (uint32_t)page * 2, (uint16_t)(y << 8 | x));
	int cols = mw(b, BDA + 0x4A);
	uint16_t pos = (uint16_t)(y * cols + x + mw(b, BDA + 0x4E) / 2);
	b->vga.crtc[0x0E] = (uint8_t)(pos >> 8);
	b->vga.crtc[0x0F] = (uint8_t)pos;
	b->cursor_x = x;
	b->cursor_y = y;
	dosbox_text_dirty(b);
}

static uint32_t text_base(struct dosbox *b, int page)
{
	return (b->vga.mode == 7 ? 0xB0000 : 0xB8000) + (uint32_t)page * mw(b, BDA + 0x4C);
}

static int char_height(struct dosbox *b)
{
	int w, h;
	vga_screen_size(b, &w, &h);
	return h >= 480 ? 16 : h >= 350 ? 14 : 8;
}

/* pixels, in whatever mode is on */
static void put_pixel(struct dosbox *b, int x, int y, uint8_t colr)
{
	struct vga *v = &b->vga;
	int w, h;
	vga_screen_size(b, &w, &h);
	if (x < 0 || y < 0 || x >= w || y >= h)
		return;
	v->dirty = 1;
	if (v->mode == 4 || v->mode == 5) {
		uint32_t a = 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 4);
		int sh = 6 - (x & 3) * 2;
		uint8_t by = mb(b, a);
		if (colr & 0x80)
			by ^= (uint8_t)((colr & 3) << sh);
		else
			by = (uint8_t)((by & ~(3 << sh)) | (colr & 3) << sh);
		mbw(b, a, by);
		return;
	}
	if (v->mode == 6) {
		uint32_t a = 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 8);
		uint8_t m = (uint8_t)(0x80 >> (x & 7)), by = mb(b, a);
		if (colr & 0x80)
			by ^= (colr & 1) ? m : 0;
		else
			by = (uint8_t)((by & ~m) | ((colr & 1) ? m : 0));
		mbw(b, a, by);
		return;
	}
	if (v->vbe_mode || (v->gc[5] & 0x40)) {
		uint32_t l = v->vbe_mode ? (uint32_t)(y * w + x) : (uint32_t)(y * v->crtc[0x13] * 8 + x);
		if (!v->vbe_mode && !(v->seq[4] & 8)) {
			uint32_t idx = (uint32_t)(y * v->crtc[0x13] * 2 + x / 4);
			v->vram[(size_t)(x & 3) * VGA_PLANE + idx] = colr;
			return;
		}
		if (l < 4 * VGA_PLANE)
			v->vram[(size_t)(l & 3) * VGA_PLANE + (l >> 2)] = colr;
		return;
	}
	/* 16-colour planar */
	uint32_t idx = (uint32_t)(y * v->crtc[0x13] * 2 + x / 8) & 0xFFFF;
	uint8_t m = (uint8_t)(0x80 >> (x & 7));
	for (int p = 0; p < 4; p++) {
		uint8_t *pl = v->vram + (size_t)p * VGA_PLANE;
		int bit = (colr >> p) & 1;
		if (colr & 0x80)
			pl[idx] ^= bit ? m : 0;
		else
			pl[idx] = (uint8_t)((pl[idx] & ~m) | (bit ? m : 0));
	}
}

static uint8_t get_pixel(struct dosbox *b, int x, int y)
{
	struct vga *v = &b->vga;
	if (v->mode == 4 || v->mode == 5) {
		uint8_t by = mb(b, 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 4));
		return (by >> (6 - (x & 3) * 2)) & 3;
	}
	if (v->mode == 6) {
		uint8_t by = mb(b, 0xB8000 + (uint32_t)((y & 1) * 0x2000 + (y >> 1) * 80 + x / 8));
		return (by >> (7 - (x & 7))) & 1;
	}
	int w, h;
	vga_screen_size(b, &w, &h);
	if (v->vbe_mode || (v->gc[5] & 0x40)) {
		if (!v->vbe_mode && !(v->seq[4] & 8))
			return v->vram[(size_t)(x & 3) * VGA_PLANE + (uint32_t)(y * v->crtc[0x13] * 2 + x / 4)];
		uint32_t l = v->vbe_mode ? (uint32_t)(y * w + x) : (uint32_t)(y * v->crtc[0x13] * 8 + x);
		return l < 4 * VGA_PLANE ? v->vram[(size_t)(l & 3) * VGA_PLANE + (l >> 2)] : 0;
	}
	uint32_t idx = (uint32_t)(y * v->crtc[0x13] * 2 + x / 8) & 0xFFFF;
	uint8_t r = 0;
	for (int p = 0; p < 4; p++)
		if (v->vram[(size_t)p * VGA_PLANE + idx] & (0x80 >> (x & 7)))
			r |= (uint8_t)(1 << p);
	return r;
}

static void gfx_char(struct dosbox *b, int cx, int cy, uint8_t ch, uint8_t fg, uint8_t bg, int opaque)
{
	int chh = char_height(b);
	const uint8_t *font = chh == 16 ? vga_font16() : chh == 14 ? vga_font14() : vga_font8();
	/* programs can point INT 43h at a font of their own */
	uint32_t user = sl(mw(b, 0x43 * 4 + 2), mw(b, 0x43 * 4));
	int own = mw(b, 0x43 * 4 + 2) != VBIOS_SEG;
	for (int y = 0; y < chh; y++) {
		uint8_t bits = own ? mb(b, user + (uint32_t)(ch * chh + y)) : font[ch * chh + y];
		for (int x = 0; x < 8; x++) {
			if (bits & (0x80 >> x))
				put_pixel(b, cx * 8 + x, cy * chh + y, fg);
			else if (opaque)
				put_pixel(b, cx * 8 + x, cy * chh + y, bg);
		}
	}
}

static void scroll(struct dosbox *b, int up, int top, int left, int bottom, int right, int n, uint8_t attr)
{
	int cols, rows;
	vga_text_size(b, &cols, &rows);
	if (bottom >= rows) bottom = rows - 1;
	if (right >= cols) right = cols - 1;
	if (top > bottom || left > right)
		return;
	int h = bottom - top + 1;
	if (n == 0 || n > h)
		n = h;
	if (vga_is_text(b)) {
		uint32_t base = text_base(b, cur_page(b));
		for (int i = 0; i < h; i++) {
			int y = up ? top + i : bottom - i;
			int from = up ? y + n : y - n;
			for (int x = left; x <= right; x++) {
				uint32_t d = base + (uint32_t)(y * cols + x) * 2;
				if ((up && from <= bottom) || (!up && from >= top)) {
					uint32_t s = base + (uint32_t)(from * cols + x) * 2;
					mbw(b, d, mb(b, s));
					mbw(b, d + 1, mb(b, s + 1));
				} else {
					mbw(b, d, ' ');
					mbw(b, d + 1, attr);
				}
			}
		}
		dosbox_text_dirty(b);
		return;
	}
	/* graphics: move whole character rows of pixels */
	int chh = char_height(b);
	for (int i = 0; i < h; i++) {
		int y = up ? top + i : bottom - i;
		int from = up ? y + n : y - n;
		for (int py = 0; py < chh; py++)
			for (int px = left * 8; px < (right + 1) * 8; px++) {
				uint8_t colr = ((up && from <= bottom) || (!up && from >= top)) ?
				    get_pixel(b, px, from * chh + py) : attr;
				put_pixel(b, px, y * chh + py, colr);
			}
	}
}

/* teletype output, as INT 10h AH=0Eh does it */
void bios_tty(struct dosbox *b, uint8_t ch)
{
	dosbox_stream_char(b, ch);
	int cols, rows;
	vga_text_size(b, &cols, &rows);
	int page = cur_page(b), x, y;
	get_cursor(b, page, &x, &y);
	switch (ch) {
	case 7:
		return;
	case 8:
		if (x)
			x--;
		break;
	case 10:
		y++;
		break;
	case 13:
		x = 0;
		break;
	default:
		if (vga_is_text(b)) {
			uint32_t a = text_base(b, page) + (uint32_t)(y * cols + x) * 2;
			mbw(b, a, ch);
			dosbox_text_dirty(b);
		} else {
			gfx_char(b, x, y, ch, (uint8_t)(b->cpu.r[R_EBX] & 0xFF) ? (uint8_t)(b->cpu.r[R_EBX] & 0xFF) : 15, 0, 1);
		}
		if (++x >= cols) {
			x = 0;
			y++;
		}
	}
	if (y >= rows) {
		y = rows - 1;
		uint8_t attr = vga_is_text(b) ? mb(b, text_base(b, page) + (uint32_t)(y * cols + x) * 2 + 1) : 0;
		scroll(b, 1, 0, 0, rows - 1, cols - 1, 1, attr ? attr : 7);
	}
	bios_set_cursor(b, x, y);
}

/* ---- INT 10h --------------------------------------------------------------------- */

static void int10(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	int cols, rows;
	vga_text_size(b, &cols, &rows);
	uint8_t fn = AH(c);
	if (fn == 0x00 || fn == 0x06 || fn == 0x07 || fn == 0x09 || fn == 0x0A || fn == 0x13 || fn == 0x05)
		dosbox_screen_mode(b);
	if (fn == 0x02) {
		int x, y;
		get_cursor(b, BH(c) & 7, &x, &y);
		if (x != DL(c) || y != DH(c))
			dosbox_screen_mode(b);
	}
	switch (fn) {
	case 0x00: {
		int mode = AL(c) & 0x7F;
		vga_set_mode(b, mode, !(AL(c) & 0x80));
		const int m = b->vga.mode;
		mbw(b, BDA + 0x49, (uint8_t)m);
		int w = (m == 0 || m == 1 || m == 4 || m == 5 || m == 0x0D || m == 0x13) ? 40 : 80;
		mww(b, BDA + 0x4A, (uint16_t)w);
		int r = m == 0x10 || m == 0x0F ? 25 : m == 0x11 || m == 0x12 ? 30 : 25;
		mbw(b, BDA + 0x84, (uint8_t)(r - 1));
		mww(b, BDA + 0x85, (uint16_t)(m <= 3 || m == 7 ? 16 : m == 0x10 || m == 0x0F ? 14 : m >= 0x11 ? 16 : 8));
		mww(b, BDA + 0x4C, (uint16_t)(m <= 3 ? (w == 40 ? 0x800 : 0x1000) : 0x2000));
		mww(b, BDA + 0x4E, 0);
		mbw(b, BDA + 0x62, 0);
		for (int p = 0; p < 8; p++)
			mww(b, BDA + 0x50 + (uint32_t)p * 2, 0);
		mbw(b, BDA + 0x66, m == 5 ? 0x30 : 0x30);
		bios_set_cursor(b, 0, 0);
		dosbox_set_graphics(b, !vga_is_text(b));
		if (m <= 3 || m == 7)
			setlo(c, R_EAX, m == 6 ? 0x3F : 0x30);
		else
			setlo(c, R_EAX, 0x20);
		break;
	}
	case 0x01:
		mww(b, BDA + 0x60, CX(c));
		b->vga.crtc[0x0A] = CH(c);
		b->vga.crtc[0x0B] = CL(c);
		dosbox_text_dirty(b);
		break;
	case 0x02: {
		int page = BH(c) & 7;
		mww(b, BDA + 0x50 + (uint32_t)page * 2, (uint16_t)(DH(c) << 8 | DL(c)));
		if (page == cur_page(b))
			bios_set_cursor(b, DL(c), DH(c));
		break;
	}
	case 0x03: {
		int x, y;
		get_cursor(b, BH(c) & 7, &x, &y);
		set16(c, R_EDX, (uint16_t)(y << 8 | x));
		set16(c, R_ECX, mw(b, BDA + 0x60));
		break;
	}
	case 0x04:
		setlo(c, R_EAX, 0);	/* no light pen */
		break;
	case 0x05: {
		int page = AL(c) & 7;
		mbw(b, BDA + 0x62, (uint8_t)page);
		mww(b, BDA + 0x4E, (uint16_t)(page * mw(b, BDA + 0x4C)));
		uint16_t start = (uint16_t)(page * mw(b, BDA + 0x4C) / 2);
		b->vga.crtc[0x0C] = (uint8_t)(start >> 8);
		b->vga.crtc[0x0D] = (uint8_t)start;
		int x, y;
		get_cursor(b, page, &x, &y);
		bios_set_cursor(b, x, y);
		break;
	}
	case 0x06: case 0x07:
		scroll(b, AH(c) == 6, CH(c), CL(c), DH(c), DL(c), AL(c), BH(c));
		break;
	case 0x08: {
		int x, y, page = BH(c) & 7;
		get_cursor(b, page, &x, &y);
		if (vga_is_text(b)) {
			uint32_t a = text_base(b, page) + (uint32_t)(y * cols + x) * 2;
			set16(c, R_EAX, (uint16_t)(mb(b, a) | mb(b, a + 1) << 8));
		} else {
			set16(c, R_EAX, 0);
		}
		break;
	}
	case 0x09: case 0x0A: {
		int x, y, page = BH(c) & 7;
		get_cursor(b, page, &x, &y);
		int n = CX(c);
		for (int i = 0; i < n; i++) {
			int px = x + i, py = y;
			while (px >= cols) {
				px -= cols;
				py++;
			}
			if (py >= rows)
				break;
			if (vga_is_text(b)) {
				uint32_t a = text_base(b, page) + (uint32_t)(py * cols + px) * 2;
				mbw(b, a, AL(c));
				if (AH(c) == 9)
					mbw(b, a + 1, BL(c));
			} else {
				gfx_char(b, px, py, AL(c), BL(c), 0, 1);
			}
		}
		dosbox_text_dirty(b);
		break;
	}
	case 0x0B:
		if (BH(c) == 0)
			mbw(b, BDA + 0x66, (uint8_t)((mb(b, BDA + 0x66) & 0xE0) | (BL(c) & 0x1F)));
		else
			mbw(b, BDA + 0x66, (uint8_t)((mb(b, BDA + 0x66) & ~0x20) | ((BL(c) & 1) << 5)));
		b->vga.dirty = 1;
		break;
	case 0x0C:
		put_pixel(b, CX(c), DX(c), AL(c));
		break;
	case 0x0D:
		setlo(c, R_EAX, get_pixel(b, CX(c), DX(c)));
		break;
	case 0x0E:
		bios_tty(b, AL(c));
		break;
	case 0x0F:
		setlo(c, R_EAX, mb(b, BDA + 0x49));
		sethi(c, R_EAX, (uint8_t)mw(b, BDA + 0x4A));
		sethi(c, R_EBX, (uint8_t)cur_page(b));
		break;
	case 0x10:
		switch (AL(c)) {
		case 0x00:
			b->vga.attr[BL(c) & 15] = BH(c);
			b->vga.dirty = 1;
			break;
		case 0x01:
			b->vga.attr[0x11] = BH(c);
			break;
		case 0x02: {
			uint32_t a = c->s[S_ES].base + DX(c);
			for (int i = 0; i < 17; i++)
				b->vga.attr[i < 16 ? i : 0x11] = x86_rd8(c, a + (uint32_t)i);
			b->vga.dirty = 1;
			break;
		}
		case 0x03:
			if (BL(c))
				b->vga.attr[0x10] |= 8;
			else
				b->vga.attr[0x10] &= (uint8_t)~8;
			break;
		case 0x07:
			sethi(c, R_EBX, b->vga.attr[BL(c) & 0x1F]);
			break;
		case 0x08:
			sethi(c, R_EBX, b->vga.attr[0x11]);
			break;
		case 0x09: {
			uint32_t a = c->s[S_ES].base + DX(c);
			for (int i = 0; i < 17; i++)
				x86_wr8(c, a + (uint32_t)i, b->vga.attr[i < 16 ? i : 0x11]);
			break;
		}
		case 0x10:
			b->vga.dac[BX(c) & 0xFF][0] = DH(c) & 0x3F;
			b->vga.dac[BX(c) & 0xFF][1] = CH(c) & 0x3F;
			b->vga.dac[BX(c) & 0xFF][2] = CL(c) & 0x3F;
			b->vga.dirty = 1;
			break;
		case 0x12: {
			uint32_t a = c->s[S_ES].base + DX(c);
			for (int i = 0; i < CX(c) && BX(c) + i < 256; i++)
				for (int k = 0; k < 3; k++)
					b->vga.dac[BX(c) + i][k] = x86_rd8(c, a + (uint32_t)(i * 3 + k)) & 0x3F;
			b->vga.dirty = 1;
			break;
		}
		case 0x13:
			if (BL(c) == 0)
				b->vga.attr[0x10] = (uint8_t)((b->vga.attr[0x10] & 0x7F) | (BH(c) & 1) << 7);
			else
				b->vga.attr[0x14] = BH(c);
			break;
		case 0x15:
			sethi(c, R_EDX, b->vga.dac[BL(c)][0]);
			sethi(c, R_ECX, b->vga.dac[BL(c)][1]);
			setlo(c, R_ECX, b->vga.dac[BL(c)][2]);
			break;
		case 0x17: {
			uint32_t a = c->s[S_ES].base + DX(c);
			for (int i = 0; i < CX(c) && BX(c) + i < 256; i++)
				for (int k = 0; k < 3; k++)
					x86_wr8(c, a + (uint32_t)(i * 3 + k), b->vga.dac[BX(c) + i][k]);
			break;
		}
		case 0x18:
			b->vga.dac_mask = BL(c);
			break;
		case 0x19:
			setlo(c, R_EBX, b->vga.dac_mask);
			break;
		case 0x1A:
			set16(c, R_EBX, (uint16_t)((b->vga.attr[0x14] & 0x0F) << 8 | ((b->vga.attr[0x10] >> 7) & 1)));
			break;
		case 0x1B: {	/* to grey */
			for (int i = 0; i < CX(c) && BX(c) + i < 256; i++) {
				uint8_t *d = b->vga.dac[BX(c) + i];
				uint8_t g = (uint8_t)((d[0] * 30 + d[1] * 59 + d[2] * 11) / 100);
				d[0] = d[1] = d[2] = g;
			}
			b->vga.dirty = 1;
			break;
		}
		}
		break;
	case 0x11:
		if (AL(c) == 0x30) {	/* font information */
			int which = BH(c);
			uint16_t off = which == 2 ? 0x2800 : which == 3 || which == 4 ? 0x2000 :
			    which == 6 ? 0x1000 : which == 1 ? 0x2000 + 128 * 8 : 0x1000;
			if (which == 0) {
				x86_load_seg_real(c, S_ES, mw(b, 0x1F * 4 + 2));
				set16(c, R_EBP, mw(b, 0x1F * 4));
			} else if (which == 1) {
				x86_load_seg_real(c, S_ES, mw(b, 0x43 * 4 + 2));
				set16(c, R_EBP, mw(b, 0x43 * 4));
			} else {
				x86_load_seg_real(c, S_ES, VBIOS_SEG);
				set16(c, R_EBP, off);
			}
			set16(c, R_ECX, (uint16_t)mw(b, BDA + 0x85));
			setlo(c, R_EDX, mb(b, BDA + 0x84));
		} else if (AL(c) == 0x12 || AL(c) == 0x02) {	/* 8x8 font: 43 or 50 lines */
			int lines = b->vga.mode == 3 ? 50 : 43;
			mbw(b, BDA + 0x84, (uint8_t)(lines - 1));
			mww(b, BDA + 0x85, 8);
			b->vga.crtc[9] = (uint8_t)((b->vga.crtc[9] & 0xE0) | 7);
			dosbox_text_dirty(b);
		} else if (AL(c) == 0x14 || AL(c) == 0x04 || AL(c) == 0x11 || AL(c) == 0x01) {
			mbw(b, BDA + 0x84, 24);
			mww(b, BDA + 0x85, 16);
			b->vga.crtc[9] = (uint8_t)((b->vga.crtc[9] & 0xE0) | 15);
			dosbox_text_dirty(b);
		}
		break;
	case 0x12:
		if (BL(c) == 0x10) {		/* EGA information */
			set16(c, R_EBX, 0x0003);	/* colour, 256 KB */
			set16(c, R_ECX, 0x0009);
		} else if (BL(c) == 0x34 || BL(c) == 0x30 || BL(c) == 0x31 || BL(c) == 0x32 || BL(c) == 0x33 ||
		    BL(c) == 0x35 || BL(c) == 0x36) {
			setlo(c, R_EAX, 0x12);
		}
		break;
	case 0x13: {
		int x = DL(c), y = DH(c), page = BH(c) & 7, ox, oy;
		get_cursor(b, page, &ox, &oy);
		bios_set_cursor(b, x, y);
		uint32_t s = c->s[S_ES].base + BP(c);
		for (int i = 0; i < CX(c); i++) {
			uint8_t ch = x86_rd8(c, s + (uint32_t)(AL(c) & 2 ? i * 2 : i));
			uint8_t at = AL(c) & 2 ? x86_rd8(c, s + (uint32_t)i * 2 + 1) : BL(c);
			int cx, cy;
			get_cursor(b, page, &cx, &cy);
			if (ch >= 0x20 && vga_is_text(b)) {
				uint32_t a = text_base(b, page) + (uint32_t)(cy * cols + cx) * 2;
				mbw(b, a, ch);
				mbw(b, a + 1, at);
				if (++cx >= cols) {
					cx = 0;
					cy = MIN(cy + 1, rows - 1);
				}
				bios_set_cursor(b, cx, cy);
			} else if (ch >= 0x20) {
				gfx_char(b, cx, cy, ch, at, 0, 1);
				if (++cx >= cols) {
					cx = 0;
					cy = MIN(cy + 1, rows - 1);
				}
				bios_set_cursor(b, cx, cy);
			} else {
				bios_tty(b, ch);
			}
		}
		if (!(AL(c) & 1))
			bios_set_cursor(b, ox, oy);
		dosbox_text_dirty(b);
		break;
	}
	case 0x1A:
		if (AL(c) == 0) {
			setlo(c, R_EAX, 0x1A);
			set16(c, R_EBX, 0x0008);	/* VGA, colour */
		}
		break;
	case 0x1B:
		setlo(c, R_EAX, 0);
		break;
	case 0x4F:
		vga_vbe(b);
		break;
	case 0xFE:	/* TopView/DESQview: the video buffer is where it always was */
		break;
	}
}

/* ---- the keyboard ------------------------------------------------------------------------ */

/* scancode -> ASCII, unshifted and shifted, US layout */
static const char kb_lo[0x3A] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8,
	9, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 13,
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
	'*', 0, ' ',
};
static const char kb_hi[0x3A] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 8,
	0, 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 13,
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
	'*', 0, ' ',
};

static void kbuf_put(struct dosbox *b, uint16_t key)
{
	uint16_t head = mw(b, BDA + 0x1A), tail = mw(b, BDA + 0x1C);
	uint16_t start = mw(b, BDA + 0x80), end = mw(b, BDA + 0x82);
	uint16_t next = (uint16_t)(tail + 2);
	if (next >= end)
		next = start;
	if (next == head)
		return;		/* full: a real BIOS beeps */
	mww(b, BDA + tail, key);
	mww(b, BDA + 0x1C, next);
}

int bios_key_avail(struct dosbox *b)
{
	return mw(b, BDA + 0x1A) != mw(b, BDA + 0x1C);
}

static uint16_t kbuf_peek(struct dosbox *b)
{
	return mw(b, BDA + mw(b, BDA + 0x1A));
}

uint16_t bios_key_get(struct dosbox *b)
{
	uint16_t head = mw(b, BDA + 0x1A);
	uint16_t k = mw(b, BDA + head);
	head = (uint16_t)(head + 2);
	if (head >= mw(b, BDA + 0x82))
		head = mw(b, BDA + 0x80);
	mww(b, BDA + 0x1A, head);
	return k;
}

/* INT 9's C half: one byte from port 60h, in AL */
static void int9_hle(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	uint8_t sc = AL(c);
	uint8_t f = mb(b, BDA + 0x17), f2 = mb(b, BDA + 0x18);
	int e0 = b->kb_e0;
	b->kb_e0 = 0;
	if (sc == 0xE0 || sc == 0xE1) {
		b->kb_e0 = sc == 0xE0;
		return;
	}
	int up = sc & 0x80;
	sc &= 0x7F;
	/* shift keys */
	switch (sc) {
	case 0x2A: if (!e0) f = up ? f & ~2 : f | 2; goto done;
	case 0x36: f = up ? f & ~1 : f | 1; goto done;
	case 0x1D: f = up ? f & ~4 : f | 4; if (!e0) f2 = up ? f2 & ~1 : f2 | 1; goto done;
	case 0x38: f = up ? f & ~8 : f | 8; if (!e0) f2 = up ? f2 & ~2 : f2 | 2; goto done;
	case 0x3A: if (!up && !(f2 & 0x40)) f ^= 0x40; f2 = up ? f2 & ~0x40 : f2 | 0x40; goto done;
	case 0x45: if (!up && !(f2 & 0x20)) f ^= 0x20; f2 = up ? f2 & ~0x20 : f2 | 0x20; goto done;
	case 0x46:
		if (e0 || (f & 4)) {		/* Ctrl+Break */
			if (!up) {
				mww(b, BDA + 0x1A, mw(b, BDA + 0x80));
				mww(b, BDA + 0x1C, mw(b, BDA + 0x80));
				kbuf_put(b, 0);
				b->ctrl_break = 1;
			}
			goto done;
		}
		if (!up && !(f2 & 0x10)) f ^= 0x10;
		f2 = up ? f2 & ~0x10 : f2 | 0x10;
		goto done;
	case 0x52:
		if (!up && !e0 && !(f & 0x20)) f ^= 0x80;
		break;
	}
	if (up)
		goto done;
	int shift = (f & 3) != 0, ctrl = (f & 4) != 0, alt = (f & 8) != 0;
	if (ctrl && alt && sc == 0x53) {	/* Ctrl+Alt+Del: end the program */
		b->want_quit = 1;
		goto done;
	}
	uint16_t key = 0;
	if (sc >= 0x3B && sc <= 0x44) {		/* F1-F10 */
		int base = alt ? 0x68 : ctrl ? 0x5E : shift ? 0x54 : 0x3B;
		key = (uint16_t)((base + sc - 0x3B) << 8);
	} else if (sc == 0x57 || sc == 0x58) {	/* F11 F12 */
		int base = alt ? 0x8B : ctrl ? 0x89 : shift ? 0x87 : 0x85;
		key = (uint16_t)((base + sc - 0x57) << 8);
	} else if (sc >= 0x47 && sc <= 0x53) {	/* the keypad and the grey keys */
		static const char pad[] = "789-456+1230.";
		int numlock = (f & 0x20) != 0;
		char ch = pad[sc - 0x47];
		if (!e0 && (numlock != shift) && ch != '-' && ch != '+') {
			key = (uint16_t)(sc << 8 | (uint8_t)ch);
		} else if (ch == '-' || ch == '+') {
			key = (uint16_t)(sc << 8 | (uint8_t)ch);
		} else {
			static const uint8_t ctl[13] = { 0x77, 0x8D, 0x84, 0, 0x73, 0x8F, 0x74, 0, 0x75, 0x91, 0x76, 0x92, 0x93 };
			static const uint8_t alts[13] = { 0x97, 0x98, 0x99, 0, 0x9B, 0, 0x9D, 0, 0x9F, 0xA0, 0xA1, 0xA2, 0xA3 };
			uint8_t s2 = ctrl ? ctl[sc - 0x47] : alt && e0 ? alts[sc - 0x47] : sc;
			key = (uint16_t)(s2 << 8 | (e0 ? 0xE0 : 0));
		}
	} else if (e0 && sc == 0x1C) {
		key = ctrl ? 0xE00A : 0xE00D;
	} else if (e0 && sc == 0x35) {
		key = 0xE02F;
	} else if (sc < 0x3A) {
		char lo = kb_lo[sc], hi = kb_hi[sc];
		int letter = lo >= 'a' && lo <= 'z';
		int caps = (f & 0x40) && letter;
		if (alt) {
			if (sc >= 2 && sc <= 13)
				key = (uint16_t)((sc + 0x76) << 8);
			else if (letter || sc == 0x1A || sc == 0x1B || sc == 0x27 || sc == 0x28 || sc == 0x29 ||
			    sc == 0x2B || sc == 0x33 || sc == 0x34 || sc == 0x35)
				key = (uint16_t)(sc << 8);
			else if (sc == 0x01)
				key = 0x0100;
			else if (sc == 0x0E)
				key = 0x0E00;
			else if (sc == 0x1C)
				key = 0x1C00;
			else if (sc == 0x0F)
				key = 0xA500;
		} else if (ctrl) {
			if (letter)
				key = (uint16_t)(sc << 8 | (uint8_t)(lo - 'a' + 1));
			else if (sc == 0x03)
				key = 0x0300;		/* Ctrl-2: NUL */
			else if (sc == 0x07)
				key = 0x071E;
			else if (sc == 0x0C)
				key = 0x0C1F;
			else if (sc == 0x1A)
				key = 0x1A1B;
			else if (sc == 0x1B)
				key = 0x1B1D;
			else if (sc == 0x2B)
				key = 0x2B1C;
			else if (sc == 0x1C)
				key = 0x1C0A;
			else if (sc == 0x0E)
				key = 0x0E7F;
			else if (sc == 0x0F)
				key = 0x9400;
			else if (sc == 0x39)
				key = 0x3920;
			else if (sc == 0x01)
				key = 0x011B;
		} else {
			char ch = (shift != caps) ? hi : lo;
			if (sc == 0x0F && shift)
				key = 0x0F00;
			else if (ch)
				key = (uint16_t)(sc << 8 | (uint8_t)ch);
		}
	}
	if (key)
		kbuf_put(b, key);
done:
	mbw(b, BDA + 0x17, f);
	mbw(b, BDA + 0x18, f2);
}

/* ---- INT 16h --------------------------------------------------------------------- */

static int int16(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	switch (AH(c)) {
	case 0x00: case 0x10:
		for (;;) {
			if (!bios_key_avail(b)) {
				if (b->want_quit)
					return 0;
				c->flags |= FL_IF;	/* let the keyboard interrupt in while we wait */
				dosbox_input_wait(b);
				return 1;
			}
			uint16_t k = bios_key_get(b);
			if (AH(c) == 0x00) {
				if ((k & 0xFF) == 0xE0 && k >> 8)
					k &= 0xFF00;
				if ((k >> 8) > 0x84 && (k & 0xFF) == 0)
					continue;	/* F11/F12 and friends: enhanced only */
			}
			set16(c, R_EAX, k);
			b->idle_polls = 0;
			return 0;
		}
	case 0x01: case 0x11:
		while (bios_key_avail(b) && AH(c) == 0x01 && (kbuf_peek(b) >> 8) > 0x84 && (kbuf_peek(b) & 0xFF) == 0)
			bios_key_get(b);
		if (bios_key_avail(b)) {
			uint16_t k = kbuf_peek(b);
			if (AH(c) == 0x01 && (k & 0xFF) == 0xE0 && k >> 8)
				k &= 0xFF00;
			set16(c, R_EAX, k);
			bios_set_flag(c, FL_ZF, 0);
			b->idle_polls = 0;
		} else {
			bios_set_flag(c, FL_ZF, 1);
			b->idle_polls++;	/* a program spinning on this is waiting for a key */
		}
		return 0;
	case 0x02:
		setlo(c, R_EAX, mb(b, BDA + 0x17));
		return 0;
	case 0x12:
		set16(c, R_EAX, (uint16_t)(mb(b, BDA + 0x17) | mb(b, BDA + 0x18) << 8));
		return 0;
	case 0x03:
		return 0;
	case 0x05:
		kbuf_put(b, CX(c));
		setlo(c, R_EAX, 0);
		return 0;
	case 0x0A:
		set16(c, R_EBX, 0x41AB);
		return 0;
	}
	return 0;
}

/* ---- INT 1Ah, 15h ------------------------------------------------------------------ */

static uint8_t to_bcd(int v)
{
	return (uint8_t)((v / 10) << 4 | (v % 10));
}

static void int1a(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	int y, mo, d, h, mi, s;
	switch (AH(c)) {
	case 0x00:
		set16(c, R_ECX, mw(b, BDA + 0x6E));
		set16(c, R_EDX, mw(b, BDA + 0x6C));
		setlo(c, R_EAX, mb(b, BDA + 0x70));
		mbw(b, BDA + 0x70, 0);
		break;
	case 0x01:
		mww(b, BDA + 0x6E, CX(c));
		mww(b, BDA + 0x6C, DX(c));
		break;
	case 0x02:
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		sethi(c, R_ECX, to_bcd(h));
		setlo(c, R_ECX, to_bcd(mi));
		sethi(c, R_EDX, to_bcd(s));
		setlo(c, R_EDX, 0);
		cf(c, 0);
		break;
	case 0x04:
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		sethi(c, R_ECX, to_bcd(y / 100));
		setlo(c, R_ECX, to_bcd(y % 100));
		sethi(c, R_EDX, to_bcd(mo));
		setlo(c, R_EDX, to_bcd(d));
		cf(c, 0);
		break;
	case 0x03: case 0x05:
		cf(c, 0);
		break;
	default:
		cf(c, 1);
	}
}

static int int15(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	switch (AH(c)) {
	case 0x24:
		if (AL(c) == 0 || AL(c) == 1) {
			c->a20 = AL(c) ? 0xFFFFFFFF : 0xFFEFFFFF;
			sethi(c, R_EAX, 0);
		} else if (AL(c) == 2) {
			setlo(c, R_EAX, (c->a20 & 0x100000) ? 1 : 0);
			sethi(c, R_EAX, 0);
		} else {
			set16(c, R_EBX, 3);
			sethi(c, R_EAX, 0);
		}
		cf(c, 0);
		return 0;
	case 0x4F:
		cf(c, 1);		/* nobody intercepts the keyboard */
		return 0;
	case 0x86: {		/* wait CX:DX microseconds */
		uint64_t now = dos_now(b);
		if (!b->wait_until) {
			b->wait_until = now + ((uint32_t)CX(c) << 16 | DX(c));
		}
		if (now < b->wait_until) {
			c->flags |= FL_IF;
			return 1;
		}
		b->wait_until = 0;
		cf(c, 0);
		return 0;
	}
	case 0x87: {		/* copy to or from extended memory */
		uint32_t gdt = c->s[S_ES].base + SI(c);
		uint32_t src = x86_rd16(c, gdt + 0x12) | (uint32_t)x86_rd8(c, gdt + 0x14) << 16 |
		    (uint32_t)x86_rd8(c, gdt + 0x17) << 24;
		uint32_t dst = x86_rd16(c, gdt + 0x1A) | (uint32_t)x86_rd8(c, gdt + 0x1C) << 16 |
		    (uint32_t)x86_rd8(c, gdt + 0x1F) << 24;
		uint32_t n = (uint32_t)CX(c) * 2;
		for (uint32_t i = 0; i < n; i++) {
			if (src + i >= c->mem_size || dst + i >= c->mem_size)
				break;
			c->mem[dst + i] = c->mem[src + i];
		}
		sethi(c, R_EAX, 0);
		cf(c, 0);
		return 0;
	}
	case 0x88:		/* extended memory: XMS has it */
		set16(c, R_EAX, 0);
		cf(c, 0);
		return 0;
	case 0xC0:
		x86_load_seg_real(c, S_ES, BIOS_SEG);
		set16(c, R_EBX, ROM_SYSCONF);
		sethi(c, R_EAX, 0);
		cf(c, 0);
		return 0;
	case 0xC1:
		x86_load_seg_real(c, S_ES, CONV_TOP_SEG);
		cf(c, 0);
		return 0;
	case 0xE8:
		if (AL(c) == 0x01) {
			uint32_t ext = c->mem_size > 0x100000 ? (c->mem_size - 0x100000) / 1024 : 0;
			uint32_t lo = MIN(ext, 15 * 1024u), hi = ext > lo ? (ext - lo) / 64 : 0;
			set16(c, R_EAX, (uint16_t)lo);
			set16(c, R_ECX, (uint16_t)lo);
			set16(c, R_EBX, (uint16_t)hi);
			set16(c, R_EDX, (uint16_t)hi);
			cf(c, 0);
			return 0;
		}
		break;
	}
	sethi(c, R_EAX, 0x86);
	cf(c, 1);
	return 0;
}

/* ---- INT 33h: the mouse --------------------------------------------------------------- */

/* the driver's own coordinates for this mode: 640 across in 320-wide modes,
 * 8 per character cell in text */
static void mouse_virtual(struct dosbox *b, int *vw, int *vh)
{
	int w, h;
	vga_screen_size(b, &w, &h);
	if (vga_is_text(b)) {
		int cols, rows;
		vga_text_size(b, &cols, &rows);
		w = cols * (cols <= 40 ? 16 : 8);
		h = rows * 8;
	} else if (w <= 320) {
		w *= 2;
	}
	*vw = MAX(w, 1);
	*vh = MAX(h, 1);
}

static void mouse_limits(struct dosbox *b)
{
	struct mouse *m = &b->mouse;
	int w, h;
	mouse_virtual(b, &w, &h);
	m->minx = 0;
	m->maxx = w - 1;
	m->miny = 0;
	m->maxy = h - 1;
}

/*
 * How the pointer's place on the screen becomes the program's coordinates,
 * one axis at a time: coordinate = off + pixel * num / den.
 *
 * A real driver moves by mickeys and the range only clips, but here the
 * host's pointer sits on top of the program's, so the two should agree. A
 * range about the size of the screen (0..319, 0..639, or Lemmings' 8..328 for
 * a cursor drawn around its middle) is the program's idea of the whole
 * screen, and is followed exactly; a small range is a box to keep the
 * pointer in, and only clips the driver's own coordinates.
 */
struct axis { int off, num, den; };

static struct axis mouse_axis(int min, int max, int pixels, int virt)
{
	int span = max - min + 1;
	pixels = MAX(pixels, 1);
	if (span * 2 < pixels)
		return (struct axis){ 0, virt, pixels };
	int r = (span + pixels / 2) / pixels;
	int miss = span - r * pixels;
	if (r >= 1 && miss <= 16 * r && miss >= -16 * r)
		return (struct axis){ min, r, 1 };
	return (struct axis){ min, span, pixels };
}

static void mouse_axes(struct dosbox *b, struct axis *ax, struct axis *ay)
{
	struct mouse *m = &b->mouse;
	int w, h, vw, vh;
	vga_screen_size(b, &w, &h);
	mouse_virtual(b, &vw, &vh);
	if (vga_is_text(b)) {		/* text programs count in cells, 8 to a cell */
		*ax = (struct axis){ 0, vw, w };
		*ay = (struct axis){ 0, vh, h };
		return;
	}
	*ax = mouse_axis(m->minx, m->maxx, w, vw);
	*ay = mouse_axis(m->miny, m->maxy, h, vh);
}

/* from the display: where the pointer is, in pixels of the program's screen */
void mouse_event(struct dosbox *b, int sx, int sy, int buttons)
{
	struct mouse *m = &b->mouse;
	if (!m->on)
		return;
	int w, h, vw, vh;
	vga_screen_size(b, &w, &h);
	mouse_virtual(b, &vw, &vh);
	struct axis ax, ay;
	mouse_axes(b, &ax, &ay);
	int x = ax.off + (int)((int64_t)sx * ax.num / ax.den);
	int y = ay.off + (int)((int64_t)sy * ay.num / ay.den);
	if (vga_is_text(b)) {		/* the cell, not the pixel */
		int cols, rows;
		vga_text_size(b, &cols, &rows);
		x = sx * cols / MAX(w, 1) * (vw / MAX(cols, 1));
		y = sy * rows / MAX(h, 1) * 8;
	}
	int ox = m->x, oy = m->y;
	m->x = MAX(m->minx, MIN(m->maxx, x));
	m->y = MAX(m->miny, MIN(m->maxy, y));
	/* mickeys: the driver's default 8 per 8 pixels across, 16 per 8 down,
	 * counted on the screen whatever the range */
	m->mickx += (m->x - ox) * ax.den * vw / MAX(ax.num * w, 1);
	m->micky += (m->y - oy) * 2 * ay.den * vh / MAX(ay.num * h, 1);
	int ev = 0;
	if (m->x != ox || m->y != oy)
		ev |= 1;
	for (int i = 0; i < 3; i++) {
		int was = (m->buttons >> i) & 1, now = (buttons >> i) & 1;
		if (now && !was) {
			m->press_count[i]++;
			m->press_x[i] = m->x;
			m->press_y[i] = m->y;
			ev |= 2 << (i * 2);
		} else if (!now && was) {
			m->rel_count[i]++;
			m->rel_x[i] = m->x;
			m->rel_y[i] = m->y;
			ev |= 4 << (i * 2);
		}
	}
	m->latched |= buttons & ~m->buttons & 7;
	m->buttons = buttons & 7;
	if (ev & m->handler_mask) {
		/* each press and release gets a call of its own, so a quick click
		 * reaches a handler that looks at BX, even when the press and release
		 * arrive together */
		int last = (m->qh + 15) % 16;
		if (m->qh != m->qt && (ev & m->handler_mask) == 1 && m->q[last].ev == 1 &&
		    m->q[last].buttons == m->buttons) {
			m->q[last].x = (int16_t)m->x;	/* more motion: just the newest place */
			m->q[last].y = (int16_t)m->y;
		} else if ((m->qh + 1) % 16 != m->qt) {
			m->q[m->qh].ev = (uint16_t)(ev & m->handler_mask);
			m->q[m->qh].buttons = (uint16_t)m->buttons;
			m->q[m->qh].x = (int16_t)m->x;
			m->q[m->qh].y = (int16_t)m->y;
			m->qh = (m->qh + 1) % 16;
		}
		m->pending_events = 1;
	}
	if (m->shown > 0)
		b->vga.dirty = 1;
}

/* call the program's event handler: like an interrupt, from the main loop */
int mouse_callback(struct dosbox *b)
{
	struct mouse *m = &b->mouse;
	struct x86 *c = &b->cpu;
	if (m->qh == m->qt || m->in_handler || !m->handler_seg || (c->cr0 & CR0_PE))
		return 0;
	for (int i = 0; i < 8; i++)
		m->saved[i] = c->r[i];
	m->saved_ds = c->s[S_DS].sel;
	m->saved_es = c->s[S_ES].sel;
	m->in_handler = 1;
	/* push our return, the way a far call would, after the interrupted spot */
	x86_push16(c, (uint16_t)x86_flags(c));
	x86_push16(c, c->s[S_CS].sel);
	x86_push16(c, (uint16_t)c->eip);
	x86_push16(c, BIOS_SEG);
	x86_push16(c, ROM_MOUSE_RET);
	set16(c, R_EAX, m->q[m->qt].ev);
	set16(c, R_EBX, m->q[m->qt].buttons);
	set16(c, R_ECX, (uint16_t)m->q[m->qt].x);
	set16(c, R_EDX, (uint16_t)m->q[m->qt].y);
	set16(c, R_ESI, (uint16_t)m->mickx);
	set16(c, R_EDI, (uint16_t)m->micky);
	m->qt = (m->qt + 1) % 16;
	m->pending_events = m->qh != m->qt;
	c->flags &= ~FL_IF;
	x86_load_seg_real(c, S_CS, m->handler_seg);
	c->eip = m->handler_off;
	return 1;
}

static void mouse_return(struct dosbox *b)
{
	struct mouse *m = &b->mouse;
	struct x86 *c = &b->cpu;
	/* the handler's RETF brought us here: our frame first, then the registers
	 * (whose SP is where the stack was before that frame went on) */
	uint16_t ip = x86_pop16(c), cs = x86_pop16(c), fl = x86_pop16(c);
	for (int i = 0; i < 8; i++)
		c->r[i] = m->saved[i];
	x86_load_seg_real(c, S_DS, m->saved_ds);
	x86_load_seg_real(c, S_ES, m->saved_es);
	x86_load_seg_real(c, S_CS, cs);
	c->eip = ip;
	x86_set_flags(c, fl, 0x0FD5 | FL_IF);
	m->in_handler = 0;
}

/* the arrow the driver draws when the program shows the cursor */
void mouse_draw(struct dosbox *b, struct surface *s, struct rect area)
{
	struct mouse *m = &b->mouse;
	if (!m->on || m->shown <= 0)
		return;
	int w, h;
	vga_screen_size(b, &w, &h);
	struct axis ax, ay;
	mouse_axes(b, &ax, &ay);
	/* back from the program's coordinates to its screen's pixels */
	int vx = (int)((int64_t)(m->x - ax.off) * ax.den / MAX(ax.num, 1));
	int vy = (int)((int64_t)(m->y - ay.off) * ay.den / MAX(ay.num, 1));
	struct rect pic = vga_fit(area, w, h);
	int scale = MAX(1, pic.w / w);
	int cw = 8, chh = 16;
	if (vga_is_text(b)) {
		int cols, rows;
		vga_text_size(b, &cols, &rows);
		cw = w / MAX(cols, 1);
		chh = h / MAX(rows, 1);
		vx = vx / cw * cw;
		vy = vy / chh * chh;
	}
	int px = pic.x + vx * pic.w / w, py = pic.y + vy * pic.h / h;
	if (vga_is_text(b)) {
		/* text: the cell under the mouse in reverse */
		for (int y = 0; y < chh * pic.h / h; y++)
			for (int x = 0; x < cw * pic.w / w; x++) {
				int X = px + x, Y = py + y;
				if (X < 0 || Y < 0 || X >= s->w || Y >= s->h)
					continue;
				uint32_t *p = &s->px[(size_t)Y * (size_t)s->pitch + X];
				*p ^= 0x00FFFFFF;
			}
		return;
	}
	static const uint16_t arrow[16] = {
		0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
		0xFF80, 0xFC00, 0xEC00, 0xC600, 0x8600, 0x0300, 0x0300, 0x0000,
	};
	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 10; x++) {
			if (!(arrow[y] & (0x8000 >> x)))
				continue;
			int edge = x == 0 || !(arrow[y] & (0x8000 >> (x + 1))) || y == 15 ||
			    !(arrow[y + 1] & (0x8000 >> x));
			uint32_t colr = col(edge ? 0x000000 : 0xFFFFFF);
			for (int sy = 0; sy < scale; sy++)
				for (int sx = 0; sx < scale; sx++) {
					int X = px + x * scale + sx, Y = py + y * scale + sy;
					if (X >= 0 && Y >= 0 && X < s->w && Y < s->h)
						s->px[(size_t)Y * (size_t)s->pitch + X] = colr;
				}
		}
}

static void int33(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	struct mouse *m = &b->mouse;
	switch (AX(c)) {
	case 0x00: case 0x21:
		/* the mouse needs a display we draw ourselves, even in text mode */
		b->mouse_wanted = 1;
		dosbox_set_graphics(b, 1);
		if (!b->disp) {
			b->mouse_wanted = 0;
			set16(c, R_EAX, 0);	/* COM1 or a remote shell: no mouse here */
			break;
		}
		memset(m, 0, sizeof *m);
		m->on = 1;
		m->shown = 0;
		mouse_limits(b);
		m->x = (m->maxx + 1) / 2;
		m->y = (m->maxy + 1) / 2;
		set16(c, R_EAX, 0xFFFF);
		set16(c, R_EBX, 2);
		break;
	case 0x01: m->shown++; if (m->shown > 1) m->shown = 1; b->vga.dirty = 1; break;
	case 0x02: m->shown--; b->vga.dirty = 1; break;
	case 0x03:
		/* a press shows at least once, however quick the click */
		set16(c, R_EBX, (uint16_t)(m->buttons | m->latched));
		m->latched = 0;
		set16(c, R_ECX, (uint16_t)m->x);
		set16(c, R_EDX, (uint16_t)m->y);
		break;
	case 0x04:
		m->x = MAX(m->minx, MIN(m->maxx, (int16_t)CX(c)));
		m->y = MAX(m->miny, MIN(m->maxy, (int16_t)DX(c)));
		break;
	case 0x05: {
		int i = MIN(BX(c), 2);
		set16(c, R_EAX, (uint16_t)m->buttons);
		set16(c, R_EBX, (uint16_t)m->press_count[i]);
		set16(c, R_ECX, (uint16_t)m->press_x[i]);
		set16(c, R_EDX, (uint16_t)m->press_y[i]);
		m->press_count[i] = 0;
		break;
	}
	case 0x06: {
		int i = MIN(BX(c), 2);
		set16(c, R_EAX, (uint16_t)m->buttons);
		set16(c, R_EBX, (uint16_t)m->rel_count[i]);
		set16(c, R_ECX, (uint16_t)m->rel_x[i]);
		set16(c, R_EDX, (uint16_t)m->rel_y[i]);
		m->rel_count[i] = 0;
		break;
	}
	case 0x07:
		m->minx = MIN((int16_t)CX(c), (int16_t)DX(c));
		m->maxx = MAX((int16_t)CX(c), (int16_t)DX(c));
		m->x = MAX(m->minx, MIN(m->maxx, m->x));
		break;
	case 0x08:
		m->miny = MIN((int16_t)CX(c), (int16_t)DX(c));
		m->maxy = MAX((int16_t)CX(c), (int16_t)DX(c));
		m->y = MAX(m->miny, MIN(m->maxy, m->y));
		break;
	case 0x09: case 0x0A: case 0x0F: case 0x13: case 0x1A: case 0x1D:
		break;
	case 0x0B:
		set16(c, R_ECX, (uint16_t)m->mickx);
		set16(c, R_EDX, (uint16_t)m->micky);
		m->mickx = m->micky = 0;
		break;
	case 0x0C:
		m->handler_mask = CX(c);
		m->handler_seg = c->s[S_ES].sel;
		m->handler_off = DX(c);
		break;
	case 0x14: {
		uint16_t om = m->handler_mask, os = m->handler_seg, oo = m->handler_off;
		m->handler_mask = CX(c);
		m->handler_seg = c->s[S_ES].sel;
		m->handler_off = DX(c);
		set16(c, R_ECX, om);
		x86_load_seg_real(c, S_ES, os);
		set16(c, R_EDX, oo);
		break;
	}
	case 0x15:
		set16(c, R_EBX, (uint16_t)sizeof *m);
		break;
	case 0x16: case 0x17:
		break;
	case 0x1B:
		set16(c, R_EBX, 50);
		set16(c, R_ECX, 50);
		set16(c, R_EDX, 50);
		break;
	case 0x24:
		set16(c, R_EBX, 0x0626);	/* driver 6.26 */
		set16(c, R_ECX, 0x0400);	/* PS/2 */
		break;
	case 0x26:
		set16(c, R_EBX, 0);
		set16(c, R_ECX, (uint16_t)m->maxx);
		set16(c, R_EDX, (uint16_t)m->maxy);
		break;
	}
}

/* ---- dispatch ------------------------------------------------------------------------ */

static void eoi(struct dosbox *b, int irq)
{
	if (irq >= 8)
		b->pic_isr[1] &= (uint8_t)~(1 << (irq - 8));
	b->pic_isr[0] &= (uint8_t)~(1 << (irq >= 8 ? 2 : irq));
}

int bios_hle(struct x86 *c, int n)
{
	struct dosbox *b = BOX(c);
	if (b->dpmi_lin && (c->cr0 & CR0_PE))
		return dpmi_hle(c, n);
	switch (n) {
	case 0x00:
		snprintf(b->errbuf, sizeof b->errbuf, "Divide overflow");
		b->errmsg = b->errbuf;
		dos_terminate(b, 255, 0);
		return 0;
	case 0x01: case 0x03: case 0x04:
		return 0;
	case 0x05:
		return 0;
	case 0x06:
	{
		/* the faulting CS:IP is in the frame the CPU pushed (we're in the ROM stub) */
		uint32_t sp = c->s[S_SS].base + (c->r[R_ESP] & 0xFFFF);
		uint16_t ip = x86_rd16(c, sp), cs = x86_rd16(c, sp + 2);
		uint32_t at = (uint32_t)cs * 16 + ip;
		snprintf(b->errbuf, sizeof b->errbuf, "Invalid instruction at %04X:%04X (%02X %02X %02X %02X)",
		    cs, ip, x86_rd8(c, at), x86_rd8(c, at + 1), x86_rd8(c, at + 2), x86_rd8(c, at + 3));
		dosbox_crash_dump(b, cs, ip);
	}
		b->errmsg = b->errbuf;
		dos_terminate(b, 255, 0);
		return 0;
	case 0x07:
		return 0;
	case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
		eoi(b, n - 8);
		return 0;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
		eoi(b, n - 0x70 + 8);
		return 0;
	case 0x10: int10(b); return 0;
	case 0x11: set16(c, R_EAX, mw(b, BDA + 0x10)); return 0;
	case 0x12: set16(c, R_EAX, mw(b, BDA + 0x13)); return 0;
	case 0x13:
		if (AH(c) == 0x00) {
			sethi(c, R_EAX, 0);
			cf(c, 0);
		} else {
			sethi(c, R_EAX, AH(c) == 0x08 ? 0x07 : 0x01);
			cf(c, 1);
		}
		return 0;
	case 0x14: sethi(c, R_EAX, 0x80); return 0;	/* serial: timeout */
	case 0x15: return int15(b);
	case 0x16: return int16(b);
	case 0x17: sethi(c, R_EAX, 0x30); return 0;	/* printer: not there */
	case 0x18: case 0x19:
		dos_terminate(b, 0, 0);
		return 0;
	case 0x1A: int1a(b); return 0;
	case 0x1B: return 0;
	case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
	case 0x28: case 0x29: case 0x2A: case 0x2E: case 0x2F:
		return dos_hle(b, n);
	case 0x33: int33(b); return 0;
	case 0x31:
		return 0;
	case HLE_INT9: int9_hle(b); return 0;
	case HLE_MOUSE_RET: mouse_return(b); return 0;
	case HLE_XMS: xms_entry(b); return 0;
	case HLE_EXIT:
		dos_terminate(b, 0, 0);
		return 0;
	case HLE_VBE_BANK:
		set16(c, R_EAX, 0x4F05);
		vga_vbe(b);
		return 0;
	case HLE_DPMI_SW: case HLE_DPMI_RET: case HLE_RM_CB: case HLE_RM_TO_PM: case HLE_RAW_TO_PM:
		return dpmi_hle(c, n);
	}
	return 0;
}
