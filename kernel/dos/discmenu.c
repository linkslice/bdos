/*
 * discmenu.c - Ctrl+Alt+D in a DOS program: the CD drives' changers, over
 * the game, with the game paused. Up and down choose a disc (or Eject),
 * Enter puts it in, Esc goes back; a line at the top says what's in now.
 *
 * It draws on the program's own screen: the display, for a graphics mode,
 * or the console, for text -- so it works on the machine's screen, in a
 * desktop window and across the network alike.
 */
#include "dos.h"
#include "../gui/gfx.h"
#include "../cdrom.h"

#define MENU_W	48
#define MENU_H	22
#define BANNER_MS 3000

struct item {
	struct changer *ch;
	int disc;		/* -1: eject; -2: the drive's heading */
};

struct menu {
	struct item it[MENU_H];
	int n, sel;
	uint16_t cell[MENU_H][MENU_W];	/* attr << 8 | char */
	int rows;
};

static void put(struct menu *m, int row, int col, int attr, const char *s)
{
	for (; *s && col < MENU_W; s++, col++)
		m->cell[row][col] = (uint16_t)(attr << 8 | (uint8_t)*s);
}

/* the box, as character cells (the CP437 double line) */
static void layout(struct menu *m)
{
	const int frame = 0x1F, text = 0x1F, hi = 0x70, dim = 0x17;
	m->rows = MIN(m->n + 4, MENU_H);
	for (int r = 0; r < m->rows; r++)
		for (int c = 0; c < MENU_W; c++)
			m->cell[r][c] = (uint16_t)(frame << 8 | ' ');
	for (int c = 1; c < MENU_W - 1; c++) {
		m->cell[0][c] = (uint16_t)(frame << 8 | 0xCD);
		m->cell[m->rows - 1][c] = (uint16_t)(frame << 8 | 0xCD);
	}
	for (int r = 1; r < m->rows - 1; r++) {
		m->cell[r][0] = (uint16_t)(frame << 8 | 0xBA);
		m->cell[r][MENU_W - 1] = (uint16_t)(frame << 8 | 0xBA);
	}
	m->cell[0][0] = (uint16_t)(frame << 8 | 0xC9);
	m->cell[0][MENU_W - 1] = (uint16_t)(frame << 8 | 0xBB);
	m->cell[m->rows - 1][0] = (uint16_t)(frame << 8 | 0xC8);
	m->cell[m->rows - 1][MENU_W - 1] = (uint16_t)(frame << 8 | 0xBC);
	put(m, 0, 3, frame, " Disc Changer ");
	put(m, m->rows - 1, 3, frame, " \x18\x19 choose  Enter put in  Esc back ");
	for (int i = 0; i < m->n && i + 2 < m->rows - 1; i++) {
		struct item *it = &m->it[i];
		char line[MENU_W];
		int row = i + 2;
		if (it->disc == -2) {
			snprintf(line, sizeof line, "%c:  %s", changer_letter(it->ch), changer_source(it->ch));
			put(m, row, 2, text, line);
			continue;
		}
		int in = it->disc >= 0 ? changer_current(it->ch) == it->disc : changer_current(it->ch) < 0;
		if (it->disc >= 0) {
			char name[33];
			strncpy0(name, changer_disc_name(it->ch, it->disc), sizeof name);
			snprintf(line, sizeof line, " %s %2d  %s ", in ? "\x10" : " ", it->disc + 1, name);
		}
		else
			snprintf(line, sizeof line, " %s     %s ", in ? "\x10" : " ", in ? "(empty)" : "Eject");
		int a = i == m->sel ? hi : it->disc < 0 ? dim : text;
		if (i == m->sel)
			for (int c = 3; c < MENU_W - 3; c++)
				m->cell[row][c] = (uint16_t)(hi << 8 | ' ');
		put(m, row, 3, a, line);
	}
}

