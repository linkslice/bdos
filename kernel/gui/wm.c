/*
 * wm.c - the bdos desktop: windows, input, and putting it all on screen.
 *
 * A window manager runs in the task that typed WIN and draws on a display:
 * the screen, the inside of a terminal window on another desktop (so WIN
 * inside WIN nests), or a network connection. Each terminal window is a
 * full console with its own shell task; tasks remember which desktop they
 * live on, so DESK$, DRAW$, MOUSE$ and the Win_ SYS calls act on *their*
 * desktop, nested or not.
 *
 * Look and behaviour follow Windows 3.1; the plumbing follows Plan 9's rio.
 * Program Manager (progman.c), File Manager (filemgr.c), forms (forms.c)
 * and menus and scroll bars (ui.c) plug in through struct winops.
 */
#include "wm_int.h"
#include "../sysapi.h"
#include "../settings.h"

int dclick_ms = 450;
#define DCLICK_MS dclick_ms

struct theme theme = {
	0x2F6F73, 0xC3C3C3, 0x1D3A8A, 0x7B7B7B, 0xFFFFFF, 0x000000, 0x1D3A8A,
	0xFFFFFF, { 0 }, 0, 0,
};

struct wm *all_wms;	/* every running desktop, for settings changes */
static struct termwin *zombies;
static int mirror_terms = 1;	/* copy terminal window text to COM1 (tests) */

uint32_t P(uint32_t rgb) { return col(rgb); }

/* ---- the desktop's lock ------------------------------------------------------------ */

void desk_lock(struct wm *wm)
{
	struct task *me = task_current();
	if (wm->lock.owner == me) {
		wm->lock.depth++;
		return;
	}
	for (;;) {
		struct task *none = NULL;
		if (__atomic_compare_exchange_n(&wm->lock.owner, &none, me, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			wm->lock.depth = 1;
			return;
		}
		/* busy: wait without the big lock, so the rest of the system goes on */
		int depth = bkl_release_all();
		while (__atomic_load_n(&wm->lock.owner, __ATOMIC_RELAXED))
			task_yield();
		bkl_reacquire(depth);
	}
}

void desk_unlock(struct wm *wm)
{
	if (wm->lock.owner != task_current())
		panic("desk_unlock: not ours");
	if (--wm->lock.depth == 0)
		__atomic_store_n(&wm->lock.owner, NULL, __ATOMIC_RELEASE);
}

void display_lock(struct display *d)
{
	if (d && d->lock)
		d->lock(d);
}

void display_unlock(struct display *d)
{
	if (d && d->unlock)
		d->unlock(d);
}

void wm_dirty(struct wm *wm, struct rect r)
{
	wm->dirty = rect_union(wm->dirty, r);
}

void wm_dirty_all(struct wm *wm)
{
	wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
}

struct rect frame_rect(struct win *w)
{
	return (struct rect){ w->x, w->y, w->w, w->h };
}

void win_dirty(struct win *w, struct rect r)
{
	if (w->closed || w->minimized)
		return;
	r.x += w->x + CLIENT_X;
	r.y += w->y + CLIENT_Y;
	wm_dirty(w->wm, rect_isect(r, frame_rect(w)));
}

void win_dirty_all(struct win *w)
{
	if (!w->closed && !w->minimized)
		wm_dirty(w->wm, frame_rect(w));
}

static struct rect icon_rect(struct wm *wm, struct win *w);

/* ---- event queues ----------------------------------------------------------- */

void evq_push(struct evq *q, const struct event *e)
{
	int next = (q->head + 1) % EVQ_SIZE;
	if (next == q->tail)
		return;
	q->e[q->head] = *e;
	q->head = next;
}

int evq_pop(struct evq *q, struct event *e)
{
	if (q->tail == q->head)
		return 0;
	*e = q->e[q->tail];
	q->tail = (q->tail + 1) % EVQ_SIZE;
	return 1;
}

/* ---- windows ------------------------------------------------------------------ */

struct win *win_new(struct wm *wm, int kind, const char *title, int x, int y, int cw, int ch)
{
	struct win *w = kzalloc(sizeof *w);
	if (!w)
		return NULL;
	if (surf_init(&w->content, cw, ch)) {
		kfree(w);
		return NULL;
	}
	fill(&w->content, 0, 0, cw, ch, P(0xFFFFFF));
	gfx_track(&w->content, wm->disp->rec);
	w->id = ++wm->next_id;
	w->kind = kind;
	w->minimizable = kind != W_DIALOG;
	strncpy0(w->title, title, sizeof w->title);
	w->wm = wm;
	w->w = FRAME_W(cw);
	w->h = FRAME_H(ch);
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	if (x < 0)	/* cascade new windows */
		x = 40 + (wm->next_id * 28) % MAX(40, sw - w->w - 60);
	if (y < 0)
		y = 30 + (wm->next_id * 24) % MAX(30, sh - w->h - 50);
	/* keep it on its desktop when it fits */
	w->x = MAX(0, MIN(x, w->w <= sw ? sw - w->w : sw - 40));
	w->y = MAX(0, MIN(y, w->h <= sh ? sh - w->h : sh - 30));
	/* on top */
	struct win **pp = &wm->wins;
	while (*pp)
		pp = &(*pp)->next;
	*pp = w;
	if (wm->focus)
		win_dirty_all(wm->focus);
	wm->focus = w;
	win_dirty_all(w);
	return w;
}

void raise_win(struct wm *wm, struct win *w)
{
	struct win **pp = &wm->wins;
	while (*pp && *pp != w)
		pp = &(*pp)->next;
	if (!*pp)
		return;
	*pp = w->next;
	w->next = NULL;
	pp = &wm->wins;
	while (*pp)
		pp = &(*pp)->next;
	*pp = w;
	if (wm->focus && wm->focus != w) {
		win_dirty_all(wm->focus);
		if (wm->focus->minimized)
			wm_dirty(wm, icon_rect(wm, wm->focus));
	}
	wm->focus = w;
	win_dirty_all(w);
	if (w->minimized)
		wm_dirty(wm, icon_rect(wm, w));
}

/* the top window that's open on the desktop (not minimized) */
struct win *topmost(struct wm *wm)
{
	struct win *t = NULL;
	for (struct win *w = wm->wins; w; w = w->next)
		if (!w->minimized)
			t = w;
	return t;
}

static void dnd_end(struct wm *wm);

void win_destroy(struct wm *wm, struct win *w)
{
	struct win **pp = &wm->wins;
	while (*pp && *pp != w)
		pp = &(*pp)->next;
	if (*pp)
		*pp = w->next;
	if (w->minimized)
		wm_dirty_all(wm);	/* the icons along the bottom shuffle up */
	else
		wm_dirty(wm, frame_rect(w));
	w->closed = 1;
	if (wm->focus == w)
		wm->focus = topmost(wm);
	if (wm->focus)
		win_dirty_all(wm->focus);
	if (w == wm->progman)
		wm->progman = NULL;
	if (wm->grab == w)
		wm->grab = NULL;
	if (wm->resizing == w)
		wm->resizing = NULL;
	if (wm->drag == w)
		wm->drag = NULL;
	if (wm->popup && wm->popup->owner == w)
		popup_close(wm);
	if (wm->dnd.src == w)
		dnd_end(wm);
	if (w->term) {
		struct termwin *t = w->term;
		t->closed = 1;	/* a display hosted here sees the close */
		t->win = NULL;
		/* the shell task may still be running: free the console when it dies */
		t->next_zombie = zombies;
		zombies = t;
	}
	if (w->dlg && w->dlg->purpose == 3) {
		if (!w->dlg->done)
			w->dlg->done = 2;	/* the waiting program frees it */
		return;
	}
	if (w->kind == W_CANVAS && w->owner) {
		/* the owning program sees the close; free when it lets go */
		struct event e = { EV_CLOSE, 0, 0, 0, 0 };
		evq_push(&w->events, &e);
		w->wm = NULL;
		surf_free(&w->content);
		return;	/* canvas wins are freed by their owner via Win_Close */
	}
	if (w->ops && w->ops->destroy)
		w->ops->destroy(w);
	kfree(w->dlg);
	surf_free(&w->content);
	kfree(w);
}

static void tw_free_history(struct termwin *t);
static void reap_zombies(void)
{
	struct termwin **pp = &zombies;
	while (*pp) {
		struct termwin *t = *pp;
		if (!t->task || !task_alive(t->task)) {
			*pp = t->next_zombie;
			kfree(t->grid);
			tw_free_history(t);
			kfree(t->paste);
			kfree(t);
		} else {
			pp = &t->next_zombie;
		}
	}
}

/* Minimized windows sit as icons along the bottom of the desktop, left to
 * right in the order they went there, Windows 3.1 style. */
static struct rect icon_rect(struct wm *wm, struct win *w)
{
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	int per_row = MAX(1, (sw - 16) / ICON_SLOT_W), i = 0;
	for (struct win *v = wm->wins; v && v != w; v = v->next)
		if (v->minimized)
			i++;
	return (struct rect){ 8 + (i % per_row) * ICON_SLOT_W, sh - ICON_SLOT_H - (i / per_row) * ICON_SLOT_H,
	    ICON_SLOT_W, ICON_SLOT_H };
}

void win_icon_dirty(struct win *w)
{
	if (w->minimized)
		wm_dirty(w->wm, icon_rect(w->wm, w));
}

static struct win *icon_at(struct wm *wm, int x, int y)
{
	for (struct win *w = wm->wins; w; w = w->next)
		if (w->minimized && rect_has(icon_rect(wm, w), x, y))
			return w;
	return NULL;
}

void win_minimize(struct win *w, int on)
{
	struct wm *wm = w->wm;
	if (on == w->minimized || (on && !w->minimizable))
		return;
	if (on) {
		wm_dirty(wm, frame_rect(w));
		w->minimized = 1;
		/* to the end of the list, so it takes the next icon slot */
		struct win **pp = &wm->wins;
		while (*pp && *pp != w)
			pp = &(*pp)->next;
		if (*pp) {
			*pp = w->next;
			w->next = NULL;
			pp = &wm->wins;
			while (*pp)
				pp = &(*pp)->next;
			*pp = w;
		}
		wm_dirty(wm, icon_rect(wm, w));
		if (wm->focus == w) {
			struct win *t = topmost(wm);
			wm->focus = t ? t : w;	/* nothing open: the icon keeps the keyboard */
			if (t)
				win_dirty_all(t);
		}
	} else {
		wm_dirty_all(wm);
		w->minimized = 0;
		raise_win(wm, w);
	}
}

static int title_buttons(struct win *w, int *close_x, int *max_x, int *min_x)
{
	/* positions within the frame, -1 when absent */
	int bx = w->w - BORDER - 18;
	*close_x = bx;
	*max_x = win_can_resize(w) ? bx - 18 : -1;
	*min_x = w->minimizable ? (*max_x >= 0 ? *max_x - 18 : bx - 18) : -1;
	return 0;
}

/* ---- the terminal console ------------------------------------------------------ */

#define SB_LINES 500

/* line L: history first (0 is the oldest kept), then the screen's rows */
static const uint16_t *tw_line(struct termwin *t, int L, int *len)
{
	if (L < 0) {
		*len = 0;
		return NULL;
	}
	if (L < t->sbcount) {
		int i = (t->sbhead + L) % SB_LINES;
		*len = t->sblen[i];
		return t->sb[i];
	}
	L -= t->sbcount;
	if (L >= t->rows) {
		*len = 0;
		return NULL;
	}
	*len = t->cols;
	return t->grid + L * t->cols;
}

static int tw_selected(struct termwin *t, int L, int c)
{
	if (!t->sel)
		return 0;
	int al = t->sa_line, ac = t->sa_col, bl = t->se_line, bc = t->se_col;
	if (bl < al || (bl == al && bc < ac)) {
		int x = al; al = bl; bl = x;
		x = ac; ac = bc; bc = x;
	}
	if (L < al || L > bl)
		return 0;
	if (L == al && c < ac)
		return 0;
	if (L == bl && c > bc)
		return 0;
	return 1;
}

/* how far back we are, top right, while looking at history */
static void tw_overlay(struct termwin *t)
{
	if (t->child || t->closed || !t->view)
		return;
	char msg[40];
	snprintf(msg, sizeof msg, " %d of %d lines back ", t->view, t->sbcount);
	int w = (int)strlen(msg) * FONT_W, x = t->win->content.w - w - 2;
	text_bg(&t->win->content, x, 0, msg, P(0), P(0xFFFF55));
	win_dirty(t->win, (struct rect){ x, 0, w, FONT_H });
}

/* draw what's at column c of the window's text row sr */
static void tw_draw(struct termwin *t, int c, int sr)
{
	if (t->child || t->closed || c >= t->cols || sr >= t->rows)
		return;
	int L = t->sbcount - t->view + sr, len;
	const uint16_t *ln = tw_line(t, L, &len);
	uint16_t cell = ln && c < len ? ln[c] : 0x0720;
	int fgi = (cell >> 8) & 0xF, bgi = (cell >> 12) & 0xF;
	if (tw_selected(t, L, c)) {
		int x = fgi; fgi = bgi; bgi = x;
		if (fgi == bgi)
			fgi = 0, bgi = 7;
	}
	uint32_t fg = P(cga_palette[fgi]), bg = P(cga_palette[bgi]);
	glyph(&t->win->content, c * FONT_W, sr * FONT_H, (unsigned char)(cell & 0xFF), fg, bg, 1);
	if (!t->view && t->cursor_on && c == t->cx && sr == t->cy)
		fill(&t->win->content, c * FONT_W, sr * FONT_H + 14, FONT_W, 2, fg);
	win_dirty(t->win, (struct rect){ c * FONT_W, sr * FONT_H, FONT_W, FONT_H });
}

/* the screen's row r changed: draw it where it shows, if it does */
static void tw_cell(struct termwin *t, int c, int r)
{
	if (r + t->view < t->rows)
		tw_draw(t, c, r + t->view);
}

static void tw_redraw(struct termwin *t)
{
	if (t->child || t->closed)
		return;
	for (int r = 0; r < t->rows; r++)
		for (int c = 0; c < t->cols; c++)
			tw_draw(t, c, r);
	tw_overlay(t);
}

static void tw_cursor_cell(struct termwin *t)
{
	if (t->cx < t->cols && t->cy < t->rows)
		tw_cell(t, t->cx, t->cy);
}

/* a line goes off the top of the screen: keep it */
static void tw_push_history(struct termwin *t, const uint16_t *row, int cols)
{
	if (!t->sb) {
		t->sb = kzalloc(sizeof(uint16_t *) * SB_LINES);
		t->sblen = kzalloc(sizeof(uint16_t) * SB_LINES);
		if (!t->sb || !t->sblen) {
			kfree(t->sb);
			kfree(t->sblen);
			t->sb = NULL;
			t->sblen = NULL;
			return;
		}
	}
	int len = cols;
	while (len > 0 && (row[len - 1] & 0xFF) == ' ' && (row[len - 1] & 0xF000) == 0)
		len--;
	uint16_t *copy = len ? kmalloc(sizeof(uint16_t) * (size_t)len) : NULL;
	if (len && !copy)
		len = 0;
	if (len)
		memcpy(copy, row, sizeof(uint16_t) * (size_t)len);
	int i;
	if (t->sbcount < SB_LINES) {
		i = (t->sbhead + t->sbcount) % SB_LINES;
		t->sbcount++;
	} else {			/* full: the oldest goes */
		i = t->sbhead;
		kfree(t->sb[i]);
		t->sbhead = (t->sbhead + 1) % SB_LINES;
		if (t->sel || t->selecting) {
			t->sa_line--;
			t->se_line--;
			if (t->sa_line < 0 || t->se_line < 0)
				t->sel = t->selecting = 0;
		}
	}
	t->sb[i] = copy;
	t->sblen[i] = (uint16_t)len;
}

static void tw_free_history(struct termwin *t)
{
	if (t->sb)
		for (int i = 0; i < SB_LINES; i++)
			kfree(t->sb[i]);
	kfree(t->sb);
	kfree(t->sblen);
	t->sb = NULL;
	t->sblen = NULL;
	t->sbcount = t->sbhead = t->view = 0;
	t->sel = t->selecting = 0;
}

static void tw_scroll(struct termwin *t)
{
	if (!t->child)
		tw_push_history(t, t->grid, t->cols);
	memmove(t->grid, t->grid + t->cols, sizeof(uint16_t) * (size_t)(t->cols * (t->rows - 1)));
	for (int c = 0; c < t->cols; c++)
		t->grid[(t->rows - 1) * t->cols + c] = (uint16_t)(t->attr << 8 | ' ');
	if (t->child || t->closed)
		return;
	if (t->view) {
		/* looking back: what's on the screen stays put */
		if (t->view < t->sbcount) {
			t->view++;
			tw_overlay(t);
		} else {
			tw_redraw(t);
		}
		return;
	}
	if (t->sel) {
		tw_redraw(t);	/* the selection moves with its text */
		return;
	}
	/* move the pixels up one text row (as a copy, so a viewer that draws
	 * for itself just copies too) */
	struct surface *s = &t->win->content;
	struct rect save = s->clip;
	surf_noclip(s);
	blit(s, 0, 0, s, 0, FONT_H, s->w, s->h - FONT_H);
	s->clip = save;
	for (int c = 0; c < t->cols; c++) {
		tw_cell(t, c, t->rows - 1);
		if (t->rows > 1)
			tw_cell(t, c, t->rows - 2);	/* the old bottom row: no stale cursor */
	}
	win_dirty(t->win, (struct rect){ 0, 0, s->w, s->h });
}

/* scroll the view back (+) or forward (-) through the history */
static void tw_view(struct termwin *t, int delta)
{
	int v = MAX(0, MIN(t->view + delta, t->sbcount));
	if (v == t->view)
		return;
	t->view = v;
	tw_redraw(t);
}

static void tw_putc(struct termwin *t, char ch)
{
	int ox = t->cx, oy = t->cy;
	switch (ch) {
	case '\n':
		t->cx = 0;
		if (++t->cy >= t->rows) {
			t->cy = t->rows - 1;
			tw_scroll(t);
		}
		break;
	case '\r':
		t->cx = 0;
		break;
	case '\b':
		if (t->cx > 0)
			t->cx--;
		break;
	case '\t':
		do
			tw_putc(t, ' ');
		while (t->cx % 8 && t->cx < t->cols);
		return;
	case 7:
		return;
	default:
		if (t->cx >= t->cols) {
			t->cx = 0;
			if (++t->cy >= t->rows) {
				t->cy = t->rows - 1;
				tw_scroll(t);
			}
			ox = t->cx;
			oy = t->cy;
		}
		t->grid[t->cy * t->cols + t->cx] = (uint16_t)(t->attr << 8 | (uint8_t)ch);
		t->cx++;
		tw_cell(t, t->cx - 1, t->cy);
	}
	if (ox < t->cols && oy < t->rows)
		tw_cell(t, ox, oy);	/* erase the old cursor */
	tw_cursor_cell(t);
}

/* a shell writing into its window: that window's desktop, if it's still there */
static int tw_lock(struct termwin *t)
{
	if (t->closed || !t->wm)
		return 0;
	desk_lock(t->wm);
	if (t->closed) {
		desk_unlock(t->wm);
		return 0;
	}
	return 1;
}

static void tw_rawchar_u(struct console *c, int ch)
{
	struct termwin *t = c->priv;
	if (t->cx >= t->cols || t->cy >= t->rows)
		return;
	int ox = t->cx, oy = t->cy;
	t->grid[t->cy * t->cols + t->cx] = (uint16_t)(t->attr << 8 | (uint8_t)ch);
	t->cx++;
	if (mirror_terms)
		serial_putc(ch >= 0x20 && ch < 0x7F ? (char)ch : ' ');
	tw_cell(t, ox, oy);
	tw_cursor_cell(t);
}

static void tw_rawchar(struct console *c, int ch)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return;
	tw_rawchar_u(c, ch);
	desk_unlock(t->wm);
}

