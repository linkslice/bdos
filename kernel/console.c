/*
 * console.c - text console on the UEFI framebuffer.
 *
 * Classic DOS look: light grey on black, 8x16 cells (Unifont), so
 * 1024x768 gives 128x48. A shadow character grid makes scrolling a
 * redraw of changed cells instead of a framebuffer-sized memmove.
 */
#include "kernel.h"
#include "font.h"

#define FG_RGB	0xAAAAAA	/* DOS light grey */
#define BG_RGB	0x000000
#define MAX_COLS 256
#define MAX_ROWS 128

static volatile uint32_t *fb;
static uint32_t fb_w, fb_h, fb_pitch;
static uint32_t fg, bg;
static int cols, rows, cx, cy;
static int cursor_on;
static char grid[MAX_ROWS][MAX_COLS];

static uint32_t pack(uint32_t rgb, uint32_t format)
{
	uint32_t r = (rgb >> 16) & 0xFF, g = (rgb >> 8) & 0xFF, b = rgb & 0xFF;
	if (format == BDOS_PIXEL_BGRX)	/* bytes B,G,R,x = 0xxxRRGGBB little endian */
		return (r << 16) | (g << 8) | b;
	return (b << 16) | (g << 8) | r;
}

static void draw_cell(int col, int row, unsigned char ch, int invert)
{
	const uint8_t *glyph = font8x16[ch];
	volatile uint32_t *p = fb + (uint64_t)row * 16 * fb_pitch + (uint64_t)col * 8;
	uint32_t on = invert ? bg : fg, off = invert ? fg : bg;
	for (int y = 0; y < 16; y++, p += fb_pitch) {
		uint8_t bits = glyph[y];
		for (int x = 0; x < 8; x++)
			p[x] = (bits & (0x80 >> x)) ? on : off;
	}
}

static void draw_cursor(int show)
{
	if (!fb || cx >= cols)
		return;
	unsigned char ch = (unsigned char)grid[cy][cx];
	if (show) {
		/* underline cursor, like DOS: last two scanlines lit */
		draw_cell(cx, cy, ch, 0);
		volatile uint32_t *p = fb + ((uint64_t)cy * 16 + 14) * fb_pitch + (uint64_t)cx * 8;
		for (int y = 0; y < 2; y++, p += fb_pitch)
			for (int x = 0; x < 8; x++)
				p[x] = fg;
	} else {
		draw_cell(cx, cy, ch, 0);
	}
}

void con_init(const struct bdos_framebuffer *f)
{
	fb = (volatile uint32_t *)f->base;
	fb_w = f->width;
	fb_h = f->height;
	fb_pitch = f->pitch;
	fg = pack(FG_RGB, f->format);
	bg = pack(BG_RGB, f->format);
	cols = (int)(fb_w / 8);
	rows = (int)(fb_h / 16);
	if (cols > MAX_COLS)
		cols = MAX_COLS;
	if (rows > MAX_ROWS)
		rows = MAX_ROWS;
	con_clear();
}

int con_cols(void) { return cols; }
int con_rows(void) { return rows; }

void con_clear(void)
{
	if (fb) {
		for (uint32_t y = 0; y < fb_h; y++)
			for (uint32_t x = 0; x < fb_w; x++)
				fb[(uint64_t)y * fb_pitch + x] = bg;
	}
	memset(grid, ' ', sizeof grid);
	cx = cy = 0;
	if (cursor_on)
		draw_cursor(1);
}

static void scroll(void)
{
	for (int r = 1; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			if (grid[r - 1][c] != grid[r][c]) {
				grid[r - 1][c] = grid[r][c];
				draw_cell(c, r - 1, (unsigned char)grid[r][c], 0);
			}
		}
	}
	for (int c = 0; c < cols; c++) {
		if (grid[rows - 1][c] != ' ') {
			grid[rows - 1][c] = ' ';
			draw_cell(c, rows - 1, ' ', 0);
		}
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

void con_putc(char c)
{
	serial_putc(c);
	if (!fb)
		return;
	if (cursor_on)
		draw_cursor(0);

	switch (c) {
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
		do
			con_putc(' ');
		while (cx % 8);
		break;
	default:
		if (cx >= cols)
			newline();
		grid[cy][cx] = c;
		draw_cell(cx, cy, (unsigned char)c, 0);
		cx++;
	}

	if (cursor_on)
		draw_cursor(1);
}

void con_write(const char *s)
{
	while (*s)
		con_putc(*s++);
}

void con_cursor(int on)
{
	if (on != cursor_on && fb) {
		cursor_on = on;
		draw_cursor(on);
	}
}