static int build(struct menu *m)
{
	char letters[27];
	int nd = cd_letters(letters, sizeof letters);
	m->n = 0;
	for (int d = 0; d < nd; d++) {
		struct changer *ch = changer_at(letters[d]);
		if (!ch || m->n + 2 > MENU_H - 4)
			continue;
		m->it[m->n++] = (struct item){ ch, -2 };
		for (int k = 0; k < changer_discs(ch) && m->n < MENU_H - 5; k++)
			m->it[m->n++] = (struct item){ ch, k };
		m->it[m->n++] = (struct item){ ch, -1 };
	}
	return m->n;
}

static void move(struct menu *m, int dir)
{
	int i = m->sel;
	do
		i = (i + dir + m->n) % m->n;
	while (m->it[i].disc == -2 && i != m->sel);
	m->sel = i;
}

/* ---- drawing it ---------------------------------------------------------------------------- */

static uint32_t pal(int i)
{
	return col(cga_palette[i & 15]);
}

static void draw_on_display(struct dosbox *b, struct menu *m)
{
	struct display *d = b->disp;
	bkl_enter();
	display_lock(d);
	if (d->alive && d->surf && d->surf->px) {
		struct surface *s = d->surf;
		int x0 = MAX(0, (s->w - MENU_W * 8) / 2), y0 = MAX(0, (s->h - m->rows * 16) / 2);
		surf_noclip(s);
		for (int r = 0; r < m->rows; r++)
			for (int c = 0; c < MENU_W; c++) {
				uint16_t cell = m->cell[r][c];
				glyph(s, x0 + c * 8, y0 + r * 16, (unsigned char)(cell & 0xFF), pal(cell >> 8), pal(cell >> 12), 1);
			}
		struct rect all = { 0, 0, s->w, s->h };
		surf_touched(s, all);
		d->flush(d, all);
		if (d->commit)
			d->commit(d);
	}
	display_unlock(d);
	bkl_leave();
}

static void draw_on_console(struct menu *m)
{
	bkl_enter();
	int cols, rows;
	con_size(&cols, &rows);
	int x0 = MAX(0, (cols - MENU_W) / 2), y0 = MAX(0, (rows - m->rows) / 2);
	con_cursor(0);
	for (int r = 0; r < m->rows; r++) {
		con_gotoxy(x0, y0 + r);
		int last = -1;
		for (int c = 0; c < MENU_W && x0 + c < cols; c++) {
			uint16_t cell = m->cell[r][c];
			if ((cell >> 8) != last) {
				last = cell >> 8;
				con_attr(last & 15, last >> 4);
			}
			con_rawchar((unsigned char)(cell & 0xFF));
		}
	}
	bkl_leave();
}

/* ---- keys -------------------------------------------------------------------------------- */

enum { K_NONE, K_UP, K_DOWN, K_ENTER, K_ESC, K_EJECT, K_QUIT };

static int map_cooked(int k)
{
	switch (k) {
	case KEY_UP: return K_UP;
	case KEY_DOWN: return K_DOWN;
	case '\r': case '\n': case ' ': return K_ENTER;
	case 27: return K_ESC;
	case 'e': case 'E': return K_EJECT;
	case KEY_EOF: return K_QUIT;
	}
	return K_NONE;
}

static int next_key(struct dosbox *b)
{
	if (!b->disp) {
		bkl_enter();
		int k = con_pollkey();
		bkl_leave();
		return k < 0 ? K_NONE : map_cooked(k);
	}
	struct display *d = b->disp;
	struct event e;
	int r = K_NONE;
	bkl_enter();
	while (r == K_NONE && d->next_event(d, &e)) {
		if (e.type == EV_RAWKEY) {
			uint8_t sc = (uint8_t)e.key;
			/* the game keeps track of Ctrl and Alt: their releases are its */
			if (sc == 0x9D || sc == 0xB8) {
				if (sc == 0x9D)
					b->raw_ctrl = 0;
				else
					b->raw_alt = 0;
				r = -(int)sc;
				break;
			}
			switch (sc) {
			case 0x48: r = K_UP; break;
			case 0x50: r = K_DOWN; break;
			case 0x1C: case 0x39: r = K_ENTER; break;
			case 0x01: r = K_ESC; break;
			case 0x12: r = K_EJECT; break;
			}
		} else if (e.type == EV_KEY && !b->raw_seen) {
			r = map_cooked(e.key);
		} else if (e.type == EV_CLOSE) {
			r = K_QUIT;
		}
	}
	if (!d->alive)
		r = K_QUIT;
	bkl_leave();
	return r;
}