static void tw_write_u(struct console *c, const char *s, size_t n)
{
	struct termwin *t = c->priv;
	for (size_t i = 0; i < n; i++) {
		if (mirror_terms)
			serial_putc(s[i]);
		tw_putc(t, s[i]);
	}
}

static void tw_write(struct console *c, const char *s, size_t n)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return;
	tw_write_u(c, s, n);
	desk_unlock(t->wm);
}

static int tw_getkey_u(struct console *c)
{
	struct termwin *t = c->priv;
	if (t->closed)
		return KEY_EOF;
	if (t->paste) {
		while (t->paste[t->paste_pos] == '\r')
			t->paste_pos++;
		int ch = (unsigned char)t->paste[t->paste_pos];
		if (ch) {
			t->paste_pos++;
			return ch == '\t' ? ' ' : ch;
		}
		kfree(t->paste);
		t->paste = NULL;
	}
	if (t->kt == t->kh)
		return -1;
	int k = t->keys[t->kt];
	t->kt = (t->kt + 1) % 256;
	return k;
}

static int tw_getkey(struct console *c)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return KEY_EOF;
	int k = tw_getkey_u(c);
	desk_unlock(t->wm);
	return k;
}

static void tw_clear_u(struct console *c)
{
	struct termwin *t = c->priv;
	for (int i = 0; i < t->cols * t->rows; i++)
		t->grid[i] = (uint16_t)(t->attr << 8 | ' ');
	t->cx = t->cy = 0;
	t->view = 0;
	t->sel = 0;
	if (mirror_terms)
		serial_putc('\n');
	tw_redraw(t);
}

static void tw_clear(struct console *c)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return;
	tw_clear_u(c);
	desk_unlock(t->wm);
}

static void tw_cursor_u(struct console *c, int on)
{
	struct termwin *t = c->priv;
	t->cursor_on = on;
	tw_cursor_cell(t);
}

static void tw_cursor(struct console *c, int on)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return;
	tw_cursor_u(c, on);
	desk_unlock(t->wm);
}

static void tw_size(struct console *c, int *w, int *h)
{
	struct termwin *t = c->priv;
	*w = t->cols;
	*h = t->rows;
}

static void tw_gotoxy_u(struct console *c, int x, int y)
{
	struct termwin *t = c->priv;
	int ox = t->cx, oy = t->cy;
	if (mirror_terms && y != oy)
		serial_putc('\n');	/* the COM1 transcript, line by line */
	else if (mirror_terms && x > ox)
		for (int i = ox; i < x && i < t->cols; i++)
			serial_putc(' ');
	t->cx = MAX(0, MIN(x, t->cols - 1));
	t->cy = MAX(0, MIN(y, t->rows - 1));
	if (ox < t->cols && oy < t->rows)
		tw_cell(t, ox, oy);
	tw_cursor_cell(t);
}

static void tw_gotoxy(struct console *c, int x, int y)
{
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return;
	tw_gotoxy_u(c, x, y);
	desk_unlock(t->wm);
}

static void tw_attr(struct console *c, int fg, int bg)
{
	struct termwin *t = c->priv;
	t->attr = ((bg & 0xF) << 4) | (fg & 0xF);
}

static void tw_key(struct termwin *t, int k)
{
	int next = (t->kh + 1) % 256;
	if (next != t->kt) {
		t->keys[t->kh] = k;
		t->kh = next;
	}
}

int console_is_termwin(struct console *c)
{
	return c && c->write == tw_write;
}

/* ---- launching things in terminal windows ------------------------------------------ */

struct launch {
	char cmd[200];
	int hold;
	char dir[200];		/* start here ("": where the desktop is) */
	char speed[12];		/* DOS programs' pace ("": as usual) */
};

static void term_task(void *arg)
{
	struct launch *l = arg;
	struct task *me = task_current();
	if (l->dir[0]) {
		struct vstat st;
		char abs[VFS_PATH_MAX];
		if (!vfs_resolve(me->cwd, l->dir, abs, sizeof abs) && !vfs_stat(abs, &st) && st.type == VFS_DIR)
			strncpy0(me->cwd, abs, sizeof me->cwd);
	}
	strncpy0(me->dos_speed, l->speed, sizeof me->dos_speed);
	if (!l->cmd[0]) {
		kfree(l);
		shell_task(NULL);	/* an interactive shell */
		return;
	}
	char *line = kstrdup(l->cmd);
	int hold = l->hold;
	kfree(l);
	shell_task(line);	/* runs one line, frees it */
	if (hold && con_pollkey() != KEY_EOF) {
		con_attr(7, 0);
		con_write("\n[Finished - press any key to close]");
		con_getkey();
	}
}

static const struct winops term_ops;

static struct win *open_term_in(struct wm *wm, const char *title, const char *cmd, int cols, int rows,
    int x, int y, const char *dir, const char *speed);

struct win *open_term(struct wm *wm, const char *title, const char *cmd, int cols, int rows,
    int x, int y)
{
	return open_term_in(wm, title, cmd, cols, rows, x, y, NULL, NULL);
}

static struct win *open_term_in(struct wm *wm, const char *title, const char *cmd, int cols, int rows,
    int x, int y, const char *dir, const char *speed)
{
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	cols = MAX(20, MIN(cols, (sw - 2 * BORDER - 24) / FONT_W));
	rows = MAX(5, MIN(rows, (sh - 2 * BORDER - TITLE_H - 24) / FONT_H));
	struct win *w = win_new(wm, W_TERM, title, x, y, cols * FONT_W, rows * FONT_H);
	if (!w)
		return NULL;
	w->resizable = 1;
	struct termwin *t = kzalloc(sizeof *t);
	t->grid = kmalloc(sizeof(uint16_t) * (size_t)(cols * rows));
	if (!t || !t->grid) {
		win_destroy(wm, w);
		return NULL;
	}
	t->cols = cols;
	t->rows = rows;
	t->attr = 0x07;
	t->win = w;
	t->wm = wm;
	t->con.name = "WINDOW";
	t->con.write = tw_write;
	t->con.getkey = tw_getkey;
	t->con.clear = tw_clear;
	t->con.cursor = tw_cursor;
	t->con.size = tw_size;
	t->con.gotoxy = tw_gotoxy;
	t->con.attr = tw_attr;
	t->con.rawchar = tw_rawchar;
	t->con.priv = t;
	w->term = t;
	w->ops = &term_ops;
	for (int i = 0; i < cols * rows; i++)
		t->grid[i] = 0x0720;
	fill(&w->content, 0, 0, w->content.w, w->content.h, P(0));

	struct launch *l = kzalloc(sizeof *l);
	strncpy0(l->cmd, cmd ? cmd : "", sizeof l->cmd);
	l->hold = cmd && *cmd;
	strncpy0(l->dir, dir ? dir : "", sizeof l->dir);
	strncpy0(l->speed, speed ? speed : "", sizeof l->speed);
	struct task *me = task_current();
	void *save = me->desk;
	me->desk = wm;		/* the new task inherits this desktop */
	t->task = task_spawn(cmd && *cmd ? "program" : "COMMAND", term_task, l, &t->con);
	me->desk = save;
	if (!t->task) {
		kfree(l);
		win_destroy(wm, w);
		return NULL;
	}
	return w;
}

int control_open(void *wm, const char *applet);
void clock_open(struct wm *wm);

static void launch_cmd_in(struct wm *wm, const char *label, const char *cmd, const char *dir,
    const char *speed);

/* the applets (applets.c, installer.c) */
int help_open(void *wm, const char *topic);
int eyes_open(void *wm, const char *arg);
int taskman_open(void *wm, const char *arg);
int dialer_open(void *wm, const char *arg);
int neko_open(void *wm, const char *arg);
int mines_open_app(void *wm, const char *arg);
int calc_open(void *wm, const char *arg);
int charmap_open(void *wm, const char *arg);
static int install_app(void *wm, const char *arg)
{
	extern int installer_open(void *wm);
	(void)arg;
	return installer_open(wm);
}

/* a Program Manager item: where it starts, minimized or not, how fast DOS runs */
void launch_item(struct wm *wm, const char *label, const char *cmd, const char *dir, int minimized,
    const char *speed)
{
	struct win *before = topmost(wm);
	int n0 = 0, n1 = 0;
	for (struct win *w = wm->wins; w; w = w->next)
		n0++;
	launch_cmd_in(wm, label, cmd, dir, speed);
	for (struct win *w = wm->wins; w; w = w->next)
		n1++;
	struct win *after = topmost(wm);
	if (minimized && n1 > n0 && after && after != before && after != wm->progman) {
		win_minimize(after, 1);
		if (before && !before->minimized)
			raise_win(wm, before);	/* the keyboard stays where it was */
	}
}

void launch_cmd(struct wm *wm, const char *label, const char *cmd)
{
	launch_cmd_in(wm, label, cmd, NULL, NULL);
}

static void launch_cmd_in(struct wm *wm, const char *label, const char *cmd, const char *dir,
    const char *speed)
{
	if (!strncasecmp(cmd, "@control", 8)) {
		const char *a = cmd + 8;
		while (*a == ' ')
			a++;
		control_open(wm, *a ? a : NULL);
		return;
	}
	if (!strcasecmp(cmd, "@clock")) {
		clock_open(wm);
		return;
	}
	if (!strcasecmp(cmd, "@pulse")) {
		extern void pulse_open(struct wm *wm);
		pulse_open(wm);
		return;
	}
	/* the applets with windows of their own */
	{
		static const struct { const char *name; int (*open)(void *wm, const char *arg); } apps[] = {
			{ "@install", install_app }, { "@help", help_open }, { "@eyes", eyes_open },
			{ "@taskman", taskman_open }, { "@terminal", dialer_open }, { "@neko", neko_open },
			{ "@mines", mines_open_app }, { "@calc", calc_open }, { "@charmap", charmap_open },
		};
		for (size_t i = 0; i < NELEM(apps); i++) {
			size_t n = strlen(apps[i].name);
			if (!strncasecmp(cmd, apps[i].name, n) && (!cmd[n] || cmd[n] == ' ')) {
				const char *arg = cmd + n;
				while (*arg == ' ')
					arg++;
				apps[i].open(wm, arg);
				return;
			}
		}
	}
	/* plain commands that bring their own window */
	if (!strncasecmp(cmd, "@files", 6)) {
		const char *d = cmd + 6;
		while (*d == ' ')
			d++;
		open_files(wm, *d ? d : dir && *dir ? dir : task_current()->cwd);
		return;
	}
	if (!strcasecmp(cmd, "@progman")) {
		if (wm->progman) {
			win_minimize(wm->progman, 0);
			raise_win(wm, wm->progman);
		} else {
			open_progman(wm);
		}
		return;
	}
	open_term_in(wm, label, cmd, 80, 25, -1, -1, dir, speed);
}

