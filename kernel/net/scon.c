/*
 * scon.c - a text console drawn on a display: the screen of a bterm -t
 * session, which is meant to be just like sitting at the machine.
 *
 * Text goes into a grid of 8x16 cells, drawn with the same glyphs as the
 * real screen. On a display that records what's drawn (bterm), that's a
 * few bytes a character and one copy to scroll. Programs that want the
 * whole display -- a DOS game, WIN -- get it from display_open(), and the
 * text comes back when they're done, as the real console does.
 */
#include "../kernel.h"
#include "../gui/wm.h"
#include "../gui/gfx.h"
#include "../vfs.h"

#define MAX_COLS 256
#define MAX_ROWS 128
#define BLINK_MS 530

struct scon {
	struct console c;
	struct display *d;
	int cols, rows, cx, cy, attr;
	int cursor_on, shown, suspended;
	uint64_t blink_at;
	struct rect dirty;
	int keys[64], kh, kt;
	int eof;
	uint16_t grid[MAX_ROWS][MAX_COLS];	/* attr << 8 | char */
};

static uint32_t pal(int i)
{
	return col(cga_palette[i & 15]);
}

static void touch(struct scon *s, struct rect r)
{
	s->dirty = rect_empty(s->dirty) ? r : rect_union(s->dirty, r);
}

/* what's been drawn goes out (to bterm: as drawing operations) */
static void sflush(struct scon *s)
{
	if (rect_empty(s->dirty) || s->suspended)
		return;
	s->d->flush(s->d, s->dirty);
	if (s->d->commit)
		s->d->commit(s->d);
	s->dirty = (struct rect){ 0, 0, 0, 0 };
}

static void draw_cell(struct scon *s, int col, int row)
{
	if (s->suspended)
		return;
	uint16_t cell = s->grid[row][col];
	glyph(s->d->surf, col * FONT_W, row * FONT_H, (unsigned char)(cell & 0xFF), pal(cell >> 8), pal(cell >> 12), 1);
	touch(s, (struct rect){ col * FONT_W, row * FONT_H, FONT_W, FONT_H });
}

static void draw_cursor(struct scon *s, int show)
{
	if (s->suspended || s->cx >= s->cols)
		return;
	draw_cell(s, s->cx, s->cy);
	if (show)
		fill(s->d->surf, s->cx * FONT_W, s->cy * FONT_H + 14, FONT_W, 2, pal(s->grid[s->cy][s->cx] >> 8));
	s->shown = show;
}

static void redraw_all(struct scon *s)
{
	if (s->suspended)
		return;
	struct surface *sf = s->d->surf;
	surf_noclip(sf);
	fill(sf, 0, 0, sf->w, sf->h, pal(0));
	for (int r = 0; r < s->rows; r++)
		for (int c = 0; c < s->cols; c++)
			if (s->grid[r][c] != (' ' | 0x0700) || (s->grid[r][c] >> 12))
				draw_cell(s, c, r);
	touch(s, (struct rect){ 0, 0, sf->w, sf->h });
	if (s->cursor_on)
		draw_cursor(s, 1);
	sflush(s);
}

/* the size the display gives: whole cells */
static void fit(struct scon *s)
{
	struct surface *sf = s->d->surf;
	s->cols = MAX(1, MIN(sf->w / FONT_W, MAX_COLS));
	s->rows = MAX(1, MIN(sf->h / FONT_H, MAX_ROWS));
	s->cx = MIN(s->cx, s->cols - 1);
	s->cy = MIN(s->cy, s->rows - 1);
}

static void scroll(struct scon *s)
{
	uint16_t blank = (uint16_t)(s->attr << 8 | ' ');
	memmove(&s->grid[0][0], &s->grid[1][0], sizeof s->grid[0] * (size_t)(s->rows - 1));
	for (int k = 0; k < MAX_COLS; k++)
		s->grid[s->rows - 1][k] = blank;
	if (s->suspended)
		return;
	/* one copy, and a blank line: cheap to send */
	struct surface *sf = s->d->surf;
	blit(sf, 0, 0, sf, 0, FONT_H, s->cols * FONT_W, (s->rows - 1) * FONT_H);
	fill(sf, 0, (s->rows - 1) * FONT_H, s->cols * FONT_W, FONT_H, pal(s->attr >> 4));
	touch(s, (struct rect){ 0, 0, s->cols * FONT_W, s->rows * FONT_H });
}

static void newline(struct scon *s)
{
	s->cx = 0;
	if (++s->cy >= s->rows) {
		scroll(s);
		s->cy = s->rows - 1;
	}
}

static void putch(struct scon *s, char ch)
{
	switch (ch) {
	case '\n':
		newline(s);
		break;
	case '\r':
		s->cx = 0;
		break;
	case '\b':
		if (s->cx > 0) {
			s->cx--;
		} else if (s->cy > 0) {
			s->cy--;
			s->cx = s->cols - 1;
		}
		break;
	case '\t':
		do {
			if (s->cx >= s->cols)
				newline(s);
			s->grid[s->cy][s->cx] = (uint16_t)(s->attr << 8 | ' ');
			draw_cell(s, s->cx, s->cy);
			s->cx++;
		} while (s->cx % 8);
		break;
	case 7:
		break;
	default:
		if (s->cx >= s->cols)
			newline(s);
		s->grid[s->cy][s->cx] = (uint16_t)(s->attr << 8 | (uint8_t)ch);
		draw_cell(s, s->cx, s->cy);
		s->cx++;
	}
}

