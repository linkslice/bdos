/*
 * console.c - the full-screen text console, the COM1 console, and the
 * per-task con_* calls that route to whichever console a task owns.
 *
 * Screen: 8x16 Unifont cells, 16 CGA colors, light grey on black by
 * default. While booting, before any task has its own console, output
 * is mirrored to COM1 as well.
 */
#include "kernel.h"
#include "font.h"

#define MAX_COLS 256
#define MAX_ROWS 128

const uint32_t cga_palette[16] = {
	0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
	0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_pitch, fb_format;
static uint32_t pal[16];
static int cols, rows, cx, cy, cur_attr = 0x07;
static int cursor_on, suspended, panic_mode;
static int mirror = 1;		/* echo screen output to COM1 */
static uint16_t grid[MAX_ROWS][MAX_COLS];	/* attr << 8 | char */

uint32_t pixel_pack(uint32_t rgb)
{
	uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
	if (fb_format == BDOS_PIXEL_BGRX)
		return (r << 16) | (g << 8) | b;
	return (b << 16) | (g << 8) | r;
}

static void draw_cell(int col, int row)
{
	if (suspended)
		return;
	uint16_t cell = grid[row][col];
	const uint8_t *glyph = font8x16[cell & 0xFF];
	uint32_t on = pal[(cell >> 8) & 0xF], off = pal[(cell >> 12) & 0xF];
	volatile uint32_t *p = fb + (uint64_t)row * 16 * fb_pitch + (uint64_t)col * 8;
	for (int y = 0; y < 16; y++, p += fb_pitch) {
		uint8_t bits = glyph[y];
		for (int x = 0; x < 8; x++)
			p[x] = (bits & (0x80 >> x)) ? on : off;
	}
}

static void draw_cursor(int show)
{
	if (!fb || suspended || cx >= cols)
		return;
	draw_cell(cx, cy);
	if (show) {
		uint32_t c = pal[(grid[cy][cx] >> 8) & 0xF];
		volatile uint32_t *p = fb + ((uint64_t)cy * 16 + 14) * fb_pitch + (uint64_t)cx * 8;
		for (int y = 0; y < 2; y++, p += fb_pitch)
			for (int x = 0; x < 8; x++)
				p[x] = c;
	}
}

static void redraw_all(void)
{
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			draw_cell(c, r);
	if (cursor_on)
		draw_cursor(1);
}

static void scr_clear(struct console *c)
{
	(void)c;
	if (mirror)
		serial_putc('\n');	/* keep the COM1 transcript line-oriented */
	uint16_t blank = (uint16_t)(cur_attr << 8 | ' ');
	for (int r = 0; r < MAX_ROWS; r++)
		for (int k = 0; k < MAX_COLS; k++)
			grid[r][k] = blank;
	cx = cy = 0;
	if (fb && !suspended) {
		uint32_t bg = pal[(cur_attr >> 4) & 0xF];
		for (uint32_t y = 0; y < fb_h; y++)
			for (uint32_t x = 0; x < fb_w; x++)
				fb[(uint64_t)y * fb_pitch + x] = bg;
		if (cursor_on)
			draw_cursor(1);
	}
}

static void scroll(void)
{
	uint16_t blank = (uint16_t)(cur_attr << 8 | ' ');
	for (int r = 1; r < rows; r++)
		for (int k = 0; k < cols; k++)
			if (grid[r - 1][k] != grid[r][k]) {
				grid[r - 1][k] = grid[r][k];
				draw_cell(k, r - 1);
			}
	for (int k = 0; k < cols; k++)
		if (grid[rows - 1][k] != blank) {
			grid[rows - 1][k] = blank;
			draw_cell(k, rows - 1);
		}
}

static void newline(void)
{
	cx = 0;
	if (++cy >= rows) {
		scroll();
		cy = rows - 1;
	}
}

static void scr_putc(char ch)
{
	if (mirror)
		serial_putc(ch);
	if (!fb)
		return;
	if (cursor_on)
		draw_cursor(0);
	switch (ch) {
	case '\n':
		newline();
		break;
	case '\r':
		cx = 0;
		break;
	case '\b':
		if (cx > 0)
			cx--;
		else if (cy > 0) {
			cy--;
			cx = cols - 1;
		}
		break;
	case '\t':
		do {
			if (cx >= cols)
				newline();
			grid[cy][cx] = (uint16_t)(cur_attr << 8 | ' ');
			draw_cell(cx, cy);
			cx++;
		} while (cx % 8);
		break;
	case 7:		/* bell */
		break;
	default:
		if (cx >= cols)
			newline();
		grid[cy][cx] = (uint16_t)(cur_attr << 8 | (uint8_t)ch);
		draw_cell(cx, cy);
		cx++;
	}
	if (cursor_on)
		draw_cursor(1);
}

static void scr_write(struct console *c, const char *s, size_t n)
{
	(void)c;
	while (n--)
		scr_putc(*s++);
}

/* Serial input with CR/LF and ANSI arrow translation. */
static int esc_state, last_cr;

static int serial_key(void)
{
	int ch = serial_getc();
	if (ch < 0)
		return -1;
	if (esc_state == 1) {
		esc_state = (ch == '[') ? 2 : 0;
		return esc_state ? -1 : 27;
	}
	if (esc_state == 2) {
		esc_state = 0;
		switch (ch) {
		case 'A': return KEY_UP;
		case 'B': return KEY_DOWN;
		case 'C': return KEY_RIGHT;
		case 'D': return KEY_LEFT;
		case 'H': return KEY_HOME;
		case 'F': return KEY_END;
		}
		return -1;
	}
	if (ch == 27) {
		esc_state = 1;
		return -1;
	}
	if (ch == '\n' && last_cr) {
		last_cr = 0;
		return -1;
	}
	last_cr = (ch == '\r');
	if (ch == '\r' || ch == '\n')
		return '\n';
	if (ch == 0x7F)
		return '\b';
	return ch;
}

static int scr_getkey(struct console *c)
{
	(void)c;
	if (suspended)
		return -1;
	int k = input_getkey();
	if (k >= 0)
		return k;
	return mirror ? serial_key() : -1;
}

static void scr_cursor(struct console *c, int on)
{
	(void)c;
	if (on != cursor_on) {
		cursor_on = on;
		draw_cursor(on);
	}
}

static void scr_size(struct console *c, int *w, int *h)
{
	(void)c;
	*w = cols;
	*h = rows;
}

static void scr_gotoxy(struct console *c, int x, int y)
{
	(void)c;
	if (cursor_on)
		draw_cursor(0);
	cx = MAX(0, MIN(x, cols - 1));
	cy = MAX(0, MIN(y, rows - 1));
	if (cursor_on)
		draw_cursor(1);
}

static void scr_attr(struct console *c, int fg, int bg)
{
	(void)c;
	cur_attr = ((bg & 0xF) << 4) | (fg & 0xF);
}

static struct console screen = {
	.name = "SCREEN",
	.write = scr_write,
	.getkey = scr_getkey,
	.clear = scr_clear,
	.cursor = scr_cursor,
	.size = scr_size,
	.gotoxy = scr_gotoxy,
	.attr = scr_attr,
};

void screen_init(const struct bdos_framebuffer *f)
{
	fb = (volatile uint32_t *)f->base;
	fb_w = f->width;
	fb_h = f->height;
	fb_pitch = f->pitch;
	fb_format = f->format;
	for (int i = 0; i < 16; i++)
		pal[i] = pixel_pack(cga_palette[i]);
	cols = MIN((int)(fb_w / 8), MAX_COLS);
	rows = MIN((int)(fb_h / 16), MAX_ROWS);
	scr_clear(&screen);
}

struct console *screen_console(void)
{
	return &screen;
}

void screen_suspend(int off)
{
	suspended = off;
	if (!off)
		redraw_all();
}

void screen_mirror(int on)
{
	mirror = on;
}

void console_panic_mode(void)
{
	panic_mode = 1;
	suspended = 0;
	mirror = 1;
	cur_attr = 0x1F;	/* white on blue: you know what this means */
}

/* ---- COM1 as a console (CTTY COM1) -------------------------------------- */

static void ser_write(struct console *c, const char *s, size_t n)
{
	(void)c;
	while (n--)
		serial_putc(*s++);
}

static int ser_getkey(struct console *c)
{
	(void)c;
	return serial_key();
}

static void ser_clear(struct console *c)
{
	ser_write(c, "\033[2J\033[H", 7);
}

static void ser_cursor(struct console *c, int on) { (void)c; (void)on; }

static void ser_size(struct console *c, int *w, int *h)
{
	(void)c;
	*w = 80;
	*h = 25;
}

static void ser_gotoxy(struct console *c, int x, int y)
{
	char buf[24];
	int n = snprintf(buf, sizeof buf, "\033[%d;%dH", y + 1, x + 1);
	ser_write(c, buf, (size_t)n);
}

static struct console serial_con = {
	.name = "COM1",
	.write = ser_write,
	.getkey = ser_getkey,
	.clear = ser_clear,
	.cursor = ser_cursor,
	.size = ser_size,
	.gotoxy = ser_gotoxy,
};

struct console *serial_console(void)
{
	return &serial_con;
}

/* ---- per-task routing ---------------------------------------------------- */

static struct console *cur(void)
{
	struct task *t = task_current();
	if (panic_mode || !t || !t->con)
		return &screen;
	return t->con;
}

void con_write(const char *s)
{
	struct console *c = cur();
	c->write(c, s, strlen(s));
}

void con_putc(char ch)
{
	struct console *c = cur();
	c->write(c, &ch, 1);
}

int con_pollkey(void)
{
	struct console *c = cur();
	return c->getkey(c);
}

int con_getkey(void)
{
	for (;;) {
		int k = con_pollkey();
		if (k >= 0)
			return k;
		task_idle();
	}
}

void con_clear(void)
{
	struct console *c = cur();
	c->clear(c);
}

void con_cursor(int on)
{
	struct console *c = cur();
	c->cursor(c, on);
}

void con_size(int *w, int *h)
{
	struct console *c = cur();
	c->size(c, w, h);
}

void con_gotoxy(int x, int y)
{
	struct console *c = cur();
	if (c->gotoxy)
		c->gotoxy(c, x, y);
}

void con_attr(int fg, int bg)
{
	struct console *c = cur();
	if (c->attr)
		c->attr(c, fg, bg);
}