/* An icon for a file, by what it is. */
const char *icon_for_file(const char *name, int is_dir)
{
	if (is_dir)
		return "folder";
	const char *dot = strrchr(name, '.');
	if (!dot)
		return "file";
	static const struct { const char *ext, *icon; } map[] = {
		{ ".BAS", "basic" }, { ".C", "cfile" }, { ".H", "cfile" }, { ".COM", "prompt" },
		{ ".EXE", "prompt" }, { ".BAT", "prompt" }, { ".TXT", "edit" }, { ".MD", "edit" },
		{ ".INI", "gear" }, { ".THM", "palette" }, { ".LNK", "prompt" }, { ".ASM", "edit" },
	};
	for (size_t i = 0; i < NELEM(map); i++)
		if (!strcasecmp(dot, map[i].ext))
			return map[i].icon;
	return "file";
}

/*
 * A Program Manager item (.LNK): the command on the first line, then
 * optional lines
 *   dir=C:\GAMES\KEEN     start there
 *   run=minimized
 *   speed=386             DOS programs' pace (dos.speed's words)
 */
int lnk_read(const char *path, struct lnk *l)
{
	memset(l, 0, sizeof *l);
	size_t len;
	int err;
	char *body = vfs_slurp(path, &len, &err);
	if (!body)
		return err ? err : E_NOENT;
	int first = 1;
	for (char *p = body; *p; ) {
		char *e = p;
		while (*e && *e != '\r' && *e != '\n')
			e++;
		char save = *e;
		*e = 0;
		if (first)
			strncpy0(l->cmd, p, sizeof l->cmd);
		else if (!strncasecmp(p, "dir=", 4))
			strncpy0(l->dir, p + 4, sizeof l->dir);
		else if (!strncasecmp(p, "speed=", 6))
			strncpy0(l->speed, p + 6, sizeof l->speed);
		else if (!strncasecmp(p, "run=", 4))
			l->minimized = !strncasecmp(p + 4, "min", 3);
		first = 0;
		*e = save;
		p = e;
		if (*p == '\r')
			p++;
		if (*p == '\n')
			p++;
		if (!save)
			break;
	}
	kfree(body);
	return 0;
}

int lnk_write(const char *path, const struct lnk *l)
{
	char buf[512];
	int n = snprintf(buf, sizeof buf, "%s\r\n", l->cmd);
	if (l->dir[0])
		n += snprintf(buf + n, sizeof buf - (size_t)n, "dir=%s\r\n", l->dir);
	if (l->minimized)
		n += snprintf(buf + n, sizeof buf - (size_t)n, "run=minimized\r\n");
	if (l->speed[0])
		n += snprintf(buf + n, sizeof buf - (size_t)n, "speed=%s\r\n", l->speed);
	if (!l->dir[0] && !l->minimized && !l->speed[0])
		n = (int)strlen(l->cmd);	/* just the command, as before */
	return vfs_spew(path, buf, (size_t)n);
}

/* Open a file the obvious way: run programs, edit text, browse folders. */
void launch_path(struct wm *wm, const char *path)
{
	struct vstat st;
	if (vfs_stat(path, &st)) {
		wm_message(wm, "Open", "That file isn't there any more.");
		return;
	}
	if (st.type == VFS_DIR) {
		open_files(wm, path);
		return;
	}
	const char *dot = strrchr(path, '.');
	char cmd[VFS_PATH_MAX + 16];
	if (dot && !strcasecmp(dot, ".LNK")) {
		struct lnk l;
		if (lnk_read(path, &l))
			return;
		char label[64];
		strncpy0(label, vfs_basename(path), sizeof label);
		char *ld = strrchr(label, '.');
		if (ld)
			*ld = 0;
		launch_item(wm, label, l.cmd, l.dir, l.minimized, l.speed);
		return;
	}
	if (dot && (!strcasecmp(dot, ".BAS") || !strcasecmp(dot, ".BAT") || !strcasecmp(dot, ".COM") ||
	    !strcasecmp(dot, ".EXE") || !strcasecmp(dot, ".C")))
		snprintf(cmd, sizeof cmd, "\"%s\"", path);
	else
		snprintf(cmd, sizeof cmd, "EDIT \"%s\"", path);
	launch_cmd(wm, vfs_basename(path), cmd);
}

/* Type text into a shell window, as if from the keyboard. */
void term_type(struct win *w, const char *text)
{
	if (!w->term || w->term->child)
		return;
	for (const char *p = text; *p; p++)
		tw_key(w->term, (unsigned char)*p);
}


/* ---- the text clipboard, and selecting text in terminal windows ------------------------- */

static char *clip_text;
static size_t clip_len;
static uint64_t clip_last_write;
static struct task *clip_writer;

void clip_text_set(const char *text, size_t n)
{
	char *c = kmalloc(n + 1);
	if (!c)
		return;
	memcpy(c, text, n);
	c[n] = 0;
	kfree(clip_text);
	clip_text = c;
	clip_len = n;
}

const char *clip_text_get(void)
{
	return clip_text ? clip_text : "";
}

/* the selection as text: lines end in \r\n, trailing blanks dropped */
static char *tw_sel_text(struct termwin *t, size_t *out_n)
{
	int al = t->sa_line, ac = t->sa_col, bl = t->se_line, bc = t->se_col;
	if (bl < al || (bl == al && bc < ac)) {
		int x = al; al = bl; bl = x;
		x = ac; ac = bc; bc = x;
	}
	size_t cap = (size_t)(bl - al + 1) * (size_t)(t->cols + 2) + 1, n = 0;
	char *buf = kmalloc(cap);
	if (!buf)
		return NULL;
	for (int L = al; L <= bl; L++) {
		int len;
		const uint16_t *ln = tw_line(t, L, &len);
		int from = L == al ? ac : 0, to = L == bl ? MIN(bc + 1, len) : len;
		int end = to;
		while (end > from && ln && (ln[end - 1] & 0xFF) == ' ')
			end--;
		for (int c = from; c < end; c++) {
			unsigned char ch = (unsigned char)(ln[c] & 0xFF);
			buf[n++] = ch < 0x20 ? ' ' : (char)ch;
		}
		if (L < bl) {
			buf[n++] = '\r';
			buf[n++] = '\n';
		}
	}
	buf[n] = 0;
	*out_n = n;
	return buf;
}

static void tw_copy(struct termwin *t)
{
	if (!t->sel)
		return;
	size_t n;
	char *txt = tw_sel_text(t, &n);
	if (txt) {
		clip_text_set(txt, n);
		kfree(txt);
	}
}

static void tw_paste(struct termwin *t)
{
	if (!clip_text || !clip_len || t->closed)
		return;
	char *c = kstrdup(clip_text);
	if (!c)
		return;
	/* a paste in the middle of another: the rest of that first */
	if (t->paste && t->paste[t->paste_pos]) {
		size_t a = strlen(t->paste + t->paste_pos), b = strlen(c);
		char *j = kmalloc(a + b + 1);
		if (j) {
			memcpy(j, t->paste + t->paste_pos, a);
			memcpy(j + a, c, b + 1);
		}
		kfree(c);
		c = j;
	}
	kfree(t->paste);
	t->paste = c;
	t->paste_pos = 0;
	if (t->view)
		tw_view(t, -t->view);
}

static void tw_select_all(struct termwin *t)
{
	t->sel = 1;
	t->sa_line = 0;
	t->sa_col = 0;
	t->se_line = t->sbcount + t->rows - 1;
	t->se_col = t->cols - 1;
	tw_redraw(t);
}

static void tw_unselect(struct termwin *t)
{
	if (t->sel) {
		t->sel = 0;
		tw_redraw(t);
	}
}

static int is_word(int ch)
{
	return isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '\\' || ch == ':' || ch == '/';
}

static void term_mouse(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	struct termwin *t = w->term;
	if (!t || t->child)
		return;
	int c = MAX(0, MIN(x / FONT_W, t->cols - 1));
	int sr = y / FONT_H;
	/* dragging past the top or bottom scrolls */
	if (t->selecting && type == EV_MOVE) {
		if (y < 0)
			tw_view(t, 1);
		else if (sr >= t->rows)
			tw_view(t, -1);
	}
	sr = MAX(0, MIN(sr, t->rows - 1));
	int L = t->sbcount - t->view + sr;
	if (type == EV_DOWN && (buttons & 4)) {	/* the middle button pastes */
		tw_paste(t);
		return;
	}
	if (type == EV_DOWN && (buttons & 1)) {
		if (dbl) {	/* a word */
			int len;
			const uint16_t *ln = tw_line(t, L, &len);
			if (!ln || c >= len || !is_word(ln[c] & 0xFF)) {
				tw_unselect(t);
				return;
			}
			int a = c, b = c;
			while (a > 0 && is_word(ln[a - 1] & 0xFF))
				a--;
			while (b + 1 < len && is_word(ln[b + 1] & 0xFF))
				b++;
			t->sel = 1;
			t->selecting = 0;
			t->sa_line = t->se_line = L;
			t->sa_col = a;
			t->se_col = b;
			tw_redraw(t);
			return;
		}
		if ((buttons & MOD_SHIFT) && t->sel) {	/* stretch the one there is */
			t->se_line = L;
			t->se_col = c;
			t->selecting = 1;
			tw_redraw(t);
			return;
		}
		tw_unselect(t);
		t->selecting = 1;
		t->sa_line = t->se_line = L;
		t->sa_col = t->se_col = c;
		return;
	}
	if (type == EV_MOVE && t->selecting) {
		if (L != t->se_line || c != t->se_col || !t->sel) {
			t->sel = 1;
			t->se_line = L;
			t->se_col = c;
			tw_redraw(t);
		}
		return;
	}
	if (type == EV_UP)
		t->selecting = 0;
}

static void term_pick(struct wm *wm, void *arg, int id)
{
	(void)wm;
	struct win *w = arg;
	struct termwin *t = w->term;
	if (!t)
		return;
	switch (id) {
	case 1: tw_copy(t); tw_unselect(t); break;
	case 2: tw_paste(t); break;
	case 3: tw_select_all(t); break;
	case 4: tw_free_history(t); tw_redraw(t); break;
	}
}

static void term_context(struct win *w, int x, int y)
{
	struct termwin *t = w->term;
	if (!t || t->child)
		return;
	struct menu_item it[MENU_MAX];
	int n = 0;
	menu_put(it, &n, "&Copy\tCtrl+Ins", t->sel ? 0 : MI_DISABLED, 1);
	menu_put(it, &n, "&Paste\tShift+Ins", clip_len ? 0 : MI_DISABLED, 2);
	menu_put(it, &n, "", MI_SEP, 0);
	menu_put(it, &n, "Select &All", 0, 3);
	menu_put(it, &n, "Clear &Scrollback", t->sbcount ? 0 : MI_DISABLED, 4);
	popup_open(w->wm, w->x + CLIENT_X + x, w->y + CLIENT_Y + y, it, n, term_pick, w, w);
}

static void term_wheel(struct win *w, int x, int y, int clicks)
{
	(void)x;
	(void)y;
	if (w->term && !w->term->child)
		tw_view(w->term, clicks * 3);
}

static const struct winops term_ops = {
	.mouse = term_mouse,
	.context = term_context,
	.wheel = term_wheel,
	.icon = "prompt",
};

/* Keys the terminal itself takes: Shift+PgUp/PgDn, copy and paste.
 * mods: 1 Shift, 2 Ctrl, 4 Alt. 1 if it took the key. */
static int term_key(struct termwin *t, int k, int mods)
{
	int shift = mods & 1, ctrl = mods & 2;
	if (shift && !ctrl && (k == KEY_PGUP || k == KEY_PGDN)) {
		tw_view(t, (k == KEY_PGUP ? 1 : -1) * MAX(1, t->rows - 1));
		return 1;
	}
	if (shift && ctrl && (k == KEY_HOME || k == KEY_END)) {
		tw_view(t, k == KEY_HOME ? t->sbcount : -t->view);
		return 1;
	}
	if ((ctrl && shift && (k == 3 || (k | 0x20) == 'c')) || (ctrl && !shift && k == KEY_INS)) {
		tw_copy(t);
		tw_unselect(t);
		return 1;
	}
	if ((ctrl && shift && (k == 22 || (k | 0x20) == 'v')) || (shift && !ctrl && k == KEY_INS)) {
		tw_paste(t);
		return 1;
	}
	/* anything else is typing: back to the live screen */
	if (t->view)
		tw_view(t, -t->view);
	if (t->sel && !t->selecting)
		tw_unselect(t);
	return 0;
}

/* ---- dialogs ---------------------------------------------------------------------- */

void render_dialog(struct win *w);
static const struct winops dialog_ops;

struct win *open_dialog(struct wm *wm, const char *title, const char *msg, int input,
    int purpose)
{
	struct dialog *d = kzalloc(sizeof *d);
	if (!d)
		return NULL;
	const char *p = msg;
	while (*p && d->nlines < 6) {
		size_t n = 0;
		while (p[n] && p[n] != '\n' && n < 60)
			n++;
		memcpy(d->lines[d->nlines], p, n);
		d->lines[d->nlines++][n] = 0;
		p += n;
		if (*p == '\n')
			p++;
	}
	d->input = input;
	d->purpose = purpose;
	int cw = 420, ch = 40 + d->nlines * 18 + (input ? 30 : 0) + 30;
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	struct win *w = win_new(wm, W_DIALOG, title, (sw - FRAME_W(cw)) / 2, (sh - FRAME_H(ch)) / 3, cw, ch);
	if (!w) {
		kfree(d);
		return NULL;
	}
	w->dlg = d;
	w->ops = &dialog_ops;
	render_dialog(w);	/* draw it now, not on the first key */
	return w;
}

static void draw_button(struct surface *s, int x, int y, int w, const char *label, int def)
{
	fill(s, x, y, w, 24, P(theme.face));
	bevel(s, x, y, w, 24, 1);
	if (def)
		outline(s, x - 1, y - 1, w + 2, 26, P(0));
	text(s, x + (w - (int)strlen(label) * FONT_W) / 2, y + 4, label, P(0));
}

void render_dialog(struct win *w)
{
	struct dialog *d = w->dlg;
	struct surface *s = &w->content;
	fill(s, 0, 0, s->w, s->h, P(theme.face));
	for (int i = 0; i < d->nlines; i++)
		text(s, 16, 16 + i * 18, d->lines[i], P(0));
	int y = 16 + d->nlines * 18 + 8;
	if (d->input) {
		fill(s, 16, y, s->w - 32, 22, P(0xFFFFFF));
		bevel(s, 16, y, s->w - 32, 22, 0);
		text(s, 20, y + 3, d->field, P(0));
		fill(s, 20 + (int)strlen(d->field) * FONT_W, y + 3, 2, 16, P(0));
		y += 32;
	}
	int two = d->input || d->purpose == 2;	/* questions get a way out */
	draw_button(s, s->w / 2 - (two ? 90 : 40), s->h - 34, 80, "OK", 1);
	if (two)
		draw_button(s, s->w / 2 + 10, s->h - 34, 80, "Cancel", 0);
	win_dirty_all(w);
}

static void dialog_finish(struct wm *wm, struct win *w, int how)
{
	struct dialog *d = w->dlg;
	d->done = how;
	if (d->purpose == 1 && how == 1 && d->field[0])
		launch_cmd(wm, d->field, d->field);
	if (d->purpose == 2 && how == 1)
		wm->running = 0;	/* Exit desktop confirmed */
	if (d->purpose == 3) {	/* the waiting program frees it; just take it off screen */
		popup_close(wm);
	}
	if (d->purpose != 3)		/* 3: a program waits on it and frees it */
		win_destroy(wm, w);
	else {
		struct win **pp = &wm->wins;
		while (*pp && *pp != w)
			pp = &(*pp)->next;
		if (*pp)
			*pp = w->next;
		wm_dirty(wm, frame_rect(w));
		if (wm->focus == w)
			wm->focus = topmost(wm);
		w->closed = 1;
	}
}