/* ---- the banner -------------------------------------------------------------------------- */

static void say(struct dosbox *b, struct changer *ch)
{
	int cur = changer_current(ch);
	if (cur < 0)
		snprintf(b->banner, sizeof b->banner, " %c: is empty ", changer_letter(ch));
	else
		snprintf(b->banner, sizeof b->banner, " %c: %s (%d of %d) ", changer_letter(ch),
		    changer_disc_name(ch, cur), cur + 1, changer_discs(ch));
	b->banner_until = uptime_ms() + BANNER_MS;
	b->banner_up = 1;
}

/* over a graphics frame (frame() calls this after drawing it) */
void disc_banner_draw(struct dosbox *b, struct surface *s)
{
	if (!b->banner_up)
		return;
	if (uptime_ms() >= b->banner_until) {
		b->banner_up = 0;
		b->vga.dirty = 1;	/* and the next frame is the game's alone */
		return;
	}
	int x = 8, y = 8;
	surf_noclip(s);
	for (const char *p = b->banner; *p && x + 8 <= s->w; p++, x += 8)
		glyph(s, x, y, (unsigned char)*p, pal(15), pal(1), 1);
	b->vga.dirty = 1;		/* keep drawing it till it goes */
}

/* in text mode, on the console: the top line, put back when it's time */
static void banner_text(struct dosbox *b)
{
	bkl_enter();
	con_gotoxy(0, 0);
	con_attr(15, 1);
	for (const char *p = b->banner; *p; p++)
		con_rawchar((unsigned char)*p);
	bkl_leave();
}

/* ---- a program writing lines (its screen is the console's scrollback): a line of its own --- */

static void line_menu(struct dosbox *b, struct menu *m)
{
	char out[256];
	int o = 0, num = 0;
	struct item pick[9];
	bkl_enter();
	o += snprintf(out + o, sizeof out - (size_t)o, "\r\n[Disc Changer]");
	for (int i = 0; i < m->n; i++) {
		struct item *it = &m->it[i];
		if (it->disc == -2)
			o += snprintf(out + o, sizeof out - (size_t)o, "  %c:", changer_letter(it->ch));
		else if (it->disc >= 0 && num < 9) {
			pick[num++] = *it;
			o += snprintf(out + o, sizeof out - (size_t)o, " %d %s%s", num, changer_disc_name(it->ch, it->disc),
			    changer_current(it->ch) == it->disc ? "*" : "");
		}
		if (o > (int)sizeof out - 48)
			break;
	}
	o += snprintf(out + o, sizeof out - (size_t)o, "\r\n  a number puts it in, E ejects, Esc goes back: ");
	con_write(out);
	bkl_leave();
	struct changer *ch = NULL;
	int want = -2;
	for (;;) {
		bkl_enter();
		int k = con_pollkey();
		bkl_leave();
		if (k < 0) {
			sleep_ms(10);
			continue;
		}
		if (k == KEY_EOF) {
			b->want_quit = 1;
			break;
		}
		if (k == 27)
			break;
		if (k >= '1' && k < '1' + num) {
			ch = pick[k - '1'].ch;
			want = pick[k - '1'].disc;
			break;
		}
		if (k == 'e' || k == 'E') {
			ch = m->it[0].ch;	/* (the first drive; DISC X: EJECT for the others) */
			want = -1;
			break;
		}
	}
	bkl_enter();
	if (ch) {
		int r = changer_insert(ch, want);
		if (r)
			snprintf(out, sizeof out, "can't read it: %s\r\n", vfs_strerror(r));
		else if (want < 0)
			snprintf(out, sizeof out, "%c: is empty\r\n", changer_letter(ch));
		else
			snprintf(out, sizeof out, "%c: %s\r\n", changer_letter(ch), changer_disc_name(ch, want));
	} else {
		snprintf(out, sizeof out, "\r\n");
	}
	con_write(out);
	bkl_leave();
}