static void s_write(struct console *c, const char *str, size_t n)
{
	struct scon *s = c->priv;
	if (s->shown)
		draw_cursor(s, 0);
	while (n--)
		putch(s, *str++);
	if (s->cursor_on)
		draw_cursor(s, 1);
	s->blink_at = uptime_ms() + BLINK_MS;
	sflush(s);
}

static void s_rawchar(struct console *c, int ch)
{
	struct scon *s = c->priv;
	if (s->cx >= s->cols)
		return;
	if (s->shown)
		draw_cursor(s, 0);
	s->grid[s->cy][s->cx] = (uint16_t)(s->attr << 8 | (uint8_t)ch);
	draw_cell(s, s->cx, s->cy);
	s->cx++;
	if (s->cursor_on)
		draw_cursor(s, 1);
	sflush(s);
}

static void s_clear(struct console *c)
{
	struct scon *s = c->priv;
	uint16_t blank = (uint16_t)(s->attr << 8 | ' ');
	for (int r = 0; r < MAX_ROWS; r++)
		for (int k = 0; k < MAX_COLS; k++)
			s->grid[r][k] = blank;
	s->cx = s->cy = 0;
	if (!s->suspended) {
		struct surface *sf = s->d->surf;
		fill(sf, 0, 0, sf->w, sf->h, pal(s->attr >> 4));
		touch(s, (struct rect){ 0, 0, sf->w, sf->h });
		if (s->cursor_on)
			draw_cursor(s, 1);
		sflush(s);
	}
}

static void s_cursor(struct console *c, int on)
{
	struct scon *s = c->priv;
	if (on == s->cursor_on)
		return;
	s->cursor_on = on;
	draw_cursor(s, on);
	sflush(s);
}

static void s_size(struct console *c, int *w, int *h)
{
	struct scon *s = c->priv;
	*w = s->cols;
	*h = s->rows;
}

static void s_gotoxy(struct console *c, int x, int y)
{
	struct scon *s = c->priv;
	if (s->shown)
		draw_cursor(s, 0);
	s->cx = MAX(0, MIN(x, s->cols - 1));
	s->cy = MAX(0, MIN(y, s->rows - 1));
	if (s->cursor_on)
		draw_cursor(s, 1);
	sflush(s);
}

static void s_attr(struct console *c, int fg, int bg)
{
	struct scon *s = c->priv;
	s->attr = ((bg & 15) << 4) | (fg & 15);
}

/* keys from the display's events; the cursor blinks while we're asked */
static int s_getkey(struct console *c)
{
	struct scon *s = c->priv;
	if (s->suspended)
		return -1;
	struct display *d = s->d;
	struct event e;
	while (s->kh == s->kt && d->next_event(d, &e)) {
		if (e.type == EV_KEY) {
			if ((s->kh + 1) % 64 != s->kt) {
				s->keys[s->kh] = e.key;
				s->kh = (s->kh + 1) % 64;
			}
		} else if (e.type == EV_CLOSE) {
			s->eof = 1;
		} else if (e.type == EV_RESIZE) {
			fit(s);		/* the display changed size (a new surface) */
			redraw_all(s);
		}
	}
	if (!d->alive)
		s->eof = 1;
	if (s->kh != s->kt) {
		int k = s->keys[s->kt];
		s->kt = (s->kt + 1) % 64;
		return k;
	}
	if (s->eof)
		return KEY_EOF;
	if (s->cursor_on && uptime_ms() >= s->blink_at) {
		draw_cursor(s, !s->shown);
		s->blink_at = uptime_ms() + BLINK_MS;
		sflush(s);
	}
	return -1;
}

struct console *scon_new(struct display *d, const char *name)
{
	struct scon *s = kzalloc(sizeof *s);
	if (!s)
		return NULL;
	s->d = d;
	s->attr = 0x07;
	s->c.name = name;
	s->c.write = s_write;
	s->c.getkey = s_getkey;
	s->c.clear = s_clear;
	s->c.cursor = s_cursor;
	s->c.size = s_size;
	s->c.gotoxy = s_gotoxy;
	s->c.attr = s_attr;
	s->c.rawchar = s_rawchar;
	s->c.priv = s;
	d->priv = s;
	fit(s);
	for (int r = 0; r < MAX_ROWS; r++)
		for (int k = 0; k < MAX_COLS; k++)
			s->grid[r][k] = 0x0720;
	redraw_all(s);
	return &s->c;
}

void scon_free(struct console *c)
{
	struct scon *s = c->priv;
	s->d->priv = NULL;
	kfree(s);
}

int console_is_scon(struct console *c)
{
	return c && c->write == s_write;
}

/* a program wants the whole display (a DOS game, WIN): it's theirs till they close it */
struct display *scon_take_display(struct console *c, int *err)
{
	struct scon *s = c->priv;
	if (s->suspended) {
		*err = E_ACCES;
		return NULL;
	}
	if (s->shown)
		draw_cursor(s, 0);
	sflush(s);
	s->suspended = 1;
	return s->d;
}

/* and back: the text as it was, and anything written meanwhile */
void scon_give_back(struct display *d)
{
	struct scon *s = d->priv;
	if (!s || !s->suspended)
		return;
	s->suspended = 0;
	fit(s);
	redraw_all(s);
}