static void dialog_key(struct wm *wm, struct win *w, int k)
{
	struct dialog *d = w->dlg;
	if (k == '\n') {
		dialog_finish(wm, w, 1);
		return;
	}
	if (k == 27) {
		dialog_finish(wm, w, 2);
		return;
	}
	if (!d->input)
		return;
	size_t l = strlen(d->field);
	if (k == '\b' && l)
		d->field[l - 1] = 0;
	else if (k == 22 || (k == KEY_INS && ((input_mods() | wm->rawmods) & 1))) {	/* paste */
		for (const char *p = clip_text_get(); *p && *p != '\r' && *p != '\n' && l < 48; p++)
			if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F)
				d->field[l++] = *p;
		d->field[l] = 0;
	} else if (k == 3)
		clip_text_set(d->field, l);
	else if (k >= 0x20 && k < 0x7F && l + 1 < sizeof d->field && l < 48) {
		d->field[l] = (char)k;
		d->field[l + 1] = 0;
	}
	render_dialog(w);
}

static void dialog_click(struct wm *wm, struct win *w, int x, int y)
{
	struct dialog *d = w->dlg;
	struct surface *s = &w->content;
	int by = s->h - 34;
	if (y < by || y > by + 24)
		return;
	int two = d->input || d->purpose == 2;
	int okx = s->w / 2 - (two ? 90 : 40);
	if (x >= okx && x < okx + 80)
		dialog_finish(wm, w, 1);
	else if (two && x >= s->w / 2 + 10 && x < s->w / 2 + 90)
		dialog_finish(wm, w, 2);
}


static void dialog_key_op(struct win *w, int k) { dialog_key(w->wm, w, k); }

static void dialog_mouse_op(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	(void)dbl;
	if (type == EV_DOWN && (buttons & 1))
		dialog_click(w->wm, w, x, y);
}

static void dialog_repaint(struct win *w) { if (!w->closed) render_dialog(w); }

static const struct winops dialog_ops = {
	.key = dialog_key_op,
	.mouse = dialog_mouse_op,
	.repaint = dialog_repaint,
	.icon = "book",
};

/* ---- desktop-wide actions and menus ----------------------------------------------- */

void about(struct wm *wm)
{
	char msg[200];
	char mem[32];
	fmt_commas(mem, sizeof mem, pmm_free_bytes() / 1024);
	snprintf(msg, sizeof msg, "%s %s - %s\n\nA 64-bit DOS with a Plan 9 heart.\nDesktop level %d. %s K free.",
	    BDOS_NAME, BDOS_VERSION, BDOS_LONGNAME, wm->depth, mem);
	open_dialog(wm, "About bdos", msg, 0, 0);
}

void tile(struct wm *wm, int cascade)
{
	int n = 0;
	for (struct win *w = wm->wins; w; w = w->next)
		if (w->kind != W_DIALOG && !w->minimized)
			n++;
	int i = 0, sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	int any_icons = 0;
	for (struct win *w = wm->wins; w; w = w->next)
		any_icons |= w->minimized;
	if (any_icons)
		sh -= ICON_SLOT_H;	/* leave the icon row clear */
	for (struct win *w = wm->wins; w; w = w->next) {
		if (w->kind == W_DIALOG || w->minimized)
			continue;
		wm_dirty(wm, frame_rect(w));
		if (cascade) {
			w->x = 16 + i * 26;
			w->y = 16 + i * 26;
		} else {
			int cols = n > 3 ? 3 : n > 1 ? 2 : 1, rws = (n + cols - 1) / cols;
			w->x = (i % cols) * (sw / cols);
			w->y = (i / cols) * (sh / rws);
			if (win_can_resize(w)) {
				w->maximized = 0;
				win_resize(w, w->x, w->y, sw / cols, sh / rws);
				i++;
				continue;
			}
		}
		w->x = MIN(w->x, sw - 60);
		w->y = MIN(w->y, sh - 40);
		wm_dirty(wm, frame_rect(w));
		i++;
	}
}

void next_window(struct wm *wm)
{
	struct win *first = NULL;
	for (struct win *w = wm->wins; w; w = w->next)
		if (!w->minimized) {
			first = w;
			break;
		}
	if (first && first != topmost(wm))
		raise_win(wm, first);
}

void exit_desktop(struct wm *wm)
{
	open_dialog(wm, "Exit Desktop",
	    wm->depth ? "Leave this nested desktop?" : "Leave the desktop and go back to DOS?", 0, 2);
}

enum {
	DM_SHELL = 1, DM_FILES, DM_PROGMAN, DM_CONTROL, DM_THEMES, DM_DESKTOP, DM_CASCADE, DM_TILE,
	DM_ABOUT, DM_EXIT, DM_CLOCK, DM_PULSE,
};

static void desktop_pick(struct wm *wm, void *arg, int id)
{
	(void)arg;
	extern int control_open(void *wm, const char *applet);
	extern void clock_open(struct wm *wm);
	switch (id) {
	case DM_SHELL: open_term(wm, "Shell", NULL, 80, 25, -1, -1); break;
	case DM_FILES: open_files(wm, "C:\\"); break;
	case DM_PROGMAN: launch_cmd(wm, "Program Manager", "@progman"); break;
	case DM_CONTROL: control_open(wm, NULL); break;
	case DM_THEMES: control_open(wm, "themes"); break;
	case DM_DESKTOP: control_open(wm, "desktop"); break;
	case DM_CASCADE: tile(wm, 1); break;
	case DM_TILE: tile(wm, 0); break;
	case DM_CLOCK: clock_open(wm); break;
	case DM_PULSE: launch_cmd(wm, "Pulse", "@pulse"); break;
	case DM_ABOUT: about(wm); break;
	case DM_EXIT: exit_desktop(wm); break;
	}
}

/* right click on the desktop itself */
void desktop_menu(struct wm *wm, int x, int y)
{
	static const struct menu_item items[] = {
		{ "&New Shell\tAlt+N", 0, DM_SHELL }, { "&File Manager", 0, DM_FILES },
		{ "&Program Manager", 0, DM_PROGMAN }, { "C&ontrol Panel", 0, DM_CONTROL },
		{ "C&lock", 0, DM_CLOCK }, { "P&ulse", 0, DM_PULSE },
		{ "", MI_SEP, 0 },
		{ "&Themes...", 0, DM_THEMES }, { "&Desktop...", 0, DM_DESKTOP },
		{ "", MI_SEP, 0 },
		{ "&Cascade", 0, DM_CASCADE }, { "T&ile", 0, DM_TILE },
		{ "", MI_SEP, 0 },
		{ "&About bdos", 0, DM_ABOUT }, { "E&xit Desktop\tAlt+X", 0, DM_EXIT },
	};
	popup_open(wm, x, y, items, (int)NELEM(items), desktop_pick, NULL, NULL);
}

enum { CM_RESTORE = 1, CM_MINIMIZE, CM_MAXIMIZE, CM_CLOSE, CM_NEXT, CM_MOVE, CM_SIZE };

/* Move and Size from the control menu: the arrow keys move an outline, Enter puts it there */
static void kb_track(struct wm *wm, struct win *w, int mode)
{
	wm->kbmove = w;
	wm->kbmode = mode;
	wm->ghost = frame_rect(w);
	wm->rs_orig = wm->ghost;
	wm_dirty(wm, wm->ghost);
}

static void control_pick(struct wm *wm, void *arg, int id)
{
	struct win *w = arg;
	int alive = 0;
	for (struct win *v = wm->wins; v; v = v->next)
		alive |= v == w;
	if (!alive)
		return;
	switch (id) {
	case CM_RESTORE:
		if (w->minimized)
			win_minimize(w, 0);
		else if (w->maximized)
			toggle_maximize(wm, w);
		break;
	case CM_MINIMIZE: win_minimize(w, 1); break;
	case CM_MAXIMIZE:
		if (w->minimized)
			win_minimize(w, 0);
		if (!w->maximized)
			toggle_maximize(wm, w);
		break;
	case CM_CLOSE: close_request(wm, w); break;
	case CM_NEXT: next_window(wm); break;
	case CM_MOVE: kb_track(wm, w, 1); break;
	case CM_SIZE: kb_track(wm, w, 2); break;
	}
}

/* the Windows 3.1 control menu: click the box at the left of a title bar */
void control_menu(struct wm *wm, struct win *w, int x, int y)
{
	struct menu_item items[12];
	int n = 0;
	items[n++] = (struct menu_item){ "&Restore", (w->maximized || w->minimized) ? 0 : MI_DISABLED, CM_RESTORE };
	items[n++] = (struct menu_item){ "&Move", (w->maximized || w->minimized) ? MI_DISABLED : 0, CM_MOVE };
	items[n++] = (struct menu_item){ "&Size", (win_can_resize(w) && !w->maximized && !w->minimized) ? 0 : MI_DISABLED, CM_SIZE };
	items[n++] = (struct menu_item){ "Mi&nimize", (w->minimizable && !w->minimized) ? 0 : MI_DISABLED, CM_MINIMIZE };
	items[n++] = (struct menu_item){ "Ma&ximize", (win_can_resize(w) && !w->maximized) ? 0 : MI_DISABLED, CM_MAXIMIZE };
	items[n++] = (struct menu_item){ "", MI_SEP, 0 };
	items[n++] = (struct menu_item){ "&Close\tAlt+Q", 0, CM_CLOSE };
	items[n++] = (struct menu_item){ "", MI_SEP, 0 };
	items[n++] = (struct menu_item){ "Nex&t Window\tAlt+W", 0, CM_NEXT };
	popup_open(wm, x, y, items, n, control_pick, w, w);
}

void close_request(struct wm *wm, struct win *w)
{
	if (w == wm->progman) {
		exit_desktop(wm);
		return;
	}
	if (w->kind == W_DIALOG) {
		dialog_finish(wm, w, 2);
		return;
	}
	win_destroy(wm, w);
}

/* ---- drag and drop -------------------------------------------------------------- */

void wm_dnd_start(struct wm *wm, struct win *src, const char *paths, const char *icon);

void wm_dnd_start(struct wm *wm, struct win *src, const char *paths, const char *icon)
{
	dnd_end(wm);
	wm->dnd.paths = kstrdup(paths);
	if (!wm->dnd.paths)
		return;
	wm->dnd.active = 1;
	wm->dnd.src = src;
	strncpy0(wm->dnd.icon, icon ? icon : "file", sizeof wm->dnd.icon);
	wm->dnd.count = 1;
	for (const char *p = paths; *p; p++)
		if (*p == '\n' && p[1])
			wm->dnd.count++;
	wm->grab = NULL;	/* the desktop carries it now */
	wm_dirty(wm, (struct rect){ wm->mx - 20, wm->my - 20, 48, 56 });
}

static void dnd_end(struct wm *wm)
{
	if (wm->dnd.active)
		wm_dirty(wm, (struct rect){ wm->mx - 20, wm->my - 20, 48, 56 });
	kfree(wm->dnd.paths);
	memset(&wm->dnd, 0, sizeof wm->dnd);
}

/* ---- resizing and maximizing ------------------------------------------------------------- */

static int win_min_cw(struct win *w)
{
	if (w->kind == W_TERM)
		return 20 * FONT_W;
	return w->min_cw ? w->min_cw : 180;
}

static int win_min_ch(struct win *w)
{
	if (w->kind == W_TERM)
		return 4 * FONT_H;
	return w->min_ch ? w->min_ch : 80;
}

int win_can_resize(struct win *w)
{
	return w->resizable;
}

/* Give a window a new frame. Terminals snap to whole character cells and
 * keep the text around the cursor; everything else redraws at the new size. */
void win_resize(struct win *w, int x, int y, int fw, int fh)
{
	struct wm *wm = w->wm;
	int cw = MAX(win_min_cw(w), fw - 2 * BORDER);
	int ch = MAX(win_min_ch(w), fh - 2 * BORDER - TITLE_H);
	struct termwin *t = w->kind == W_TERM ? w->term : NULL;
	int cols = 0, rows = 0;
	if (t) {
		cols = MAX(1, MIN(cw / FONT_W, 250));
		rows = MAX(1, MIN(ch / FONT_H, 120));
		if (!t->child) {	/* text snaps to whole cells; a game or desktop in it needn't */
			cw = cols * FONT_W;
			ch = rows * FONT_H;
		}
	}
	wm_dirty(wm, frame_rect(w));
	w->x = x;
	w->y = MAX(0, y);
	if (cw == w->content.w && ch == w->content.h) {
		wm_dirty(wm, frame_rect(w));
		return;
	}
	struct surface ns;
	if (surf_init(&ns, cw, ch))
		return;
	uint16_t *grid = NULL;
	if (t && !(grid = kmalloc(sizeof(uint16_t) * (size_t)(cols * rows)))) {
		surf_free(&ns);
		return;
	}
	fill(&ns, 0, 0, cw, ch, P(t ? 0 : 0xFFFFFF));
	if (w->kind == W_CANVAS)	/* a program's drawing: keep what fits */
		blit(&ns, 0, 0, &w->content, 0, 0, MIN(cw, w->content.w), MIN(ch, w->content.h));
	surf_free(&w->content);
	w->content = ns;
	gfx_track(&w->content, wm->disp->rec);
	w->w = FRAME_W(cw);
	w->h = FRAME_H(ch);
	wm_dirty(wm, frame_rect(w));
	if (t) {
		for (int i = 0; i < cols * rows; i++)
			grid[i] = 0x0720;
		int shift = MAX(0, t->cy - (rows - 1));
		if (!t->child)
			for (int r = 0; r < shift; r++)
				tw_push_history(t, t->grid + r * t->cols, t->cols);
		t->view = 0;
		t->sel = t->selecting = 0;
		for (int r = 0; r < rows && r + shift < t->rows; r++)
			for (int c = 0; c < cols && c < t->cols; c++)
				grid[r * cols + c] = t->grid[(r + shift) * t->cols + c];
		kfree(t->grid);
		t->grid = grid;
		t->cols = cols;
		t->rows = rows;
		t->cy -= shift;
		t->cx = MIN(t->cx, cols);
		tw_redraw(t);
		if (t->child) {		/* the display in it: tell it, and show it again */
			struct event e = { EV_RESIZE, cw, ch, 0, 0 };
			evq_push(&t->child_ev, &e);
			win_dirty_all(w);
		}
		return;
	}
	if (w->kind == W_CANVAS) {
		struct event e = { EV_RESIZE, cw, ch, 0, 0 };
		evq_push(&w->events, &e);
		win_dirty_all(w);
	} else if (w->ops && w->ops->resized) {
		w->ops->resized(w);
	}
}

void toggle_maximize(struct wm *wm, struct win *w)
{
	if (!win_can_resize(w))
		return;
	if (w->maximized) {
		w->maximized = 0;
		win_resize(w, w->restore.x, w->restore.y, w->restore.w, w->restore.h);
	} else {
		w->restore = frame_rect(w);
		w->maximized = 1;
		int any_icons = 0;
		for (struct win *v = wm->wins; v; v = v->next)
			any_icons |= v->minimized;
		win_resize(w, 0, 0, wm->disp->surf->w, wm->disp->surf->h - (any_icons ? ICON_SLOT_H : 0));
	}
}