/* ---- the menu ------------------------------------------------------------------------------ */

void disc_menu(struct dosbox *b)
{
	struct menu *m = kzalloc(sizeof *m);
	if (!m)
		return;
	bkl_enter();
	int n = build(m);
	bkl_leave();
	uint64_t t0 = uptime_ms();
	if (!n) {
		snprintf(b->banner, sizeof b->banner, " No CD drives: MOUNT D: GAME.ISO, or bterm -d game.iso ");
		if (!b->disp && !b->text_live) {
			bkl_enter();
			con_write("\r\n[No CD drives: MOUNT D: GAME.ISO, or bterm -d game.iso]\r\n");
			bkl_leave();
		} else {
			b->banner_until = uptime_ms() + BANNER_MS;
			b->banner_up = 1;
			if (!b->disp)
				banner_text(b);
		}
		kfree(m);
		return;
	}
	if (!b->disp && !b->text_live) {
		line_menu(b, m);
		b->lag_us += (uptime_ms() - t0) * 1000;
		if (b->g_last_wall)
			b->g_last_wall = uptime_us();
		kfree(m);
		return;
	}
	/* start on the disc that's in */
	for (int i = 0; i < m->n; i++)
		if (m->it[i].disc != -2 && (m->it[i].disc == changer_current(m->it[i].ch) ||
		    (m->it[i].disc == -1 && changer_current(m->it[i].ch) < 0))) {
			m->sel = i;
			break;
		}
	if (m->it[m->sel].disc == -2)
		move(m, 1);
	struct changer *changed = NULL;
	int redraw = 1;
	for (;;) {
		if (redraw) {
			bkl_enter();
			layout(m);
			bkl_leave();
			if (b->disp)
				draw_on_display(b, m);
			else
				draw_on_console(m);
			redraw = 0;
		}
		int k = next_key(b);
		if (k < 0) {			/* a Ctrl or Alt let go: the game hears it */
			kbd_push(b, (uint8_t)-k);
			continue;
		}
		if (k == K_NONE) {
			sleep_ms(10);
			continue;
		}
		if (k == K_QUIT) {
			b->want_quit = 1;
			break;
		}
		if (k == K_ESC)
			break;
		if (k == K_UP || k == K_DOWN) {
			move(m, k == K_UP ? -1 : 1);
			redraw = 1;
			continue;
		}
		struct item *it = &m->it[m->sel];
		int want = k == K_EJECT ? -1 : it->disc;
		bkl_enter();
		int r = changer_insert(it->ch, want);
		bkl_leave();
		if (r) {
			snprintf(b->banner, sizeof b->banner, " %c: can't read %s ", changer_letter(it->ch),
			    want >= 0 ? changer_disc_name(it->ch, want) : "it");
			b->banner_until = uptime_ms() + BANNER_MS;
			b->banner_up = 1;
			changed = NULL;
		} else {
			changed = it->ch;
		}
		break;
	}
	if (changed) {
		bkl_enter();
		say(b, changed);
		bkl_leave();
	}
	/* the game's own picture back, all of it; and its clock didn't run meanwhile */
	b->lag_us += (uptime_ms() - t0) * 1000;
	if (b->g_last_wall)
		b->g_last_wall = uptime_us();
	b->vga.dirty = 1;
	b->shadow_of = NULL;
	memset(b->shown, 0xFF, sizeof b->shown);
	if (!b->disp && b->banner_up)
		b->banner_text_pending = 1;
	kfree(m);
}

/* text mode: show the banner once the screen's been put back, and take it away after */
void disc_banner_text(struct dosbox *b)
{
	if (b->banner_text_pending) {
		b->banner_text_pending = 0;
		banner_text(b);
		return;
	}
	if (b->banner_up && uptime_ms() >= b->banner_until) {
		b->banner_up = 0;
		memset(b->shown, 0xFF, sizeof b->shown);
	}
}