/* which borders of w are under (x, y): 1 left 2 right 4 top 8 bottom */
static int edge_at(struct win *w, int x, int y)
{
	if (!w || !win_can_resize(w) || w->maximized)
		return 0;
	int lx = x - w->x, ly = y - w->y;
	if (lx < 0 || ly < 0 || lx >= w->w || ly >= w->h)
		return 0;
	int e = 0, corner = 24;
	if (lx < GRIP)
		e |= 1;
	else if (lx >= w->w - GRIP)
		e |= 2;
	if (ly < GRIP)
		e |= 4;
	else if (ly >= w->h - GRIP)
		e |= 8;
	if (e & 3) {
		if (ly < corner)
			e |= 4;
		else if (ly >= w->h - corner)
			e |= 8;
	}
	if (e & 12) {
		if (lx < corner)
			e |= 1;
		else if (lx >= w->w - corner)
			e |= 2;
	}
	return e;
}

static void resize_track(struct wm *wm, int x, int y)
{
	struct win *w = wm->resizing;
	struct rect o = wm->rs_orig, g = o;
	int dx = x - wm->rs_mx, dy = y - wm->rs_my;
	int minw = FRAME_W(win_min_cw(w)), minh = FRAME_H(win_min_ch(w));
	int e = wm->rs_edges;
	if (e & 1) {
		g.w = MAX(minw, o.w - dx);
		g.x = o.x + o.w - g.w;
	}
	if (e & 2)
		g.w = MAX(minw, o.w + dx);
	if (e & 4) {
		g.h = MAX(minh, o.h - dy);
		g.y = o.y + o.h - g.h;
		if (g.y < 0) {
			g.h += g.y;
			g.y = 0;
		}
	}
	if (e & 8)
		g.h = MAX(minh, o.h + dy);
	wm_dirty(wm, wm->ghost);
	wm->ghost = g;
	wm_dirty(wm, g);
}

/* ---- composing the screen ---------------------------------------------------------------- */

static const char *const pointer[] = {
	"k...........", "kk..........", "kwk.........", "kwwk........", "kwwwk.......",
	"kwwwwk......", "kwwwwwk.....", "kwwwwwwk....", "kwwwwwwwk...", "kwwwwwwwwk..",
	"kwwwwwwwwwk.", "kwwwwwkkkkkk", "kwwkwwk.....", "kwk.kwwk....", "kk..kwwk....",
	"k....kwwk...", ".....kwwk...", "......kk....",
};
#define PTR_W 12
#define PTR_H 18

/* resize pointers: a double-headed arrow through the hot spot */
static void draw_resize_pointer(struct surface *s, int cx, int cy, int edges)
{
	int dx, dy;
	if ((edges & 3) && !(edges & 12)) { dx = 1; dy = 0; }
	else if ((edges & 12) && !(edges & 3)) { dx = 0; dy = 1; }
	else if ((edges & 5) == 5 || (edges & 10) == 10) { dx = 1; dy = 1; }
	else { dx = 1; dy = -1; }
	int px[64], py[64], n = 0, L = 7;
	int qx = -dy, qy = dx;	/* perpendicular */
	for (int t = -L; t <= L; t++) {
		px[n] = cx + t * dx;
		py[n++] = cy + t * dy;
	}
	for (int end = -1; end <= 1; end += 2)
		for (int k = 1; k <= 3; k++)
			for (int side = -1; side <= 1; side += 2) {
				px[n] = cx + end * (L - k) * dx + side * k * qx;
				py[n++] = cy + end * (L - k) * dy + side * k * qy;
			}
	for (int i = 0; i < n; i++)
		fill(s, px[i] - 1, py[i] - 1, 3, 3, P(0xFFFFFF));
	for (int i = 0; i < n; i++)
		plot(s, px[i], py[i], P(0));
}

static void draw_frame(struct surface *s, struct win *w, int active)
{
	int x = w->x, y = w->y;
	fill(s, x, y, w->w, w->h, P(theme.face));
	bevel(s, x, y, w->w, w->h, 1);
	uint32_t tb = P(active ? theme.active : theme.inactive);
	fill(s, x + BORDER, y + BORDER, w->w - 2 * BORDER, TITLE_H - 2, tb);
	int tw = (int)strlen(w->title) * FONT_W;
	int maxw = w->w - 2 * BORDER - 34 - (w->resizable ? 18 : 0) - (w->minimizable ? 18 : 0);
	char t[64];
	strncpy0(t, w->title, sizeof t);
	if (tw > maxw && maxw > 0) {
		t[maxw / FONT_W] = 0;
		tw = (int)strlen(t) * FONT_W;
	}
	text(s, x + (w->w - tw) / 2, y + BORDER + 1, t, P(theme.title_text));
	/* close box */
	int bx = x + w->w - BORDER - 18, by = y + BORDER + 1;
	fill(s, bx, by, 16, 16, P(theme.face));
	bevel(s, bx, by, 16, 16, 1);
	line(s, bx + 4, by + 4, bx + 11, by + 11, P(0));
	line(s, bx + 11, by + 4, bx + 4, by + 11, P(0));
	int cx_, mx_, nx_;
	title_buttons(w, &cx_, &mx_, &nx_);
	if (nx_ >= 0) {		/* minimize: a down arrow */
		int nx = x + nx_;
		fill(s, nx, by, 16, 16, P(theme.face));
		bevel(s, nx, by, 16, 16, 1);
		arrow_glyph(s, nx + 8, by + 8, 1, P(0));
	}
	if (win_can_resize(w)) {
		/* maximize/restore box, and the notches that mark the resize corners */
		int mx = x + mx_;
		fill(s, mx, by, 16, 16, P(theme.face));
		bevel(s, mx, by, 16, 16, 1);
		if (w->maximized) {
			arrow_glyph(s, mx + 8, by + 4, 0, P(0));
			arrow_glyph(s, mx + 8, by + 11, 1, P(0));
		} else {
			arrow_glyph(s, mx + 8, by + 7, 0, P(0));
		}
		if (!w->maximized) {
			uint32_t d = P(0x707070);
			vline(s, x + 18, y, BORDER, d);
			vline(s, x + w->w - 19, y, BORDER, d);
			vline(s, x + 18, y + w->h - BORDER, BORDER, d);
			vline(s, x + w->w - 19, y + w->h - BORDER, BORDER, d);
			hline(s, x, y + 18, BORDER, d);
			hline(s, x + w->w - BORDER, y + 18, BORDER, d);
			hline(s, x, y + w->h - 19, BORDER, d);
			hline(s, x + w->w - BORDER, y + w->h - 19, BORDER, d);
		}
	}
	/* control-menu box on the left, Windows 3.1 style */
	int cx = x + BORDER + 2;
	fill(s, cx, by, 16, 16, P(theme.face));
	bevel(s, cx, by, 16, 16, 1);
	fill(s, cx + 3, by + 7, 10, 2, P(0));
}

/* the desktop: its colour, and the theme's pattern over it */
static void draw_desktop(struct wm *wm, struct surface *s, struct rect r)
{
	fill(s, r.x, r.y, r.w, r.h, P(wm->desktop));
	if (theme.has_pattern && !wm->depth)
		fill_pattern(s, r, theme.pattern, P(theme.pattern_ink));
	/* a quiet watermark */
	const char *mark = wm->depth ? "bdos (nested)" : "bdos";
	text(s, s->w - (int)strlen(mark) * FONT_W - 12, s->h - 24, mark, P(0xB0C8C8));
}

static void draw_min_icon(struct wm *wm, struct surface *s, struct win *w)
{
	struct rect r = icon_rect(wm, w);
	const char *icon = w->ops && w->ops->icon ? w->ops->icon :
	    w->kind == W_TERM ? "prompt" : w->kind == W_CANVAS ? "gear" : w->kind == W_PROGMAN ? "group" : "file";
	if (w->ops && w->ops->draw_icon)
		w->ops->draw_icon(w, s, r.x + (r.w - 32) / 2, r.y + 4);
	else
		draw_icon(s, r.x + (r.w - 32) / 2, r.y + 4, icon);
	/* "File Manager - C:\DEMOS" is just "File Manager" down here */
	char label[16];
	int max = r.w / FONT_W - 1;
	strncpy0(label, w->title, sizeof label);
	char *dash = strstr(label, " - ");
	if (dash)
		*dash = 0;
	if ((int)strlen(label) > max && max > 3)
		strcpy(label + max - 3, "...");
	int lw = (int)strlen(label) * FONT_W;
	int lx = r.x + (r.w - lw) / 2, ly = r.y + 40;
	fill(s, lx - 2, ly, lw + 4, FONT_H, P(w == wm->focus ? theme.select : 0xFFFFFF));
	text(s, lx, ly, label, P(w == wm->focus ? 0xFFFFFF : 0));
}

static void compose(struct wm *wm, struct rect r)
{
	struct surface *s = wm->disp->surf;
	r = rect_isect(r, (struct rect){ 0, 0, s->w, s->h });
	if (rect_empty(r))
		return;
	surf_clip(s, r);
	draw_desktop(wm, s, r);
	for (struct win *w = wm->wins; w; w = w->next)
		if (w->minimized && !rect_empty(rect_isect(icon_rect(wm, w), r)))
			draw_min_icon(wm, s, w);
	for (struct win *w = wm->wins; w; w = w->next) {
		struct rect fr = frame_rect(w);
		if (w->minimized || rect_empty(rect_isect(fr, r)))
			continue;
		draw_frame(s, w, w == wm->focus);
		blit(s, w->x + CLIENT_X, w->y + CLIENT_Y, &w->content, 0, 0, w->content.w, w->content.h);
	}
	if (wm->popup)
		popup_draw(wm, s);
	if (wm->dnd.active) {	/* what's being dragged follows the pointer */
		draw_icon(s, wm->mx - 16, wm->my - 16, wm->dnd.icon);
		if (wm->dnd.count > 1) {
			char n[8];
			snprintf(n, sizeof n, "%d", wm->dnd.count);
			fill(s, wm->mx + 10, wm->my + 10, (int)strlen(n) * FONT_W + 4, FONT_H, P(theme.select));
			text(s, wm->mx + 12, wm->my + 10, n, P(0xFFFFFF));
		}
	}
	if (wm->resizing || wm->kbmove) {
		struct rect g = wm->ghost;
		outline(s, g.x, g.y, g.w, g.h, P(0));
		outline(s, g.x + 1, g.y + 1, g.w - 2, g.h - 2, P(0xFFFFFF));
		outline(s, g.x + 2, g.y + 2, g.w - 4, g.h - 4, P(0));
	}
	if (wm->disp->draws_pointer) {
		if (wm->ptr)
			draw_resize_pointer(s, wm->mx, wm->my, wm->ptr);
		else
			for (int j = 0; j < PTR_H; j++)
				for (int i = 0; i < PTR_W; i++) {
					char c = pointer[j][i];
					if (c != '.')
						plot(s, wm->mx + i, wm->my + j, P(c == 'k' ? 0 : 0xFFFFFF));
				}
	}
	surf_noclip(s);
	wm->disp->flush(wm->disp, r);
}

/* ---- input routing ------------------------------------------------------------------- */

static struct win *win_at(struct wm *wm, int x, int y)
{
	struct win *hit = NULL;
	for (struct win *w = wm->wins; w; w = w->next)
		if (!w->minimized && rect_has(frame_rect(w), x, y))
			hit = w;
	return hit;
}

static void send_to_window(struct win *w, struct event *e)
{
	if (w->kind == W_TERM && w->term) {
		struct termwin *t = w->term;
		if (t->child) {
			evq_push(&t->child_ev, e);
		} else if (e->type == EV_KEY) {
			tw_key(t, e->key);
		}
	} else if (w->kind == W_CANVAS) {
		evq_push(&w->events, e);
	}
}

/* windows that want raw events: programs' canvases, nested desktops */
static int raw_window(struct win *w)
{
	return w && (w->kind == W_CANVAS || (w->term && w->term->child));
}

static void kb_track_key(struct wm *wm, int k)
{
	struct win *w = wm->kbmove;
	int alive = 0;
	for (struct win *v = wm->wins; v; v = v->next)
		alive |= v == w;
	struct rect g = wm->ghost;
	int step = 8;
	if (!alive || k == 27) {
		wm->kbmove = NULL;
		wm_dirty(wm, g);
		return;
	}
	if (k == '\n') {
		wm->kbmove = NULL;
		wm_dirty(wm, g);
		win_resize(w, g.x, g.y, g.w, g.h);
		return;
	}
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	if (wm->kbmode == 1) {
		if (k == KEY_LEFT) g.x -= step;
		if (k == KEY_RIGHT) g.x += step;
		if (k == KEY_UP) g.y -= step;
		if (k == KEY_DOWN) g.y += step;
		g.x = MAX(-g.w + 40, MIN(g.x, sw - 40));
		g.y = MAX(0, MIN(g.y, sh - 20));
	} else {
		if (k == KEY_LEFT) g.w -= step;
		if (k == KEY_RIGHT) g.w += step;
		if (k == KEY_UP) g.h -= step;
		if (k == KEY_DOWN) g.h += step;
		g.w = MAX(g.w, 120);
		g.h = MAX(g.h, 60);
	}
	wm_dirty(wm, wm->ghost);
	wm->ghost = g;
	wm_dirty(wm, g);
}

static void handle_key(struct wm *wm, int k)
{
	if (wm->kbmove) {
		kb_track_key(wm, k);
		return;
	}
	if (wm->popup) {
		popup_key(wm, k);
		return;
	}
	if (wm->dnd.active && k == 27) {
		dnd_end(wm);
		return;
	}
	/* Alt+letter: the focused window's menu bar first, as in Windows */
	struct win *f = wm->focus;
	if (f && !f->minimized && f->menubar && f->ops && f->ops->mouse && (k & KEY_ALT) &&
	    isalpha(k & 0xFF)) {
		struct menubar *mb = f->menubar;
		for (int i = 0, x = 6; i < mb->n; i++) {
			int wd = (int)strlen(mb->titles[i]) * FONT_W + 12;
			if (toupper(mb->titles[i][0]) == toupper(k & 0xFF)) {
				f->ops->mouse(f, EV_DOWN, x + 3, 3, 1, 0);
				popup_select_first(wm);
				return;
			}
			x += wd;
		}
	}
	/* desktop-wide keys */
	if (k == 27 && ((input_mods() | wm->rawmods) & 2)) {	/* Ctrl+Esc: the Task Manager */
		taskman_open(wm, NULL);
		return;
	}
	if (k == (KEY_ALT | 'n')) {
		open_term(wm, "Shell", NULL, 80, 25, -1, -1);
		return;
	}
	if (k == (KEY_ALT | 'w')) {
		next_window(wm);
		return;
	}
	if (k == (KEY_ALT | 'x')) {
		exit_desktop(wm);
		return;
	}
	struct win *w = wm->focus;
	if (!w)
		return;
	if (k == (KEY_ALT | 'q')) {
		close_request(wm, w);
		return;
	}
	if (k == (KEY_ALT | ' ')) {	/* Alt+Space: the control menu, as in Windows */
		control_menu(wm, w, w->minimized ? icon_rect(wm, w).x : w->x + BORDER,
		    w->minimized ? icon_rect(wm, w).y - 150 : w->y + CLIENT_Y);
		return;
	}
	if (w->minimized) {
		if (k == '\n')
			win_minimize(w, 0);
		return;
	}
	if (w->kind == W_TERM && w->term && !w->term->child &&
	    term_key(w->term, k, input_mods() | wm->rawmods))
		return;
	if (w->ops && w->ops->key) {
		w->ops->key(w, k);
		return;
	}
	struct event e = { EV_KEY, 0, 0, 0, k };
	send_to_window(w, &e);
}

static struct rect ptr_rect(struct wm *wm)
{
	return (struct rect){ wm->mx - 10, wm->my - 10, PTR_W + 12, PTR_H + 12 };
}

static void update_pointer_shape(struct wm *wm, struct win *w)
{
	int shape = wm->resizing ? wm->rs_edges : (wm->drag || wm->grab || wm->dnd.active) ? 0 :
	    edge_at(w, wm->mx, wm->my);
	if (shape == wm->ptr)
		return;
	wm->ptr = shape;
	if (wm->disp->draws_pointer)
		wm_dirty(wm, ptr_rect(wm));
	else if (wm->disp->pointer)
		wm->disp->pointer(wm->disp, shape);	/* bterm shows its own */
}

static void deliver(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	if (w->ops && w->ops->mouse)
		w->ops->mouse(w, type, x - w->x - CLIENT_X, y - w->y - CLIENT_Y, buttons, dbl);
}

static void finish_drop(struct wm *wm, int x, int y)
{
	struct win *t = win_at(wm, x, y);
	char *paths = wm->dnd.paths;
	wm->dnd.paths = NULL;
	int copy = (wm->mods & MOD_CTRL) != 0;
	dnd_end(wm);
	if (!paths)
		return;
	if (t && t->ops && t->ops->drop) {
		t->ops->drop(t, x - t->x - CLIENT_X, y - t->y - CLIENT_Y, paths, copy);
	} else if (t && t->term && !t->term->child) {
		/* a shell: type the names in, quoted */
		raise_win(wm, t);
		for (char *p = paths; *p; ) {
			char *e = strchr(p, '\n');
			if (e)
				*e = 0;
			if (*p) {
				term_type(t, "\"");
				term_type(t, p);
				term_type(t, "\" ");
			}
			if (!e)
				break;
			p = e + 1;
		}
	}
	kfree(paths);
}

static void handle_mouse(struct wm *wm, struct event *e)
{
	int x = e->x, y = e->y;
	if (wm->disp->draws_pointer && (x != wm->mx || y != wm->my)) {
		wm_dirty(wm, ptr_rect(wm));
		if (wm->dnd.active)
			wm_dirty(wm, (struct rect){ wm->mx - 20, wm->my - 20, 48, 56 });
		wm->mx = x;
		wm->my = y;
		wm_dirty(wm, ptr_rect(wm));
		if (wm->dnd.active)
			wm_dirty(wm, (struct rect){ x - 20, y - 20, 48, 56 });
	} else if (wm->dnd.active && (x != wm->mx || y != wm->my)) {
		wm_dirty(wm, (struct rect){ wm->mx - 20, wm->my - 20, 48, 56 });
		wm_dirty(wm, (struct rect){ x - 20, y - 20, 48, 56 });
	}
	wm->mx = x;
	wm->my = y;
	int buttons = e->buttons & 7;
	wm->mods = e->buttons & (MOD_SHIFT | MOD_CTRL | MOD_ALT);
	int pressed = buttons & ~wm->buttons, released = wm->buttons & ~buttons;
	wm->buttons = buttons;
	int etype = pressed ? EV_DOWN : released ? EV_UP : EV_MOVE;

	/* an open menu takes everything */
	if (wm->popup && popup_mouse(wm, etype, x, y))
		return;

	if (wm->dnd.active) {
		if (released & 1)
			finish_drop(wm, x, y);
		return;
	}
	if (wm->resizing) {
		if (released & 1) {
			struct win *w = wm->resizing;
			struct rect g = wm->ghost;
			wm->resizing = NULL;
			wm_dirty(wm, g);
			w->maximized = 0;
			win_resize(w, g.x, g.y, g.w, g.h);
			update_pointer_shape(wm, win_at(wm, x, y));
		} else {
			resize_track(wm, x, y);
		}
		return;
	}
	if (wm->drag) {
		if (released & 1) {
			wm->drag = NULL;
			return;
		}
		struct win *w = wm->drag;
		wm_dirty(wm, frame_rect(w));
		w->x = x - wm->drag_dx;
		w->y = MAX(0, y - wm->drag_dy);
		w->maximized = 0;
		wm_dirty(wm, frame_rect(w));
		return;
	}
	if (wm->grab) {		/* the window the press went to gets the rest */
		struct win *g = wm->grab;
		if (raw_window(g)) {
			struct event le = *e;
			le.type = etype;
			le.x = x - g->x - CLIENT_X;
			le.y = y - g->y - CLIENT_Y;
			send_to_window(g, &le);
		} else {
			deliver(g, etype, x, y, buttons | wm->mods, 0);
		}
		if (!buttons && wm->grab == g)
			wm->grab = NULL;
		return;
	}

	struct win *w = win_at(wm, x, y);
	update_pointer_shape(wm, w);
	if (pressed) {
		int button = pressed & 1 ? 1 : pressed & 2 ? 2 : 4;
		uint64_t now = uptime_ms();
		int dbl = button == wm->click_button && now - wm->click_ms < (uint64_t)DCLICK_MS &&
		    x - wm->click_x < 5 && wm->click_x - x < 5 && y - wm->click_y < 5 && wm->click_y - y < 5;
		wm->click_ms = dbl ? 0 : now;
		wm->click_x = x;
		wm->click_y = y;
		wm->click_button = button;

		struct win *ic = w ? NULL : icon_at(wm, x, y);
		if (ic) {	/* a minimized window's icon */
			raise_win(wm, ic);
			if (button == 2)
				control_menu(wm, ic, x, y - 150);
			else if (dbl)
				win_minimize(ic, 0);
			return;
		}
		if (!w) {	/* the desktop */
			if (button == 2)
				desktop_menu(wm, x, y);
			return;
		}
		if (w != wm->focus || w != topmost(wm))
			raise_win(wm, w);
		int lx = x - w->x, ly = y - w->y;
		int on_bar = ly >= BORDER && ly < CLIENT_Y;
		if (on_bar && button == 2) {
			control_menu(wm, w, x, y);
			return;
		}
		if (button == 1) {
			int cx_, mx_, nx_;
			title_buttons(w, &cx_, &mx_, &nx_);
			if (on_bar && lx >= cx_ && lx < cx_ + 16) {
				close_request(wm, w);
				return;
			}
			if (on_bar && mx_ >= 0 && lx >= mx_ && lx < mx_ + 16) {
				toggle_maximize(wm, w);
				return;
			}
			if (on_bar && nx_ >= 0 && lx >= nx_ && lx < nx_ + 16) {
				win_minimize(w, 1);
				return;
			}
			int edges = edge_at(w, x, y);
			if (edges) {
				wm->resizing = w;
				wm->rs_edges = edges;
				wm->rs_orig = frame_rect(w);
				wm->ghost = wm->rs_orig;
				wm->rs_mx = x;
				wm->rs_my = y;
				wm->ptr = edges;
				wm_dirty(wm, wm->ghost);
				return;
			}
			if (ly < CLIENT_Y) {
				if (lx >= BORDER + 2 && lx < BORDER + 18) {
					if (dbl)
						close_request(wm, w);	/* double-click the control box */
					else
						control_menu(wm, w, w->x + BORDER, w->y + CLIENT_Y);
				} else if (dbl && win_can_resize(w)) {
					toggle_maximize(wm, w);	/* double-click the title */
				} else {
					wm->drag = w;
					wm->drag_dx = x - w->x;
					wm->drag_dy = y - w->y;
				}
				return;
			}
		}
		if (ly < CLIENT_Y || lx < CLIENT_X || lx >= w->w - BORDER || ly >= w->h - BORDER)
			return;
		if (button == 2 && !raw_window(w)) {	/* right click inside a window */
			if (w->ops && w->ops->context)
				w->ops->context(w, lx - CLIENT_X, ly - CLIENT_Y);
			else
				control_menu(wm, w, x, y);
			return;
		}
		wm->grab = w;
		if (raw_window(w)) {
			struct event le = *e;
			le.type = EV_DOWN;
			le.x = lx - CLIENT_X;
			le.y = ly - CLIENT_Y;
			send_to_window(w, &le);
		} else {
			deliver(w, EV_DOWN, x, y, buttons | wm->mods, dbl);
		}
		return;
	}
	/* plain motion over canvases and nested desktops */
	if (raw_window(w)) {
		struct event le = *e;
		le.type = EV_MOVE;
		le.x = x - w->x - CLIENT_X;
		le.y = y - w->y - CLIENT_Y;
		if (le.x >= 0 && le.y >= 0 && le.x < w->content.w && le.y < w->content.h)
			send_to_window(w, &le);
	}
}

/* ---- the main loop -------------------------------------------------------------------- */

static void check_terms(struct wm *wm)
{
	for (struct win *w = wm->wins; w; ) {
		struct win *next = w->next;
		if (w->kind == W_TERM && w->term && w->term->task && !task_alive(w->term->task))
			win_destroy(wm, w);	/* its shell or program ended */
		else if (w->kind == W_DIALOG && w->dlg && w->dlg->done && w->dlg->purpose != 3)
			win_destroy(wm, w);
		w = next;
	}
	reap_zombies();
}

/* the display changed size (a window a nested desktop is in, bterm's window):
 * keep everything on it, maximized windows filling it */
static void wm_display_resized(struct wm *wm)
{
	struct surface *s = wm->disp->surf;
	for (struct win *w = wm->wins; w; w = w->next) {
		if (w->maximized) {
			win_resize(w, 0, 0, s->w, s->h);
			continue;
		}
		int fw = MIN(w->w, s->w), fh = MIN(w->h, s->h);
		int x = MAX(0, MIN(w->x, s->w - fw)), y = MAX(0, MIN(w->y, s->h - fh));
		if (fw != w->w || fh != w->h)
			win_resize(w, x, y, fw, fh);
		else {
			w->x = x;
			w->y = y;
		}
	}
	wm->mx = MIN(wm->mx, s->w - 1);
	wm->my = MIN(wm->my, s->h - 1);
	wm->dirty = (struct rect){ 0, 0, 0, 0 };
	wm_dirty(wm, (struct rect){ 0, 0, s->w, s->h });
}

/* Shift, Ctrl and Alt from the make/break codes, for keys that come without
 * them (a bterm's keyboard isn't this machine's) */
static void track_rawmods(struct wm *wm, int sc)
{
	if (sc == 0xE0) {
		wm->raw_e0 = 1;
		return;
	}
	int e0 = wm->raw_e0, bit = 0;
	wm->raw_e0 = 0;
	switch (sc & 0x7F) {
	case 0x2A: case 0x36: bit = e0 ? 0 : 1; break;	/* E0 2A: the fake shift */
	case 0x1D: bit = 2; break;
	case 0x38: bit = 4; break;
	}
	if (!bit)
		return;
	if (sc & 0x80)
		wm->rawmods &= ~bit;
	else
		wm->rawmods |= bit;
}

/* the wheel: to the window under the pointer, as Windows 10 does */
static void handle_wheel(struct wm *wm, struct event *e)
{
	if (wm->popup || wm->dnd.active || wm->resizing || wm->drag)
		return;
	struct win *w = win_at(wm, e->x, e->y);
	if (!w)
		return;
	int lx = e->x - w->x - CLIENT_X, ly = e->y - w->y - CLIENT_Y;
	if (raw_window(w)) {
		struct event le = *e;
		le.x = lx;
		le.y = ly;
		send_to_window(w, &le);
	} else if (w->ops && w->ops->wheel) {
		w->ops->wheel(w, lx, ly, e->key);
	}
}

/* what other tasks and desktops asked of this one */
static void wm_requests(struct wm *wm)
{
	if (__atomic_exchange_n(&wm->want_repaint, 0, __ATOMIC_ACQ_REL)) {
		for (struct win *w = wm->wins; w; w = w->next)
			if (!w->closed && w->ops && w->ops->repaint)
				w->ops->repaint(w);
		wm_dirty_all(wm);
	}
	if (__atomic_exchange_n(&wm->want_progman, 0, __ATOMIC_ACQ_REL)) {
		extern void progman_reload(struct wm *wm);
		progman_reload(wm);
	}
	if (__atomic_exchange_n(&wm->want_files, 0, __ATOMIC_ACQ_REL)) {
		extern void files_refresh(struct wm *wm);
		files_refresh(wm);
	}
	if (__atomic_exchange_n(&wm->want_dirty, 0, __ATOMIC_ACQ_REL))
		wm_dirty_all(wm);
}

/*
 * Drawing what changed. The desktop's lock is held, the big kernel lock
 * isn't: desktops draw on whichever cores are free, side by side, and a
 * slow one doesn't hold the others up. Whatever the display needs the
 * kernel for (sending over the network) waits for its commit.
 */
static void wm_render(struct wm *wm, struct rect r)
{
	int depth = bkl_release_all();
	display_lock(wm->disp);
	compose(wm, r);
	display_unlock(wm->disp);
	bkl_reacquire(depth);
	if (wm->disp->commit)
		wm->disp->commit(wm->disp);
}

static void wm_loop(struct wm *wm)
{
	struct surface *s = wm->disp->surf;
	wm_dirty(wm, (struct rect){ 0, 0, s->w, s->h });
	while (wm->running && wm->disp->alive) {
		struct event e;
		int busy = 0;
		desk_lock(wm);
		wm_requests(wm);
		while (wm->disp->next_event(wm->disp, &e)) {
			busy = 1;
			if (e.type == EV_KEY)
				handle_key(wm, e.key);
			else if (e.type == EV_RAWKEY) {
				track_rawmods(wm, e.key);
				/* make/break codes: only for a window hosting a display (a DOS
				 * game, a nested desktop) */
				struct win *f = wm->focus;
				if (f && !wm->popup && f->kind == W_TERM && f->term && f->term->child)
					evq_push(&f->term->child_ev, &e);
			} else if (e.type == EV_CLOSE)
				wm->running = 0;
			else if (e.type == EV_RESIZE)
				wm_display_resized(wm);
			else if (e.type == EV_WHEEL)
				handle_wheel(wm, &e);
			else
				handle_mouse(wm, &e);
		}
		check_terms(wm);
		form_tick_all(wm);
		if (wm->follow_time && uptime_ms() >= wm->next_time_check) {
			/* the desktop follows the sun */
			wm->next_time_check = uptime_ms() + 20000;
			int yy, mo, d, h, mi, s;
			arch_rtc(&yy, &mo, &d, &h, &mi, &s);
			uint32_t c = time_of_day_colour(h * 60 + mi);
			if (c != wm->desktop) {
				wm->desktop = c;
				wm_dirty_all(wm);
			}
		}
		if (!wm->progman && wm->running)
			wm->running = 0;	/* Program Manager closed: leave */
		if (!rect_empty(wm->dirty)) {
			struct rect r = wm->dirty;
			wm->dirty = (struct rect){ 0, 0, 0, 0 };
			wm_render(wm, r);
			busy = 1;
		}
		desk_unlock(wm);
		if (!busy)
			task_idle();
		else
			task_yield();
	}
}

/* Close everything; wait (briefly) for shells to notice. */
static void wm_shutdown(struct wm *wm)
{
	desk_lock(wm);
	while (wm->wins)
		win_destroy(wm, wm->wins);
	desk_unlock(wm);
	uint64_t until = uptime_ms() + 2000;
	while (zombies && uptime_ms() < until) {
		reap_zombies();
		task_idle();
	}
}

int desktop_run_on(struct display *d, const char *title)
{
	struct wm *wm = kzalloc(sizeof *wm);
	if (!wm)
		return 1;
	wm->disp = d;
	wm->running = 1;
	wm->mx = d->surf->w / 2;
	wm->my = d->surf->h / 2;
	strncpy0(wm->title, title, sizeof wm->title);
	struct task *me = task_current();
	struct wm *outer = me->desk;
	wm->depth = outer ? outer->depth + 1 : 0;
	static const uint32_t level_colours[] = { 0x2F6F73, 0x5B3F72, 0x404850, 0x6E4A2F };
	wm->desktop = wm->depth ? level_colours[wm->depth % 4] : setting_rgb("desktop.color");
	if (!wm->depth && setting_int("desktop.timeofday")) {
		wm->follow_time = 1;
		wm->next_time_check = 0;
	}
	wm->next_wm = all_wms;
	all_wms = wm;
	void *save_desk = me->desk;
	me->desk = wm;

	if (d->attach)
		d->attach(d, wm);
	open_progman(wm);
	if (!wm->depth) {
		extern void progman_startup(struct wm *wm);
		progman_startup(wm);
	}
	wm_loop(wm);
	wm_shutdown(wm);

	me->desk = save_desk;
	for (struct wm **pp = &all_wms; *pp; pp = &(*pp)->next_wm)
		if (*pp == wm) {
			*pp = wm->next_wm;
			break;
		}
	kfree(wm);
	return 0;
}

/* ---- displays: the screen, and a terminal window ----------------------------------------- */

struct screen_disp {
	struct display d;
	struct surface back;
	int mx, my, buttons;
};

static void scr_flush(struct display *d, struct rect r)
{
	struct screen_disp *sd = (struct screen_disp *)d;
	const struct bdos_framebuffer *fb = &bootinfo->fb;
	volatile uint32_t *dst = (volatile uint32_t *)fb->base;
	for (int y = r.y; y < r.y + r.h; y++) {
		uint32_t *src = sd->back.px + (size_t)y * (size_t)sd->back.pitch + r.x;
		volatile uint32_t *row = dst + (size_t)y * fb->pitch + r.x;
		for (int x = 0; x < r.w; x++)
			row[x] = src[x];
	}
}

static int scr_event(struct display *d, struct event *e)
{
	struct screen_disp *sd = (struct screen_disp *)d;
	int raw = input_getraw();
	if (raw >= 0) {
		*e = (struct event){ EV_RAWKEY, sd->mx, sd->my, sd->buttons, raw };
		return 1;
	}
	int k = input_getkey();
	if (k < 0)
		k = serial_console()->getkey(serial_console());
	if (k >= 0) {
		*e = (struct event){ EV_KEY, sd->mx, sd->my, sd->buttons, k };
		return 1;
	}
	struct mouse_event m;
	if (input_getmouse(&m)) {
		if (m.wheel) {
			*e = (struct event){ EV_WHEEL, sd->mx, sd->my, sd->buttons | (input_mods() << 8), m.wheel };
			return 1;
		}
		if (m.abs) {	/* vmmouse: the pointer sits exactly under the host's */
			sd->mx = MIN((int)((int64_t)m.ax * sd->back.w / 65535), sd->back.w - 1);
			sd->my = MIN((int)((int64_t)m.ay * sd->back.h / 65535), sd->back.h - 1);
		} else {
			sd->mx = MAX(0, MIN(sd->mx + m.dx, sd->back.w - 1));
			sd->my = MAX(0, MIN(sd->my + m.dy, sd->back.h - 1));
		}
		int type = EV_MOVE;
		if ((m.buttons & 1) && !(sd->buttons & 1))
			type = EV_DOWN;
		else if (!(m.buttons & 1) && (sd->buttons & 1))
			type = EV_UP;
		sd->buttons = m.buttons;
		int mods = input_mods();
		*e = (struct event){ type, sd->mx, sd->my, m.buttons | (mods << 8), 0 };
		return 1;
	}
	return 0;
}

struct term_disp {
	struct display d;
	struct termwin *t;
	int orig_w, orig_h;	/* the window's size before a program asked for another */
};

static void td_flush(struct display *d, struct rect r)
{
	struct term_disp *td = (struct term_disp *)d;
	if (td->t->win && !td->t->closed)
		win_dirty(td->t->win, r);
}

/* the pixels are a window's on the host desktop: its lock */
static void td_lock(struct display *d)
{
	struct term_disp *td = (struct term_disp *)d;
	if (td->t->wm)
		desk_lock(td->t->wm);
}

static void td_unlock(struct display *d)
{
	struct term_disp *td = (struct term_disp *)d;
	if (td->t->wm)
		desk_unlock(td->t->wm);
}

static void td_attach(struct display *d, struct wm *wm)
{
	(void)wm;
	struct term_disp *td = (struct term_disp *)d;
	td->t->child = 1;	/* text drawing stops; events go to the display */
}

static int td_event(struct display *d, struct event *e)
{
	struct term_disp *td = (struct term_disp *)d;
	if (td->t->closed) {
		d->alive = 0;
		return 0;
	}
	return evq_pop(&td->t->child_ev, e);
}

/*
 * A local display for whatever console a task is on: the whole screen, or
 * the inside of its desktop window. Used by WIN and by the remote viewer.
 */
struct display *display_open(struct console *c, int *err)
{
	extern struct console *console_unwrap(struct console *c);
	c = console_unwrap(c);
	if (c == screen_console()) {
		struct screen_disp *sd = kzalloc(sizeof *sd);
		const struct bdos_framebuffer *fb = &bootinfo->fb;
		if (!sd || surf_init(&sd->back, (int)fb->width, (int)fb->height)) {
			kfree(sd);
			*err = E_NOMEM;
			return NULL;
		}
		sd->d.surf = &sd->back;
		sd->d.flush = scr_flush;
		sd->d.next_event = scr_event;
		sd->d.draws_pointer = 1;
		sd->d.alive = 1;
		sd->d.name = "screen";
		sd->mx = sd->back.w / 2;
		sd->my = sd->back.h / 2;
		while (input_getkey() >= 0)
			;
		screen_suspend(1);
		return &sd->d;
	}
	if (console_is_termwin(c)) {
		struct termwin *t = c->priv;
		if (t->child || !t->win) {
			*err = E_ACCES;
			return NULL;
		}
		struct term_disp *td = kzalloc(sizeof *td);
		if (!td) {
			*err = E_NOMEM;
			return NULL;
		}
		td->t = t;
		td->d.surf = &t->win->content;
		td->d.flush = td_flush;
		td->d.next_event = td_event;
		td->d.alive = 1;
		td->d.name = "window";
		td->d.attach = td_attach;
		td->d.lock = td_lock;
		td->d.unlock = td_unlock;
		memset(&t->child_ev, 0, sizeof t->child_ev);
		return &td->d;
	}
	*err = E_NOTSUP;
	return NULL;
}

/* A program wants w x h pixels (0, 0: back to how it was). Windows grow or
 * shrink to fit, within the screen; the full screen is what it is. */
int display_want_size(struct display *d, int w, int h)
{
	if (!strcmp(d->name, "screen"))
		return 0;
	struct term_disp *td = (struct term_disp *)d;
	struct termwin *t = td->t;
	struct win *win = t->win;
	if (!win || t->closed || !win->wm)
		return -1;
	struct wm *host = win->wm;
	desk_lock(host);
	if (t->closed || !t->win) {
		desk_unlock(host);
		return -1;
	}
	if (!td->orig_w) {
		td->orig_w = win->w;
		td->orig_h = win->h;
	}
	if (!w) {
		w = td->orig_w;
		h = td->orig_h;
	} else {
		w += 2 * BORDER;
		h += 2 * BORDER + TITLE_H;
	}
	struct surface *scr = win->wm->disp->surf;
	w = MIN(w, scr->w);
	h = MIN(h, scr->h);
	int x = MIN(win->x, scr->w - w), y = MIN(win->y, scr->h - h);
	if (!win->maximized) {
		win_resize(win, MAX(0, x), MAX(0, y), w, h);
		d->surf = &win->content;
	}
	desk_unlock(host);
	return 0;
}

void display_close(struct display *d)
{
	if (!strcmp(d->name, "screen")) {
		struct screen_disp *sd = (struct screen_disp *)d;
		screen_suspend(0);
		surf_free(&sd->back);
		kfree(sd);
		return;
	}
	struct term_disp *td = (struct term_disp *)d;
	struct termwin *t = td->t;
	if (td->orig_w)
		display_want_size(d, 0, 0);
	if (tw_lock(t)) {
		t->child = 0;
		if (t->win) {
			tw_redraw(t);
			win_dirty_all(t->win);
		}
		desk_unlock(t->wm);
	} else {
		t->child = 0;
	}
	kfree(td);
}

int display_depth(void)
{
	struct wm *wm = task_current()->desk;
	return wm ? wm->depth + 1 : 0;
}

/* WIN: start a desktop on whatever this task's console is. */
int desktop_run(const char *remote)
{
	struct task *me = task_current();
	if (remote && *remote) {
		extern int remote_desktop(const char *host);
		return remote_desktop(remote);
	}
	extern struct console *console_unwrap(struct console *c);
	struct console *c = console_unwrap(me->con);
	extern int console_is_remote(struct console *c);
	if (console_is_remote(c)) {
		extern int remote_session_desktop(struct console *c);
		return remote_session_desktop(c);
	}
	int err = 0;
	struct display *d = display_open(c, &err);
	if (!d) {
		if (err == E_ACCES)
			kprintf("This window already has a desktop.\n");
		else if (err == E_NOTSUP)
			kprintf("WIN needs a screen or a desktop window (this console is %s).\n", c->name);
		else
			kprintf("Not enough memory for the desktop\n");
		return 1;
	}
	char title[48];
	int depth = display_depth();
	if (depth)
		snprintf(title, sizeof title, "Program Manager (level %d)", depth);
	else
		strcpy(title, "Program Manager");
	int r = desktop_run_on(d, title);
	display_close(d);
	return r;
}

/* ---- canvas windows for programs: the Win_ SYS calls ------------------------------------ */

static struct wm *my_desk(void)
{
	return task_current()->desk;
}

#define MAX_CANVAS 32
static struct win *canvases[MAX_CANVAS];

static struct win *canvas_get(long long id)
{
	if (id < 1 || id > MAX_CANVAS)
		return NULL;
	struct win *w = canvases[id - 1];
	if (!w || w->owner != task_current())
		return NULL;
	return w;
}

static uint32_t argcol(struct sysval *a, int argc, int i, uint32_t def)
{
	return P(i < argc ? (uint32_t)a[i].n & 0xFFFFFF : def);
}

static int sys_win_open_u(int argc, struct sysval *a, struct sysval *r)
{
	struct wm *wm = my_desk();
	if (!wm)
		return -2;	/* no desktop */
	const char *title = argc > 0 && a[0].is_str ? a[0].s : "Window";
	int x = argc > 1 ? (int)a[1].n : -1, y = argc > 2 ? (int)a[2].n : -1;
	int w = argc > 3 ? (int)MAX(40, a[3].n) : 320, h = argc > 4 ? (int)MAX(30, a[4].n) : 200;
	int slot = -1;
	for (int i = 0; i < MAX_CANVAS; i++)
		if (!canvases[i]) {
			slot = i;
			break;
		}
	if (slot < 0)
		return -3;
	struct win *win = win_new(wm, W_CANVAS, title, x, y, w, h);
	if (!win)
		return -3;
	win->owner = task_current();
	win->resizable = 1;
	canvases[slot] = win;
	r->n = slot + 1;
	return 0;
}

static int sys_win_close_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w)
		return -2;
	canvases[a[0].n - 1] = NULL;
	if (w->wm) {
		w->owner = NULL;
		win_destroy(w->wm, w);
	} else {
		kfree(w);	/* already closed by the user */
	}
	return 0;
}

static int sys_win_clear_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || !w->wm)
		return w ? 0 : -2;
	fill(&w->content, 0, 0, w->content.w, w->content.h, argcol(a, argc, 1, 0xFFFFFF));
	win_dirty_all(w);
	return 0;
}

static int sys_win_rect_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || argc < 5)
		return -2;
	if (!w->wm)
		return 0;
	int x = (int)a[1].n, y = (int)a[2].n, wd = (int)a[3].n, ht = (int)a[4].n;
	fill(&w->content, x, y, wd, ht, argcol(a, argc, 5, 0));
	win_dirty(w, (struct rect){ x, y, wd, ht });
	return 0;
}

static int sys_win_line_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || argc < 5)
		return -2;
	if (!w->wm)
		return 0;
	int x0 = (int)a[1].n, y0 = (int)a[2].n, x1 = (int)a[3].n, y1 = (int)a[4].n;
	line(&w->content, x0, y0, x1, y1, argcol(a, argc, 5, 0));
	win_dirty(w, (struct rect){ MIN(x0, x1), MIN(y0, y1), (x0 > x1 ? x0 - x1 : x1 - x0) + 1,
	    (y0 > y1 ? y0 - y1 : y1 - y0) + 1 });
	return 0;
}

static int sys_win_pixel_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || argc < 3)
		return -2;
	if (!w->wm)
		return 0;
	plot(&w->content, (int)a[1].n, (int)a[2].n, argcol(a, argc, 3, 0));
	win_dirty(w, (struct rect){ (int)a[1].n, (int)a[2].n, 1, 1 });
	return 0;
}

static int sys_win_text_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || argc < 4 || !a[3].is_str)
		return -2;
	if (!w->wm)
		return 0;
	int x = (int)a[1].n, y = (int)a[2].n;
	text(&w->content, x, y, a[3].s, argcol(a, argc, 4, 0));
	win_dirty(w, (struct rect){ x, y, (int)strlen(a[3].s) * FONT_W, FONT_H });
	return 0;
}

/* Win_Event id: 0 nothing, 1 key, 2 click, 3 mouse moved, 4 resized, -1 closed */
static int sys_win_event_u(int argc, struct sysval *a, struct sysval *r)
{
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w)
		return -2;
	struct event e;
	r->n = 0;
	while (evq_pop(&w->events, &e)) {
		if (e.type == EV_CLOSE) {
			r->n = -1;
			return 0;
		}
		if (e.type == EV_KEY) {
			w->last_key = e.key;
			r->n = 1;
			return 0;
		}
		if (e.type == EV_RESIZE) {
			r->n = 4;
			return 0;
		}
		w->last_x = e.x;
		w->last_y = e.y;
		r->n = e.type == EV_DOWN ? 2 : 3;
		return 0;
	}
	if (!w->wm)
		r->n = -1;
	return 0;
}

static int sys_win_info_u(int argc, struct sysval *a, struct sysval *r, int what)
{
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w)
		return -2;
	r->n = what == 0 ? w->last_key : what == 1 ? w->last_x : what == 2 ? w->last_y :
	    what == 3 ? w->content.w : w->content.h;
	return 0;
}


static int sys_desk_color_u(int argc, struct sysval *a, struct sysval *r);
static int sys_desk_run_u(int argc, struct sysval *a, struct sysval *r);
static int sys_desk_size_u(int argc, struct sysval *a, struct sysval *r);

/* programs drawing on the desktop (BASIC, C): under its lock */
#define DESK_LOCKED(name) \
	static int name(int argc, struct sysval *a, struct sysval *r) \
	{ \
		struct wm *wm = my_desk(); \
		if (wm) \
			desk_lock(wm); \
		int x = name##_u(argc, a, r); \
		if (wm) \
			desk_unlock(wm); \
		return x; \
	}
DESK_LOCKED(sys_win_open)
DESK_LOCKED(sys_win_close)
DESK_LOCKED(sys_win_clear)
DESK_LOCKED(sys_win_rect)
DESK_LOCKED(sys_win_line)
DESK_LOCKED(sys_win_pixel)
DESK_LOCKED(sys_win_text)
DESK_LOCKED(sys_win_event)
DESK_LOCKED(sys_desk_color)
DESK_LOCKED(sys_desk_run)
DESK_LOCKED(sys_desk_size)

static int sys_win_info(int argc, struct sysval *a, struct sysval *r, int what)
{
	struct wm *wm = my_desk();
	if (wm)
		desk_lock(wm);
	int x = sys_win_info_u(argc, a, r, what);
	if (wm)
		desk_unlock(wm);
	return x;
}

static int sys_win_key(int c, struct sysval *a, struct sysval *r) { return sys_win_info(c, a, r, 0); }
static int sys_win_x(int c, struct sysval *a, struct sysval *r) { return sys_win_info(c, a, r, 1); }
static int sys_win_y(int c, struct sysval *a, struct sysval *r) { return sys_win_info(c, a, r, 2); }
static int sys_win_w(int c, struct sysval *a, struct sysval *r) { return sys_win_info(c, a, r, 3); }
static int sys_win_h(int c, struct sysval *a, struct sysval *r) { return sys_win_info(c, a, r, 4); }

static int sys_win_wait_close(int argc, struct sysval *a, struct sysval *r)
{
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w)
		return -2;
	for (;;) {
		struct sysval ev;
		sys_win_event(argc, a, &ev);
		if (ev.n == -1)
			break;
		if (con_pollkey() == KEY_EOF)	/* our terminal went away */
			break;
		task_idle();
	}
	r->n = 0;
	return sys_win_close(argc, a, r);
}

static int sys_desk_color_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct wm *wm = my_desk();
	if (!wm || !argc)
		return -2;
	wm->desktop = (uint32_t)a[0].n & 0xFFFFFF;
	wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
	return 0;
}

static int sys_desk_message(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct wm *wm = my_desk();
	if (!wm || !argc || !a[0].is_str)
		return -2;
	desk_lock(wm);
	struct win *w = open_dialog(wm, argc > 1 && a[1].is_str ? a[1].s : "Message", a[0].s, 0, 3);
	if (!w) {
		desk_unlock(wm);
		return -3;
	}
	render_dialog(w);
	desk_unlock(wm);
	while (!w->dlg->done)
		task_idle();
	desk_lock(wm);
	if (!w->closed) {	/* still on screen: take it down */
		w->dlg->purpose = 0;
		win_destroy(wm, w);
	} else {
		kfree(w->dlg);
		surf_free(&w->content);
		kfree(w);
	}
	desk_unlock(wm);
	return 0;
}

static int sys_desk_run_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct wm *wm = my_desk();
	if (!wm || !argc || !a[0].is_str)
		return -2;
	launch_cmd(wm, a[0].s, a[0].s);
	return 0;
}

static int sys_desk_size_u(int argc, struct sysval *a, struct sysval *r)
{
	(void)argc; (void)a;
	struct wm *wm = my_desk();
	if (!wm)
		return -2;
	r->n = (long long)wm->disp->surf->w << 16 | wm->disp->surf->h;
	return 0;
}

static const struct sys_entry desk_calls[] = {
	{ "Desk_Color", sys_desk_color, "Desk_Color &RRGGBB    - repaint this desktop" },
	{ "Desk_Colour", sys_desk_color, NULL },
	{ "Desk_Message", sys_desk_message, "Desk_Message text$    - message box, waits for OK" },
	{ "Desk_Run", sys_desk_run, "Desk_Run cmd$         - start a command in a new window" },
	{ "Desk_Size", sys_desk_size, "Desk_Size             - width<<16 | height" },
	{ "Win_Open", sys_win_open, "Win_Open t$,x,y,w,h TO id - a window to draw in (-1 = auto place)" },
	{ "Win_Close", sys_win_close, "Win_Close id" },
	{ "Win_Clear", sys_win_clear, "Win_Clear id, &RRGGBB" },
	{ "Win_Rect", sys_win_rect, "Win_Rect id,x,y,w,h,&RRGGBB" },
	{ "Win_Line", sys_win_line, "Win_Line id,x0,y0,x1,y1,&RRGGBB" },
	{ "Win_Pixel", sys_win_pixel, "Win_Pixel id,x,y,&RRGGBB" },
	{ "Win_Text", sys_win_text, "Win_Text id,x,y,text$,&RRGGBB" },
	{ "Win_Event", sys_win_event, "Win_Event id TO e     - 0 none 1 key 2 click 3 move 4 resized -1 closed" },
	{ "Win_Key", sys_win_key, "Win_Key id TO k       - last key" },
	{ "Win_X", sys_win_x, "Win_X id TO x         - last mouse x" },
	{ "Win_Y", sys_win_y, "Win_Y id TO y         - last mouse y" },
	{ "Win_Width", sys_win_w, NULL },
	{ "Win_Height", sys_win_h, NULL },
	{ "Win_WaitClose", sys_win_wait_close, "Win_WaitClose id      - wait for the user to close it" },
};

/* ---- devices: DESK$, DRAW$, MOUSE$ -------------------------------------------------------- */

static uint32_t parse_rgb(const char *s)
{
	while (*s == ' ' || *s == '#' || *s == '&')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	return (uint32_t)strtol(s, NULL, 16) & 0xFFFFFF;
}

/* Split "cmd arg arg "quoted arg"" into words. */
static int words(char *s, char **w, int max)
{
	int n = 0;
	while (*s && n < max) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (*s == '"') {
			w[n++] = ++s;
			while (*s && *s != '"')
				s++;
		} else {
			w[n++] = s;
			while (*s && *s != ' ' && *s != '\t')
				s++;
		}
		if (*s)
			*s++ = 0;
	}
	return n;
}

static void desk_command(struct wm *wm, char *line)
{
	char *w[8];
	int n = words(line, w, 8);
	if (!n)
		return;
	if (!strcasecmp(w[0], "color") || !strcasecmp(w[0], "colour")) {
		if (n > 1) {
			wm->desktop = parse_rgb(w[1]);
			wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
		}
	} else if (!strcasecmp(w[0], "accent") && n > 1) {
		theme.active = theme.select = parse_rgb(w[1]);
		extern void repaint_all(void);
		repaint_all();
	} else if (!strcasecmp(w[0], "open") || !strcasecmp(w[0], "shell")) {
		open_term(wm, n > 1 ? w[1] : "Shell", NULL, 80, 25,
		    n > 2 ? (int)strtol(w[2], NULL, 10) : -1, n > 3 ? (int)strtol(w[3], NULL, 10) : -1);
	} else if (!strcasecmp(w[0], "run") && n > 1) {
		/* run the rest of the line */
		char cmd[200] = "";
		for (int i = 1; i < n; i++) {
			strcat0(cmd, w[i], sizeof cmd);
			if (i + 1 < n)
				strcat0(cmd, " ", sizeof cmd);
		}
		launch_cmd(wm, cmd, cmd);
	} else if (!strcasecmp(w[0], "raise") && n > 1) {
		/* bring a window forward by its title (the rest of the line) */
		char title[64] = "";
		for (int i = 1; i < n; i++) {
			strcat0(title, w[i], sizeof title);
			if (i + 1 < n)
				strcat0(title, " ", sizeof title);
		}
		for (struct win *v = wm->wins; v; v = v->next)
			if (!strcasecmp(v->title, title)) {
				if (v->minimized)
					win_minimize(v, 0);
				raise_win(wm, v);
				break;
			}
	} else if (!strcasecmp(w[0], "files")) {
		open_files(wm, n > 1 ? w[1] : task_current()->cwd);
	} else if (!strcasecmp(w[0], "tile")) {
		tile(wm, 0);
	} else if (!strcasecmp(w[0], "cascade")) {
		tile(wm, 1);
	} else if (!strcasecmp(w[0], "message") && n > 1) {
		open_dialog(wm, "Message", w[1], 0, 0);
	} else if (!strcasecmp(w[0], "about")) {
		about(wm);
	} else if (!strcasecmp(w[0], "exit")) {
		wm->running = 0;
	} else if (!strcasecmp(w[0], "mirror") && n > 1) {
		mirror_terms = !strcasecmp(w[1], "on");
	}
}

static long desk_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	struct wm *wm = my_desk();
	if (!wm)
		return E_NOTSUP;	/* "Not supported on this drive": no desktop here */
	desk_lock(wm);
	char line[256];
	size_t o = 0;
	const char *p = buf;
	for (size_t i = 0; i <= n; i++) {
		if (i == n || p[i] == '\n' || p[i] == '\r') {
			line[o] = 0;
			if (o)
				desk_command(wm, line);
			o = 0;
		} else if (o + 1 < sizeof line) {
			line[o++] = p[i];
		}
	}
	desk_unlock(wm);
	return (long)n;
}

static long desk_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	struct wm *wm = my_desk();
	char info[1536];
	int len;
	if (!wm)
		len = snprintf(info, sizeof info, "no desktop\n");
	else {
		desk_lock(wm);
		int nw = 0;
		for (struct win *w = wm->wins; w; w = w->next)
			nw++;
		len = snprintf(info, sizeof info, "desktop level %d %dx%d windows %d color #%06x\n",
		    wm->depth, wm->disp->surf->w, wm->disp->surf->h, nw, wm->desktop);
		/* one line per window, bottom first: id x y w h focus "title" */
		for (struct win *w = wm->wins; w && len < (int)sizeof info - 100; w = w->next)
		{
			char back[24] = "";
			if (w->term && w->term->view)
				snprintf(back, sizeof back, " back %d", w->term->view);
			len += snprintf(info + len, sizeof info - (size_t)len, "window %d %d %d %d %d%s%s%s \"%s\"\n",
			    w->id, w->x, w->y, w->w, w->h, w == wm->focus ? " focus" : "",
			    w->minimized ? " minimized" : "", back, w->title);
		}
		desk_unlock(wm);
	}
	size_t c = MIN(n, (size_t)len);
	memcpy(buf, info, c);
	return (long)c;
}

static long mouse_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	struct wm *wm = my_desk();
	char info[64];
	int len = snprintf(info, sizeof info, "%d %d %d\n", wm ? wm->mx : 0, wm ? wm->my : 0,
	    wm ? wm->buttons : 0);
	size_t c = MIN(n, (size_t)len);
	memcpy(buf, info, c);
	return (long)c;
}

/*
 * DRAW$: drawing commands for the calling task's own terminal window, the
 * way a program in a rio window draws on that window.
 *   clear #rrggbb | rect x y w h #c | line x0 y0 x1 y1 #c | text x y #c words...
 */
static long draw_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	extern struct console *console_unwrap(struct console *c);
	struct console *c = console_unwrap(task_current()->con);
	if (!console_is_termwin(c))
		return E_NOTSUP;
	struct termwin *t = c->priv;
	if (!tw_lock(t))
		return (long)n;
	if (!t->win) {
		desk_unlock(t->wm);
		return (long)n;
	}
	struct surface *s = &t->win->content;
	char cmd[256];
	size_t o = 0;
	const char *p = buf;
	for (size_t i = 0; i <= n; i++) {
		if (i < n && p[i] != '\n' && p[i] != '\r') {
			if (o + 1 < sizeof cmd)
				cmd[o++] = p[i];
			continue;
		}
		cmd[o] = 0;
		o = 0;
		char *w[10];
		int nw = words(cmd, w, 10);
		if (!nw)
			continue;
#define NUM(i) ((int)strtol(w[i], NULL, 10))
		if (!strcasecmp(w[0], "clear") && nw > 1)
			fill(s, 0, 0, s->w, s->h, P(parse_rgb(w[1])));
		else if (!strcasecmp(w[0], "rect") && nw > 5)
			fill(s, NUM(1), NUM(2), NUM(3), NUM(4), P(parse_rgb(w[5])));
		else if (!strcasecmp(w[0], "line") && nw > 5)
			line(s, NUM(1), NUM(2), NUM(3), NUM(4), P(parse_rgb(w[5])));
		else if (!strcasecmp(w[0], "text") && nw > 4) {
			char str[200] = "";
			for (int k = 4; k < nw; k++) {
				strcat0(str, w[k], sizeof str);
				if (k + 1 < nw)
					strcat0(str, " ", sizeof str);
			}
			text(s, NUM(1), NUM(2), str, P(parse_rgb(w[3])));
		} else if (!strcasecmp(w[0], "redraw"))
			tw_redraw(t);
#undef NUM
		win_dirty(t->win, (struct rect){ 0, 0, s->w, s->h });
	}
	desk_unlock(t->wm);
	return (long)n;
}

/* CLIP$: the text clipboard. TYPE CLIP$ shows it; DIR > CLIP$ fills it */
static long clip_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	size_t c = MIN(n, clip_len);
	memcpy(buf, clip_text_get(), c);
	return (long)c;
}

static long clip_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	/* one program's writes in quick succession are one piece of text */
	uint64_t now = uptime_ms();
	if (clip_writer == task_current() && now - clip_last_write < 500 && clip_text) {
		char *j = kmalloc(clip_len + n + 1);
		if (!j)
			return E_NOMEM;
		memcpy(j, clip_text, clip_len);
		memcpy(j + clip_len, buf, n);
		j[clip_len + n] = 0;
		kfree(clip_text);
		clip_text = j;
		clip_len += n;
	} else {
		clip_text_set(buf, n);
	}
	clip_writer = task_current();
	clip_last_write = now;
	return (long)n;
}

static struct chardev clip_dev = { "CLIP$", clip_read, clip_write, NULL, 1 };
static struct chardev desk_dev = { "DESK$", desk_read, desk_write, NULL, 1 };
static struct chardev mouse_dev = { "MOUSE$", mouse_read, NULL, NULL, 1 };
static struct chardev draw_dev = { "DRAW$", NULL, draw_write, NULL, 0 };

/* repaint every window of every desktop: the colours changed */
void repaint_all(void)
{
	for (struct wm *wm = all_wms; wm; wm = wm->next_wm)
		wm->want_repaint = 1;	/* each desktop does its own */
}

static void wm_settings(const char *key)
{
	if (!*key || !strcmp(key, "mouse.dclick"))
		dclick_ms = (int)MAX(150, MIN(1500, setting_int("mouse.dclick")));
	if (*key && !strcmp(key, "desktop.color"))
		for (struct wm *wm = all_wms; wm; wm = wm->next_wm)
			if (!wm->depth && !wm->follow_time) {
				wm->desktop = setting_rgb("desktop.color") & 0xFFFFFF;
				wm->want_dirty = 1;
			}
	if (*key && !strcmp(key, "desktop.timeofday"))
		for (struct wm *wm = all_wms; wm; wm = wm->next_wm) {
			if (wm->depth)
				continue;
			wm->follow_time = setting_int("desktop.timeofday") != 0;
			wm->next_time_check = 0;
			if (!wm->follow_time) {
				wm->desktop = setting_rgb("desktop.color") & 0xFFFFFF;
				wm->want_dirty = 1;
			}
		}
}

void desktop_init(void)
{
	setting_watch(wm_settings);
	themes_init();
	dev_register(&desk_dev);
	dev_register(&clip_dev);
	dev_register(&mouse_dev);
	dev_register(&draw_dev);
	for (size_t i = 0; i < NELEM(desk_calls); i++)
		sys_register(&desk_calls[i]);
}

