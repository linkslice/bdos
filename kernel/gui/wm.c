/*
 * wm.c - the bdos desktop.
 *
 * A window manager runs in the task that typed WIN and draws on a display:
 * the screen, the inside of a terminal window on another desktop (so WIN
 * inside WIN nests), or a network connection. Each terminal window is a
 * full console with its own shell task; tasks remember which desktop they
 * live on, so DESK$, DRAW$, MOUSE$ and the Win_ SYS calls act on *their*
 * desktop, nested or not.
 *
 * Layout and look follow Program Manager; behaviour follows Plan 9's rio.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../sysapi.h"
#include "gfx.h"
#include "wm.h"
#include "form.h"
#include "../settings.h"

#define BORDER		4
#define TITLE_H		20
#define MENU_H		20
#define CLIENT_X	BORDER
#define CLIENT_Y	(BORDER + TITLE_H)
#define FRAME_W(cw)	((cw) + 2 * BORDER)
#define FRAME_H(ch)	((ch) + 2 * BORDER + TITLE_H)
static int dclick_ms = 450;	/* the mouse.dclick setting */
#define DCLICK_MS	dclick_ms
#define GRIP		5	/* how near the edge a press starts a resize */

enum { W_TERM, W_PROGMAN, W_FILES, W_CANVAS, W_DIALOG, W_FORM };

struct theme {
	uint32_t desktop, face, active, inactive, title_text, menu_text, select;
};

static struct theme theme = {
	0x2F6F73, 0xC3C3C3, 0x1D3A8A, 0x7B7B7B, 0xFFFFFF, 0x000000, 0x1D3A8A,
};

/* window colour schemes (the desktop.scheme setting) */
static const struct scheme schemes[] = {
	{ "Default", 0xC3C3C3, 0x1D3A8A, 0x7B7B7B, 0xFFFFFF, 0x1D3A8A, 0 },
	{ "Ocean", 0xB8CCD4, 0x0F5F7F, 0x6F8C98, 0xFFFFFF, 0x0F5F7F, 0 },
	{ "Plum", 0xCCC2D0, 0x5B2F72, 0x857790, 0xFFFFFF, 0x5B2F72, 0 },
	{ "Graphite", 0xB4B4B4, 0x303030, 0x7A7A7A, 0xFFFFFF, 0x505050, 0 },
	{ "Rose", 0xD8C4C4, 0x8A2F3F, 0x9A7A80, 0xFFFFFF, 0x8A2F3F, 0 },
	{ "Hot Dog Stand", 0xFFFF00, 0xFF0000, 0xC00000, 0xFFFFFF, 0xFF0000, 0 },
};

const struct scheme *wm_schemes(int *n)
{
	*n = (int)NELEM(schemes);
	return schemes;
}

struct wm;
struct win;

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

/* ---- terminal windows -------------------------------------------------------- */

struct termwin {
	struct console con;
	struct win *win;
	struct wm *wm;
	int cols, rows, cx, cy, attr, cursor_on;
	uint16_t *grid;
	int keys[256];
	int kh, kt;
	struct task *task;
	int closed;		/* the window is gone: input reads as end-of-file */
	int hold;		/* keep the window after the command ends */
	int child;		/* a nested desktop or remote view owns the pixels */
	struct evq child_ev;
	struct termwin *next_zombie;
};

struct item {
	char label[48];
	char cmd[200];
	char icon[16];
	int order;
};

struct group {
	char name[48];
	struct item items[24];
	int nitems;
	struct rect r;		/* in client coordinates */
};

struct progman {
	struct group groups[8];
	int ngroups;
	int sel_g, sel_i;
	int menu_open;		/* which menu-bar menu is down, -1 none */
};

struct files {
	char dir[VFS_PATH_MAX];
	struct vstat *ents;
	int n, sel, top;
};

struct dialog {
	char lines[6][80];
	int nlines;
	int input;		/* has a text field */
	char field[160];
	int done;		/* 1 OK, 2 Cancel */
	int purpose;		/* 0 message, 1 Run... */
};

struct win {
	int id;
	int kind;
	char title[64];
	int x, y, w, h;		/* frame, in desktop coordinates */
	struct surface content;
	struct wm *wm;
	struct termwin *term;
	struct progman *pm;
	struct files *fm;
	struct dialog *dlg;
	struct form *form;
	int resizable;
	int maximized;
	struct rect restore;	/* frame before maximizing */
	struct task *owner;	/* canvas windows: the task drawing in it */
	struct evq events;
	int last_key, last_x, last_y;
	int closed;
	struct win *next;	/* z order, bottom first */
};

struct wm {
	struct display *disp;
	struct win *wins;
	struct win *focus;
	int mx, my, buttons;
	struct win *drag;
	int drag_dx, drag_dy;
	struct rect dirty;
	int running;
	int next_id;
	int depth;
	uint64_t click_ms;
	int click_x, click_y;
	struct win *progman;
	char title[48];
	uint32_t desktop;	/* this desktop's own colour */
	struct win *resizing;	/* border being dragged */
	int rs_edges;		/* 1 left 2 right 4 top 8 bottom */
	struct rect rs_orig, ghost;
	int rs_mx, rs_my;
	int ptr;		/* pointer shape: 0 arrow, else resize edges */
	struct win *capture;	/* a form slider being dragged */
	struct wm *next_wm;
};

static struct wm *all_wms;	/* every running desktop, for settings changes */
static void form_free(struct form *f);
static int win_can_resize(struct win *w);
static void win_resize(struct win *w, int x, int y, int fw, int fh);

static struct termwin *zombies;
static int mirror_terms = 1;	/* copy terminal window text to COM1 (tests) */

static uint32_t P(uint32_t rgb) { return col(rgb); }

static void wm_dirty(struct wm *wm, struct rect r)
{
	wm->dirty = rect_union(wm->dirty, r);
}

static struct rect frame_rect(struct win *w)
{
	return (struct rect){ w->x, w->y, w->w, w->h };
}

static void win_dirty(struct win *w, struct rect r)
{
	if (w->closed)
		return;
	r.x += w->x + CLIENT_X;
	r.y += w->y + CLIENT_Y;
	wm_dirty(w->wm, rect_isect(r, frame_rect(w)));
}

static void win_dirty_all(struct win *w)
{
	if (!w->closed)
		wm_dirty(w->wm, frame_rect(w));
}

/* ---- the terminal console ------------------------------------------------------ */

static void tw_cell(struct termwin *t, int c, int r)
{
	if (t->child || t->closed)
		return;
	uint16_t cell = t->grid[r * t->cols + c];
	uint32_t fg = P(cga_palette[(cell >> 8) & 0xF]), bg = P(cga_palette[(cell >> 12) & 0xF]);
	glyph(&t->win->content, c * FONT_W, r * FONT_H, (unsigned char)(cell & 0xFF), fg, bg, 1);
	if (t->cursor_on && c == t->cx && r == t->cy)
		fill(&t->win->content, c * FONT_W, r * FONT_H + 14, FONT_W, 2, fg);
	win_dirty(t->win, (struct rect){ c * FONT_W, r * FONT_H, FONT_W, FONT_H });
}

static void tw_redraw(struct termwin *t)
{
	for (int r = 0; r < t->rows; r++)
		for (int c = 0; c < t->cols; c++)
			tw_cell(t, c, r);
}

static void tw_cursor_cell(struct termwin *t)
{
	if (t->cx < t->cols && t->cy < t->rows)
		tw_cell(t, t->cx, t->cy);
}

static void tw_scroll(struct termwin *t)
{
	memmove(t->grid, t->grid + t->cols, sizeof(uint16_t) * (size_t)(t->cols * (t->rows - 1)));
	for (int c = 0; c < t->cols; c++)
		t->grid[(t->rows - 1) * t->cols + c] = (uint16_t)(t->attr << 8 | ' ');
	if (t->child || t->closed)
		return;
	/* move the pixels up one text row */
	struct surface *s = &t->win->content;
	for (int y = 0; y < s->h - FONT_H; y++)
		memcpy(s->px + (size_t)y * (size_t)s->pitch, s->px + (size_t)(y + FONT_H) * (size_t)s->pitch,
		    (size_t)s->w * 4);
	for (int c = 0; c < t->cols; c++) {
		tw_cell(t, c, t->rows - 1);
		if (t->rows > 1)
			tw_cell(t, c, t->rows - 2);	/* the old bottom row: no stale cursor */
	}
	win_dirty(t->win, (struct rect){ 0, 0, s->w, s->h });
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

static void tw_write(struct console *c, const char *s, size_t n)
{
	struct termwin *t = c->priv;
	for (size_t i = 0; i < n; i++) {
		if (mirror_terms)
			serial_putc(s[i]);
		tw_putc(t, s[i]);
	}
}

static int tw_getkey(struct console *c)
{
	struct termwin *t = c->priv;
	if (t->closed)
		return KEY_EOF;
	if (t->kt == t->kh)
		return -1;
	int k = t->keys[t->kt];
	t->kt = (t->kt + 1) % 256;
	return k;
}

static void tw_clear(struct console *c)
{
	struct termwin *t = c->priv;
	for (int i = 0; i < t->cols * t->rows; i++)
		t->grid[i] = (uint16_t)(t->attr << 8 | ' ');
	t->cx = t->cy = 0;
	if (mirror_terms)
		serial_putc('\n');
	tw_redraw(t);
}

static void tw_cursor(struct console *c, int on)
{
	struct termwin *t = c->priv;
	t->cursor_on = on;
	tw_cursor_cell(t);
}

static void tw_size(struct console *c, int *w, int *h)
{
	struct termwin *t = c->priv;
	*w = t->cols;
	*h = t->rows;
}

static void tw_gotoxy(struct console *c, int x, int y)
{
	struct termwin *t = c->priv;
	int ox = t->cx, oy = t->cy;
	t->cx = MAX(0, MIN(x, t->cols - 1));
	t->cy = MAX(0, MIN(y, t->rows - 1));
	if (ox < t->cols && oy < t->rows)
		tw_cell(t, ox, oy);
	tw_cursor_cell(t);
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

/* ---- windows ------------------------------------------------------------------ */

static struct win *win_new(struct wm *wm, int kind, const char *title, int x, int y, int cw, int ch)
{
	struct win *w = kzalloc(sizeof *w);
	if (!w)
		return NULL;
	if (surf_init(&w->content, cw, ch)) {
		kfree(w);
		return NULL;
	}
	fill(&w->content, 0, 0, cw, ch, P(0xFFFFFF));
	w->id = ++wm->next_id;
	w->kind = kind;
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

static void raise_win(struct wm *wm, struct win *w)
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
	if (wm->focus && wm->focus != w)
		win_dirty_all(wm->focus);
	wm->focus = w;
	win_dirty_all(w);
}

static struct win *topmost(struct wm *wm)
{
	struct win *t = NULL;
	for (struct win *w = wm->wins; w; w = w->next)
		t = w;
	return t;
}

static void win_destroy(struct wm *wm, struct win *w)
{
	struct win **pp = &wm->wins;
	while (*pp && *pp != w)
		pp = &(*pp)->next;
	if (*pp)
		*pp = w->next;
	wm_dirty(wm, frame_rect(w));
	w->closed = 1;
	if (wm->focus == w)
		wm->focus = topmost(wm);
	if (wm->focus)
		win_dirty_all(wm->focus);
	if (w == wm->progman)
		wm->progman = NULL;
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
	if (w->form) {
		if (wm->capture == w)
			wm->capture = NULL;
		form_free(w->form);	/* tells its handler if it didn't know */
	}
	if (wm->resizing == w)
		wm->resizing = NULL;
	if (w->pm)
		kfree(w->pm);
	if (w->fm) {
		kfree(w->fm->ents);
		kfree(w->fm);
	}
	kfree(w->dlg);
	surf_free(&w->content);
	kfree(w);
}

static void reap_zombies(void)
{
	struct termwin **pp = &zombies;
	while (*pp) {
		struct termwin *t = *pp;
		if (!t->task || !task_alive(t->task)) {
			*pp = t->next_zombie;
			kfree(t->grid);
			kfree(t);
		} else {
			pp = &t->next_zombie;
		}
	}
}

/* ---- launching things in terminal windows ------------------------------------------ */

struct launch {
	char cmd[200];
	int hold;
};

static void term_task(void *arg)
{
	struct launch *l = arg;
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

static struct win *open_term(struct wm *wm, const char *title, const char *cmd, int cols, int rows,
    int x, int y)
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
	t->con.priv = t;
	w->term = t;
	for (int i = 0; i < cols * rows; i++)
		t->grid[i] = 0x0720;
	fill(&w->content, 0, 0, w->content.w, w->content.h, P(0));

	struct launch *l = kzalloc(sizeof *l);
	strncpy0(l->cmd, cmd ? cmd : "", sizeof l->cmd);
	l->hold = cmd && *cmd;
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

static void launch_cmd(struct wm *wm, const char *label, const char *cmd)
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
	/* plain commands that bring their own window */
	if (!strcasecmp(cmd, "@files")) {
		extern struct win *open_files(struct wm *wm, const char *dir);
		open_files(wm, task_current()->cwd);
		return;
	}
	open_term(wm, label, cmd, 80, 25, -1, -1);
}

/* ---- dialogs ---------------------------------------------------------------------- */

static void render_dialog(struct win *w);

static struct win *open_dialog(struct wm *wm, const char *title, const char *msg, int input,
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

static void render_dialog(struct win *w)
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

/* ---- forms: widget windows (Control Panel and friends) ------------------------------------ */

#define FORM_MAX 64

struct fwidget {
	int kind, x, y, w, h;
	char text[64];
	long val, min, max;
	const char *const *opts;
	int nopts;
	int disabled, hidden, def;
	uint32_t rgb;
	char icon[16];
	char **items;
	int nitems, top;
	int typing;		/* spin: digits typed since focus */
	form_draw_fn draw;
};

struct form {
	struct fwidget wd[FORM_MAX];
	int n;
	int focus;
	form_fn fn;
	void *priv;
	struct win *win;
	struct wm *wm;
	int cw, ch;
	int min_w, min_h;
	int closing, dead;
	int tick_ms;
	uint64_t next_tick;
	int px, py;		/* last click, relative to the widget */
	int drag;		/* slider being dragged, -1 none */
	char title[64];
};

static void render_form(struct win *w);

static int wm_focus_is(struct win *w)
{
	return w->wm && w->wm->focus == w;
}

struct form *form_new(const char *title, int w, int h, form_fn fn, void *priv)
{
	struct form *f = kzalloc(sizeof *f);
	if (!f)
		return NULL;
	strncpy0(f->title, title, sizeof f->title);
	f->cw = w;
	f->ch = h;
	f->fn = fn;
	f->priv = priv;
	f->focus = -1;
	f->drag = -1;
	return f;
}

static int focusable(struct fwidget *d)
{
	if (d->disabled || d->hidden)
		return 0;
	switch (d->kind) {
	case FW_BUTTON: case FW_EDIT: case FW_SPIN: case FW_CHOICE: case FW_CHECK:
	case FW_SLIDER: case FW_ICON: case FW_SWATCH: case FW_LIST:
		return 1;
	}
	return 0;
}

int form_add(struct form *f, int kind, int x, int y, int w, int h, const char *text)
{
	if (!f || f->n >= FORM_MAX)
		return -1;
	struct fwidget *d = &f->wd[f->n];
	memset(d, 0, sizeof *d);
	d->kind = kind;
	d->x = x;
	d->y = y;
	d->w = w;
	d->h = h;
	d->max = 100;
	if (text)
		strncpy0(d->text, text, sizeof d->text);
	if (f->focus < 0 && focusable(d))
		f->focus = f->n;
	return f->n++;
}

static struct fwidget *W(struct form *f, int id)
{
	static struct fwidget dummy;
	if (!f || id < 0 || id >= f->n)
		return &dummy;
	return &f->wd[id];
}

void form_range(struct form *f, int id, long min, long max)
{
	struct fwidget *d = W(f, id);
	d->min = min;
	d->max = max;
	d->val = MAX(min, MIN(d->val, max));
}

void form_options(struct form *f, int id, const char *const *opts, int n)
{
	struct fwidget *d = W(f, id);
	d->opts = opts;
	d->nopts = n;
	d->min = 0;
	d->max = n - 1;
}

void form_icon(struct form *f, int id, const char *icon) { strncpy0(W(f, id)->icon, icon, 16); }
void form_custom(struct form *f, int id, form_draw_fn draw) { W(f, id)->draw = draw; }
void form_default(struct form *f, int id) { W(f, id)->def = 1; }
int form_focused(struct form *f) { return f->focus; }
void form_enable(struct form *f, int id, int on) { W(f, id)->disabled = !on; }
void form_hide(struct form *f, int id, int hidden) { W(f, id)->hidden = hidden; }
void form_focus(struct form *f, int id) { if (focusable(W(f, id))) f->focus = id; }
long form_val(struct form *f, int id) { return W(f, id)->val; }
const char *form_text(struct form *f, int id) { return W(f, id)->text; }
void form_set_rgb(struct form *f, int id, uint32_t rgb) { W(f, id)->rgb = rgb; }
uint32_t form_rgb(struct form *f, int id) { return W(f, id)->rgb; }
void *form_priv(struct form *f) { return f->priv; }
struct wm *form_wm(struct form *f) { return f->wm; }
void form_point(struct form *f, int *x, int *y) { *x = f->px; *y = f->py; }

void form_move(struct form *f, int id, int x, int y, int w, int h)
{
	struct fwidget *d = W(f, id);
	d->x = x;
	d->y = y;
	d->w = w;
	d->h = h;
}

void form_set_val(struct form *f, int id, long v)
{
	struct fwidget *d = W(f, id);
	if (d->kind == FW_SPIN || d->kind == FW_SLIDER || d->kind == FW_CHOICE)
		v = MAX(d->min, MIN(v, d->max));
	d->val = v;
}

void form_set_text(struct form *f, int id, const char *text)
{
	strncpy0(W(f, id)->text, text, sizeof f->wd[0].text);
}

void form_list_clear(struct form *f, int id)
{
	struct fwidget *d = W(f, id);
	for (int i = 0; i < d->nitems; i++)
		kfree(d->items[i]);
	kfree(d->items);
	d->items = NULL;
	d->nitems = 0;
	d->val = 0;
	d->top = 0;
}

void form_list_add(struct form *f, int id, const char *line)
{
	struct fwidget *d = W(f, id);
	char **n = krealloc(d->items, sizeof(char *) * (size_t)(d->nitems + 1));
	if (!n)
		return;
	d->items = n;
	d->items[d->nitems++] = kstrdup(line);
}

void form_tick(struct form *f, int ms)
{
	f->tick_ms = ms;
	f->next_tick = uptime_ms() + (uint64_t)ms;
}

void form_resizable(struct form *f, int min_w, int min_h)
{
	f->min_w = min_w;
	f->min_h = min_h;
	if (f->win)
		f->win->resizable = 1;
}

void form_size(struct form *f, int *w, int *h)
{
	*w = f->win ? f->win->content.w : f->cw;
	*h = f->win ? f->win->content.h : f->ch;
}

void form_title(struct form *f, const char *title)
{
	strncpy0(f->title, title, sizeof f->title);
	if (f->win) {
		strncpy0(f->win->title, title, sizeof f->win->title);
		win_dirty_all(f->win);
	}
}

void form_redraw(struct form *f)
{
	if (f->win && !f->dead)
		render_form(f->win);
}

int form_show(struct wm *wm, struct form *f, int x, int y)
{
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	if (x < 0)
		x = (sw - FRAME_W(f->cw)) / 2 + (wm->next_id % 5) * 16;
	if (y < 0)
		y = (sh - FRAME_H(f->ch)) / 3 + (wm->next_id % 5) * 16;
	struct win *w = win_new(wm, W_FORM, f->title, x, y, f->cw, f->ch);
	if (!w) {
		form_list_clear(f, 0);
		kfree(f);
		return -1;
	}
	w->form = f;
	w->resizable = f->min_w > 0;
	f->win = w;
	f->wm = wm;
	render_form(w);
	return 0;
}

struct form *form_find(struct wm *wm, const char *title)
{
	for (struct win *w = wm->wins; w; w = w->next)
		if (w->form && !w->form->closing && !strcasecmp(w->form->title, title))
			return w->form;
	return NULL;
}

void form_raise(struct form *f)
{
	if (f->win)
		raise_win(f->wm, f->win);
}

void form_close(struct form *f)
{
	f->closing = 1;
}

static void form_free(struct form *f)
{
	f->dead = 1;
	if (f->fn) {	/* closed from outside (Alt+Q, desktop exit): tell it */
		form_fn fn = f->fn;
		f->fn = NULL;
		fn(f, -1, FE_CLOSE);
	}
	for (int i = 0; i < f->n; i++)
		if (f->wd[i].items) {
			for (int k = 0; k < f->wd[i].nitems; k++)
				kfree(f->wd[i].items[k]);
			kfree(f->wd[i].items);
		}
	kfree(f);
}

static void form_emit(struct form *f, int id, int ev)
{
	if (f->fn && !f->dead)
		f->fn(f, id, ev);
}

/* after every event: re-render, and take the window down if asked */
static void form_settle(struct form *f)
{
	if (f->dead)
		return;
	if (f->closing) {
		f->dead = 1;
		if (f->fn)
			f->fn(f, -1, FE_CLOSE);
		f->fn = NULL;
		if (f->win)
			win_destroy(f->wm, f->win);	/* frees f */
		return;
	}
	render_form(f->win);
}

/* ---- drawing ---- */

static void sunken(struct surface *s, int x, int y, int w, int h, uint32_t bg)
{
	fill(s, x, y, w, h, P(bg));
	bevel(s, x, y, w, h, 0);
}

static void arrow_glyph(struct surface *s, int cx, int cy, int dir, uint32_t c)
{
	/* a small solid triangle: 0 up 1 down 2 left 3 right */
	for (int i = 0; i < 4; i++) {
		switch (dir) {
		case 0: hline(s, cx - i, cy - 2 + i, 2 * i + 1, c); break;
		case 1: hline(s, cx - i, cy + 2 - i, 2 * i + 1, c); break;
		case 2: vline(s, cx - 2 + i, cy - i, 2 * i + 1, c); break;
		case 3: vline(s, cx + 2 - i, cy - i, 2 * i + 1, c); break;
		}
	}
}

static void small_button(struct surface *s, int x, int y, int w, int h, int dir)
{
	fill(s, x, y, w, h, P(theme.face));
	bevel(s, x, y, w, h, 1);
	arrow_glyph(s, x + w / 2, y + h / 2, dir, P(0));
}

static void focus_rect(struct surface *s, int x, int y, int w, int h)
{
	for (int i = 0; i < w; i += 2) {
		plot(s, x + i, y, P(0));
		plot(s, x + i, y + h - 1, P(0));
	}
	for (int i = 0; i < h; i += 2) {
		plot(s, x, y + i, P(0));
		plot(s, x + w - 1, y + i, P(0));
	}
}

static int slider_thumb_x(struct fwidget *d)
{
	long span = MAX(1, d->max - d->min);
	return d->x + 6 + (int)((d->val - d->min) * (d->w - 12) / span);
}

static void render_widget(struct form *f, int i)
{
	struct surface *s = &f->win->content;
	struct fwidget *d = &f->wd[i];
	if (d->hidden)
		return;
	int foc = i == f->focus && wm_focus_is(f->win);
	uint32_t ink = P(d->disabled ? 0x808080 : 0);
	char buf[80];
	switch (d->kind) {
	case FW_LABEL:
		text(s, d->x, d->y, d->text, ink);
		break;
	case FW_GROUP: {
		int ty = d->y + 7;
		outline(s, d->x, ty, d->w, d->h - 7, P(0x808080));
		outline(s, d->x + 1, ty + 1, d->w, d->h - 7, P(0xFFFFFF));
		if (d->text[0]) {
			int tw = (int)strlen(d->text) * FONT_W;
			fill(s, d->x + 8, d->y, tw + 6, FONT_H, P(theme.face));
			text(s, d->x + 11, d->y, d->text, P(0));
		}
		break;
	}
	case FW_BUTTON: {
		fill(s, d->x, d->y, d->w, d->h, P(theme.face));
		bevel(s, d->x, d->y, d->w, d->h, 1);
		if (d->def || foc)
			outline(s, d->x - 1, d->y - 1, d->w + 2, d->h + 2, P(0));
		int tw = (int)strlen(d->text) * FONT_W;
		text(s, d->x + (d->w - tw) / 2, d->y + (d->h - FONT_H) / 2, d->text, ink);
		if (foc)
			focus_rect(s, d->x + 4, d->y + 3, d->w - 8, d->h - 6);
		break;
	}
	case FW_EDIT:
		sunken(s, d->x, d->y, d->w, d->h, d->disabled ? theme.face : 0xFFFFFF);
		text(s, d->x + 4, d->y + (d->h - FONT_H) / 2, d->text, ink);
		if (foc)
			fill(s, d->x + 4 + (int)strlen(d->text) * FONT_W, d->y + 3, 2, d->h - 6, P(0));
		break;
	case FW_SPIN: {
		int bw = 16;
		sunken(s, d->x, d->y, d->w - bw, d->h, 0xFFFFFF);
		snprintf(buf, sizeof buf, d->text[0] == '0' ? "%02ld" : "%ld", d->val);	/* text "0" = zero-pad */
		if (foc)
			fill(s, d->x + 3, d->y + 3, (int)strlen(buf) * FONT_W + 2, d->h - 6, P(theme.select));
		text(s, d->x + 4, d->y + (d->h - FONT_H) / 2, buf, foc ? P(0xFFFFFF) : ink);
		small_button(s, d->x + d->w - bw, d->y, bw, d->h / 2, 0);
		small_button(s, d->x + d->w - bw, d->y + d->h / 2, bw, d->h - d->h / 2, 1);
		break;
	}
	case FW_CHOICE: {
		int bw = 18;
		small_button(s, d->x, d->y, bw, d->h, 2);
		small_button(s, d->x + d->w - bw, d->y, bw, d->h, 3);
		sunken(s, d->x + bw, d->y, d->w - 2 * bw, d->h, 0xFFFFFF);
		const char *o = d->val >= 0 && d->val < d->nopts ? d->opts[d->val] : "";
		int tw = (int)strlen(o) * FONT_W;
		int tx = d->x + (d->w - tw) / 2;
		if (foc)
			fill(s, tx - 2, d->y + 3, tw + 4, d->h - 6, P(theme.select));
		text(s, tx, d->y + (d->h - FONT_H) / 2, o, foc ? P(0xFFFFFF) : ink);
		break;
	}
	case FW_CHECK: {
		int by = d->y + (d->h - 13) / 2;
		sunken(s, d->x, by, 13, 13, 0xFFFFFF);
		if (d->val) {
			line(s, d->x + 3, by + 3, d->x + 9, by + 9, P(0));
			line(s, d->x + 9, by + 3, d->x + 3, by + 9, P(0));
			line(s, d->x + 4, by + 3, d->x + 9, by + 8, P(0));
			line(s, d->x + 8, by + 3, d->x + 3, by + 8, P(0));
		}
		text(s, d->x + 20, d->y + (d->h - FONT_H) / 2, d->text, ink);
		if (foc)
			focus_rect(s, d->x + 18, d->y + (d->h - FONT_H) / 2 - 1,
			    (int)strlen(d->text) * FONT_W + 4, FONT_H + 2);
		break;
	}
	case FW_SLIDER: {
		int cy = d->y + d->h / 2;
		sunken(s, d->x + 4, cy - 2, d->w - 8, 4, 0x808080);
		long span = MAX(1, d->max - d->min);
		for (long v = d->min; v <= d->max && span <= 40; v++) {
			int tx = d->x + 6 + (int)((v - d->min) * (d->w - 12) / span);
			vline(s, tx, d->y + d->h - 4, 4, P(0x606060));
		}
		int tx = slider_thumb_x(d);
		fill(s, tx - 5, d->y + 2, 11, d->h - 8, P(theme.face));
		bevel(s, tx - 5, d->y + 2, 11, d->h - 8, 1);
		if (foc)
			focus_rect(s, d->x, d->y, d->w, d->h);
		break;
	}
	case FW_ICON: {
		draw_icon(s, d->x + (d->w - 32) / 2, d->y + 2, d->icon);
		int tw = (int)strlen(d->text) * FONT_W;
		int tx = d->x + (d->w - tw) / 2, ty = d->y + 38;
		if (foc)
			fill(s, tx - 2, ty, tw + 4, FONT_H, P(theme.select));
		text(s, tx, ty, d->text, foc ? P(0xFFFFFF) : P(0));
		break;
	}
	case FW_SWATCH:
		if (d->val)
			fill(s, d->x - 3, d->y - 3, d->w + 6, d->h + 6, P(0));
		fill(s, d->x, d->y, d->w, d->h, P(d->rgb));
		bevel(s, d->x, d->y, d->w, d->h, 0);
		if (foc)
			focus_rect(s, d->x - 5, d->y - 5, d->w + 10, d->h + 10);
		break;
	case FW_LIST: {
		sunken(s, d->x, d->y, d->w, d->h, 0xFFFFFF);
		int rows = MAX(1, (d->h - 4) / 18);
		if (d->val < d->top)
			d->top = (int)d->val;
		if (d->val >= d->top + rows)
			d->top = (int)d->val - rows + 1;
		struct rect save = s->clip;
		surf_clip(s, (struct rect){ d->x + 2, d->y + 2, d->w - 4, d->h - 4 });
		for (int k = d->top; k < d->nitems && k < d->top + rows; k++) {
			int ry = d->y + 2 + (k - d->top) * 18;
			int sel = k == d->val;
			if (sel)
				fill(s, d->x + 2, ry, d->w - 4, 18, P(foc ? theme.select : 0xC0C0C0));
			text(s, d->x + 6, ry + 1, d->items[k], P(sel && foc ? 0xFFFFFF : 0));
		}
		s->clip = save;
		if (!d->nitems && d->text[0])
			text(s, d->x + 6, d->y + 4, d->text, P(0x808080));
		break;
	}
	case FW_CUSTOM:
		if (d->draw) {
			struct rect save = s->clip;
			surf_clip(s, (struct rect){ d->x, d->y, d->w, d->h });
			d->draw(f, i, s, (struct rect){ d->x, d->y, d->w, d->h });
			s->clip = save;
		}
		break;
	}
}

static void render_form(struct win *w)
{
	struct form *f = w->form;
	if (!f || f->dead)
		return;
	struct surface *s = &w->content;
	fill(s, 0, 0, s->w, s->h, P(theme.face));
	for (int i = 0; i < f->n; i++)
		render_widget(f, i);
	win_dirty_all(w);
}

/* ---- input ---- */

static void form_step(struct form *f, int i, long delta)
{
	struct fwidget *d = &f->wd[i];
	long v = d->val + delta;
	if (d->kind == FW_CHOICE) {	/* wraps around */
		if (v < 0)
			v = d->nopts - 1;
		if (v >= d->nopts)
			v = 0;
	}
	v = MAX(d->min, MIN(v, d->max));
	if (v != d->val) {
		d->val = v;
		form_emit(f, i, FE_CHANGE);
	}
}

static void form_next_focus(struct form *f, int dir)
{
	for (int k = 1; k <= f->n; k++) {
		int i = ((f->focus < 0 ? 0 : f->focus) + dir * k + f->n * 2) % f->n;
		if (focusable(&f->wd[i])) {
			f->focus = i;
			f->wd[i].typing = 0;
			return;
		}
	}
}

/* arrow keys between icons: the nearest icon in that direction */
static void form_icon_move(struct form *f, int dx, int dy)
{
	struct fwidget *c = &f->wd[f->focus];
	int best = -1, bestd = 1 << 30;
	for (int i = 0; i < f->n; i++) {
		struct fwidget *d = &f->wd[i];
		if (i == f->focus || d->kind != FW_ICON || !focusable(d))
			continue;
		int ex = d->x - c->x, ey = d->y - c->y;
		if ((dx && (ex * dx <= 0)) || (dy && (ey * dy <= 0)))
			continue;
		int dist = (dx ? ex * dx : ey * dy) * 4 + (dx ? (ey < 0 ? -ey : ey) : (ex < 0 ? -ex : ex)) * 8;
		if (dist < bestd) {
			bestd = dist;
			best = i;
		}
	}
	if (best >= 0)
		f->focus = best;
}

static void form_key(struct wm *wm, struct win *w, int k)
{
	(void)wm;
	struct form *f = w->form;
	if (!f || f->dead)
		return;
	struct fwidget *d = f->focus >= 0 ? &f->wd[f->focus] : NULL;
	if (k == 27) {
		form_close(f);
	} else if (k == '\t') {
		form_next_focus(f, 1);
	} else if (k == KEY_SHIFT_TAB) {
		form_next_focus(f, -1);
	} else if (d && d->kind == FW_EDIT && k == '\b') {
		size_t l = strlen(d->text);
		if (l) {
			d->text[l - 1] = 0;
			form_emit(f, f->focus, FE_CHANGE);
		}
	} else if (d && d->kind == FW_EDIT && k >= 0x20 && k < 0x7F) {
		size_t l = strlen(d->text);
		if (l + 1 < sizeof d->text && (int)(l + 1) * FONT_W < d->w - 8) {
			d->text[l] = (char)k;
			d->text[l + 1] = 0;
			form_emit(f, f->focus, FE_CHANGE);
		}
	} else if (d && d->kind == FW_SPIN && k >= '0' && k <= '9') {
		long v = d->typing ? d->val * 10 + (k - '0') : (k - '0');
		if (v > d->max)
			v = k - '0';
		d->typing = 1;
		d->val = v;		/* clamped when focus leaves or Enter */
		form_emit(f, f->focus, FE_CHANGE);
	} else if (d && (d->kind == FW_SPIN || d->kind == FW_SLIDER || d->kind == FW_CHOICE) &&
	    (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT || k == KEY_PGUP || k == KEY_PGDN)) {
		long step = (k == KEY_PGUP || k == KEY_PGDN) ? 10 : 1;
		int up = k == KEY_UP || k == KEY_RIGHT || k == KEY_PGUP;
		if (d->kind == FW_CHOICE && (k == KEY_UP || k == KEY_DOWN))
			form_next_focus(f, k == KEY_DOWN ? 1 : -1);
		else
			form_step(f, f->focus, up ? step : -step);
	} else if (d && d->kind == FW_LIST && (k == KEY_UP || k == KEY_DOWN)) {
		long v = d->val + (k == KEY_DOWN ? 1 : -1);
		if (v >= 0 && v < d->nitems) {
			d->val = v;
			form_emit(f, f->focus, FE_CHANGE);
		}
	} else if (d && d->kind == FW_ICON && (k == KEY_UP || k == KEY_DOWN || k == KEY_LEFT || k == KEY_RIGHT)) {
		form_icon_move(f, k == KEY_LEFT ? -1 : k == KEY_RIGHT ? 1 : 0, k == KEY_UP ? -1 : k == KEY_DOWN ? 1 : 0);
	} else if (k == KEY_UP || k == KEY_LEFT) {
		form_next_focus(f, -1);
	} else if (k == KEY_DOWN || k == KEY_RIGHT) {
		form_next_focus(f, 1);
	} else if (k == ' ' && d && d->kind == FW_CHECK) {
		d->val = !d->val;
		form_emit(f, f->focus, FE_CHANGE);
	} else if (k == ' ' && d && (d->kind == FW_BUTTON || d->kind == FW_SWATCH)) {
		form_emit(f, f->focus, FE_CLICK);
	} else if (k == '\n') {
		if (d && d->kind == FW_SPIN) {
			d->val = MAX(d->min, MIN(d->val, d->max));
			d->typing = 0;
		}
		if (d && (d->kind == FW_BUTTON || d->kind == FW_SWATCH)) {
			form_emit(f, f->focus, FE_CLICK);
		} else if (d && (d->kind == FW_ICON || d->kind == FW_LIST)) {
			form_emit(f, f->focus, FE_DBLCLICK);
		} else {
			for (int i = 0; i < f->n; i++)
				if (f->wd[i].def && !f->wd[i].disabled) {
					form_emit(f, i, FE_CLICK);
					break;
				}
		}
	}
	if (d && f->focus >= 0 && d != &f->wd[f->focus] && d->kind == FW_SPIN) {
		d->val = MAX(d->min, MIN(d->val, d->max));	/* left a spin: tidy what was typed */
		d->typing = 0;
	}
	form_settle(f);
}

static void slider_set_from_x(struct form *f, int i, int x)
{
	struct fwidget *d = &f->wd[i];
	long span = MAX(1, d->max - d->min);
	long v = d->min + ((long)(x - d->x - 6) * span + (d->w - 12) / 2) / MAX(1, d->w - 12);
	v = MAX(d->min, MIN(v, d->max));
	if (v != d->val) {
		d->val = v;
		form_emit(f, i, FE_CHANGE);
	}
}

static void form_click(struct wm *wm, struct win *w, int x, int y, int dbl)
{
	struct form *f = w->form;
	if (!f || f->dead)
		return;
	int hit = -1;
	for (int i = 0; i < f->n; i++) {
		struct fwidget *d = &f->wd[i];
		if (d->hidden || d->kind == FW_LABEL || d->kind == FW_GROUP)
			continue;
		int pad = d->kind == FW_SWATCH ? 3 : 0;
		if (x >= d->x - pad && x < d->x + d->w + pad && y >= d->y - pad && y < d->y + d->h + pad)
			hit = i;
	}
	if (hit < 0 || f->wd[hit].disabled) {
		form_settle(f);
		return;
	}
	struct fwidget *d = &f->wd[hit];
	if (f->focus >= 0 && f->focus != hit && f->wd[f->focus].kind == FW_SPIN) {
		struct fwidget *o = &f->wd[f->focus];
		o->val = MAX(o->min, MIN(o->val, o->max));
		o->typing = 0;
	}
	if (focusable(d))
		f->focus = hit;
	f->px = x - d->x;
	f->py = y - d->y;
	switch (d->kind) {
	case FW_BUTTON:
	case FW_SWATCH:
		form_emit(f, hit, FE_CLICK);
		break;
	case FW_CHECK:
		d->val = !d->val;
		form_emit(f, hit, FE_CHANGE);
		break;
	case FW_SPIN:
		d->typing = 0;
		if (f->px >= d->w - 16)
			form_step(f, hit, f->py < d->h / 2 ? 1 : -1);
		break;
	case FW_CHOICE:
		if (f->px < 18)
			form_step(f, hit, -1);
		else if (f->px >= d->w - 18)
			form_step(f, hit, 1);
		break;
	case FW_SLIDER:
		slider_set_from_x(f, hit, x);
		f->drag = hit;
		wm->capture = w;
		break;
	case FW_ICON:
		form_emit(f, hit, dbl ? FE_DBLCLICK : FE_CLICK);
		break;
	case FW_LIST: {
		int row = d->top + (f->py - 2) / 18;
		if (row >= 0 && row < d->nitems) {
			if (row != d->val) {
				d->val = row;
				form_emit(f, hit, FE_CHANGE);
			}
			if (dbl)
				form_emit(f, hit, FE_DBLCLICK);
		}
		break;
	}
	case FW_CUSTOM:
		form_emit(f, hit, dbl ? FE_DBLCLICK : FE_CLICK);
		break;
	}
	form_settle(f);
}

static void form_drag(struct win *w, int x, int y)
{
	(void)y;
	struct form *f = w->form;
	if (!f || f->dead || f->drag < 0)
		return;
	slider_set_from_x(f, f->drag, x);
	form_settle(f);
}

static void form_tick_all(struct wm *wm)
{
	uint64_t now = uptime_ms();
	for (struct win *w = wm->wins; w; ) {
		struct win *next = w->next;
		struct form *f = w->form;
		if (f && !f->dead && f->tick_ms && now >= f->next_tick) {
			f->next_tick = now + (uint64_t)f->tick_ms;
			form_emit(f, -1, FE_TICK);
			form_settle(f);
		}
		w = next;
	}
}

/* ---- for applets: desktop services ---- */

void wm_launch(struct wm *wm, const char *label, const char *cmd) { launch_cmd(wm, label, cmd); }
int wm_depth(struct wm *wm) { return wm->depth; }
uint32_t wm_desktop_colour(struct wm *wm) { return wm->desktop; }

void wm_set_desktop_colour(struct wm *wm, uint32_t rgb)
{
	wm->desktop = rgb & 0xFFFFFF;
	wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
}

void wm_message(struct wm *wm, const char *title, const char *msg)
{
	struct win *w = open_dialog(wm, title, msg, 0, 0);
	if (w)
		render_dialog(w);
}

uint32_t ui_colour(int which)
{
	return which == 0 ? theme.face : which == 1 ? theme.active : theme.select;
}

/* ---- Program Manager ------------------------------------------------------------------ */

static const char *const menu_names[] = { "File", "Options", "Window", "Help" };
static const char *const menu_items[4][5] = {
	{ "New Shell", "Run...", "File Manager", "Exit Desktop", NULL },
	{ "Control Panel", "Desktop...", "Colours...", "Date & Time...", NULL },
	{ "Tile", "Cascade", "Next Window", "Clock", NULL },
	{ "About bdos", "Keyboard", NULL, NULL, NULL },
};

static const char *guess_icon(const char *cmd)
{
	const char *dot = strrchr(cmd, '.');
	if (!*cmd)
		return "prompt";
	if (!strncasecmp(cmd, "EDIT", 4))
		return "edit";
	if (!strncasecmp(cmd, "WIN ", 4))
		return "network";
	if (!strcasecmp(cmd, "@files"))
		return "folder";
	if (!strncasecmp(cmd, "@control", 8))
		return "control";
	if (!strcasecmp(cmd, "@clock"))
		return "clock";
	if (dot && !strncasecmp(dot, ".BAS", 4))
		return "basic";
	if (!strncasecmp(cmd, "BASIC", 5))
		return "basic";
	return "prompt";
}

static int pm_load_cb(void *arg, struct vstat *st)
{
	struct group *g = arg;
	if (st->type != VFS_FILE || g->nitems >= (int)NELEM(g->items))
		return 0;
	struct item *it = &g->items[g->nitems++];
	strncpy0(it->label, st->name, sizeof it->label);
	char *dot = strrchr(it->label, '.');
	if (dot && dot != it->label)
		*dot = 0;
	return 0;
}

static int pm_groups_cb(void *arg, struct vstat *st)
{
	struct progman *pm = arg;
	if (st->type == VFS_DIR && pm->ngroups < (int)NELEM(pm->groups))
		strncpy0(pm->groups[pm->ngroups++].name, st->name, sizeof pm->groups[0].name);
	return 0;
}

static void pm_default(struct progman *pm)
{
	static const struct { const char *g, *label, *cmd, *icon; } defs[] = {
		{ "Main", "Shell", "", "prompt" },
		{ "Main", "File Manager", "@files", "folder" },
		{ "Main", "BASIC", "BASIC", "basic" },
	};
	pm->ngroups = 1;
	strcpy(pm->groups[0].name, "Main");
	for (size_t i = 0; i < NELEM(defs); i++) {
		struct item *it = &pm->groups[0].items[pm->groups[0].nitems++];
		strncpy0(it->label, defs[i].label, sizeof it->label);
		strncpy0(it->cmd, defs[i].cmd, sizeof it->cmd);
		strncpy0(it->icon, defs[i].icon, sizeof it->icon);
	}
}

/* Groups are directories in C:\PROGMAN; items are files whose first line is
 * the command. An "icon" extended attribute picks the picture. */
static void pm_load(struct progman *pm)
{
	pm->ngroups = 0;
	if (vfs_list("C:\\PROGMAN", pm_groups_cb, pm) || !pm->ngroups) {
		pm_default(pm);
		return;
	}
	for (int g = 0; g < pm->ngroups; g++) {
		char dir[VFS_PATH_MAX];
		snprintf(dir, sizeof dir, "C:\\PROGMAN\\%s", pm->groups[g].name);
		vfs_list(dir, pm_load_cb, &pm->groups[g]);
		for (int i = 0; i < pm->groups[g].nitems; i++) {
			struct item *it = &pm->groups[g].items[i];
			/* find the file again by label (any extension) */
			char pat[VFS_PATH_MAX];
			struct vstat st;
			const char *exts[] = { ".LNK", "" };
			for (size_t e = 0; e < NELEM(exts); e++) {
				snprintf(pat, sizeof pat, "%s\\%s%s", dir, it->label, exts[e]);
				if (vfs_stat(pat, &st))
					continue;
				size_t len;
				int err;
				char *data = vfs_slurp(pat, &len, &err);
				if (data) {
					char *nl = strchr(data, '\n');
					if (nl)
						*nl = 0;
					size_t l = strlen(data);
					while (l && (data[l - 1] == '\r' || data[l - 1] == ' '))
						data[--l] = 0;
					strncpy0(it->cmd, data, sizeof it->cmd);
					kfree(data);
				}
				if (vfs_xattr_get(pat, "icon", it->icon, sizeof it->icon) < 0 ||
				    !icon_exists(it->icon))
					strncpy0(it->icon, guess_icon(it->cmd), sizeof it->icon);
				char ord[8];
				it->order = vfs_xattr_get(pat, "order", ord, sizeof ord) > 0 ?
				    (int)strtol(ord, NULL, 10) : 50;
				break;
			}
		}
		/* items: by "order" attribute, then by name */
		struct group *gr = &pm->groups[g];
		for (int i = 1; i < gr->nitems; i++) {
			struct item key = gr->items[i];
			int j = i - 1;
			while (j >= 0 && (gr->items[j].order > key.order ||
			    (gr->items[j].order == key.order && strcasecmp(gr->items[j].label, key.label) > 0))) {
				gr->items[j + 1] = gr->items[j];
				j--;
			}
			gr->items[j + 1] = key;
		}
	}
	/* groups: Main first, then by name */
	for (int i = 1; i < pm->ngroups; i++) {
		struct group key = pm->groups[i];
		int j = i - 1;
		while (j >= 0 && (strcasecmp(key.name, "Main") == 0 ||
		    (strcasecmp(pm->groups[j].name, "Main") && strcasecmp(pm->groups[j].name, key.name) > 0))) {
			pm->groups[j + 1] = pm->groups[j];
			j--;
		}
		pm->groups[j + 1] = key;
	}
}

#define CELL_W 84
#define CELL_H 70

static void pm_layout(struct win *w)
{
	struct progman *pm = w->pm;
	int cw = w->content.w;
	int y = MENU_H + 8, x = 8;
	int colw = (cw - 24) / 2;
	int col_y[2] = { y, y };
	for (int g = 0; g < pm->ngroups; g++) {
		int c = col_y[0] <= col_y[1] ? 0 : 1;
		int per_row = MAX(1, (colw - 8) / CELL_W);
		int rows = MAX(1, (pm->groups[g].nitems + per_row - 1) / per_row);
		int gh = TITLE_H + 6 + rows * CELL_H;
		pm->groups[g].r = (struct rect){ x + c * (colw + 8), col_y[c], colw, gh };
		col_y[c] += gh + 8;
	}
}

static void render_progman(struct win *w)
{
	struct progman *pm = w->pm;
	struct surface *s = &w->content;
	fill(s, 0, 0, s->w, s->h, P(0xE8E8E8));
	/* menu bar */
	fill(s, 0, 0, s->w, MENU_H, P(theme.face));
	hline(s, 0, MENU_H - 1, s->w, P(0x808080));
	for (int i = 0, x = 6; i < 4; i++) {
		int wd = (int)strlen(menu_names[i]) * FONT_W + 12;
		if (pm->menu_open == i) {
			fill(s, x, 1, wd, MENU_H - 2, P(theme.select));
			text(s, x + 6, 2, menu_names[i], P(0xFFFFFF));
		} else {
			text(s, x + 6, 2, menu_names[i], P(theme.menu_text));
		}
		x += wd;
	}
	for (int g = 0; g < pm->ngroups; g++) {
		struct group *gr = &pm->groups[g];
		struct rect r = gr->r;
		fill(s, r.x, r.y, r.w, r.h, P(theme.face));
		bevel(s, r.x, r.y, r.w, r.h, 1);
		fill(s, r.x + 3, r.y + 3, r.w - 6, TITLE_H - 2, P(g == pm->sel_g ? theme.active : theme.inactive));
		text(s, r.x + (r.w - (int)strlen(gr->name) * FONT_W) / 2, r.y + 4, gr->name, P(0xFFFFFF));
		fill(s, r.x + 3, r.y + TITLE_H + 1, r.w - 6, r.h - TITLE_H - 4, P(0xFFFFFF));
		int per_row = MAX(1, (r.w - 8) / CELL_W);
		for (int i = 0; i < gr->nitems; i++) {
			int cx = r.x + 6 + (i % per_row) * CELL_W;
			int cy = r.y + TITLE_H + 4 + (i / per_row) * CELL_H;
			draw_icon(s, cx + (CELL_W - 32) / 2, cy + 2, gr->items[i].icon);
			/* label: one or two lines of up to 10 characters */
			char l1[12] = "", l2[12] = "";
			const char *lab = gr->items[i].label;
			size_t len = strlen(lab);
			if (len <= 10) {
				strncpy0(l1, lab, sizeof l1);
			} else {
				int brk = -1;
				for (int k = 0; k <= 10 && lab[k]; k++)
					if (lab[k] == ' ')
						brk = k;
				if (brk > 0) {
					memcpy(l1, lab, (size_t)brk);
					l1[brk] = 0;
					strncpy0(l2, lab + brk + 1, sizeof l2);
				} else {
					strncpy0(l1, lab, sizeof l1);
				}
			}
			int sel = g == pm->sel_g && i == pm->sel_i;
			for (int k = 0; k < 2; k++) {
				const char *ln = k ? l2 : l1;
				if (!*ln)
					continue;
				int lw = (int)strlen(ln) * FONT_W;
				if (sel)
					fill(s, cx + (CELL_W - lw) / 2 - 2, cy + 36 + k * 15, lw + 4, 16, P(theme.select));
				text(s, cx + (CELL_W - lw) / 2, cy + 36 + k * 15, ln, P(sel ? 0xFFFFFF : 0));
			}
		}
	}
	/* the open menu */
	if (pm->menu_open >= 0) {
		int x = 6;
		for (int i = 0; i < pm->menu_open; i++)
			x += (int)strlen(menu_names[i]) * FONT_W + 12;
		int n = 0, wmax = 0;
		while (n < 5 && menu_items[pm->menu_open][n]) {
			wmax = MAX(wmax, (int)strlen(menu_items[pm->menu_open][n]));
			n++;
		}
		int mw = wmax * FONT_W + 24, mh = n * 20 + 6;
		fill(s, x, MENU_H, mw, mh, P(theme.face));
		bevel(s, x, MENU_H, mw, mh, 1);
		for (int i = 0; i < n; i++)
			text(s, x + 12, MENU_H + 4 + i * 20, menu_items[pm->menu_open][i], P(0));
	}
	win_dirty_all(w);
}

static void about(struct wm *wm)
{
	char msg[200];
	char mem[32];
	fmt_commas(mem, sizeof mem, pmm_free_bytes() / 1024);
	snprintf(msg, sizeof msg, "%s %s - %s\n\nA 64-bit DOS with a Plan 9 heart.\nDesktop level %d. %s K free.",
	    BDOS_NAME, BDOS_VERSION, BDOS_LONGNAME, wm->depth, mem);
	open_dialog(wm, "About bdos", msg, 0, 0);
}

static void tile(struct wm *wm, int cascade)
{
	int n = 0;
	for (struct win *w = wm->wins; w; w = w->next)
		if (w != wm->progman && w->kind != W_DIALOG)
			n++;
	int i = 0, sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	for (struct win *w = wm->wins; w; w = w->next) {
		if (w == wm->progman || w->kind == W_DIALOG)
			continue;
		wm_dirty(wm, frame_rect(w));
		if (cascade) {
			w->x = 20 + i * 26;
			w->y = 20 + i * 26;
		} else {
			int cols = n > 1 ? 2 : 1, rws = (n + cols - 1) / cols;
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

static void next_window(struct wm *wm)
{
	struct win *first = wm->wins;
	if (first && first != topmost(wm))
		raise_win(wm, first);
}

static void menu_pick(struct wm *wm, int m, int i)
{
	switch (m * 10 + i) {
	case 0: open_term(wm, "Shell", NULL, 80, 25, -1, -1); break;
	case 1: open_dialog(wm, "Run", "Command line:", 1, 1); break;
	case 2: launch_cmd(wm, "File Manager", "@files"); break;
	case 3:
		open_dialog(wm, "Exit Desktop",
		    wm->depth ? "Leave this nested desktop?" : "Leave the desktop and go back to DOS?", 0, 2);
		break;
	case 10: control_open(wm, NULL); break;
	case 11: control_open(wm, "desktop"); break;
	case 12: control_open(wm, "colors"); break;
	case 13: control_open(wm, "date"); break;
	case 23: clock_open(wm); break;
	case 20: tile(wm, 0); break;
	case 21: tile(wm, 1); break;
	case 22: next_window(wm); break;
	case 30: about(wm); break;
	case 31:
		open_dialog(wm, "Keyboard",
		    "Alt+N  new shell        Alt+W  next window\nAlt+Q  close window     Alt+X  exit desktop\n"
		    "Arrows/Tab/Enter work in Program Manager.", 0, 0);
		break;
	}
}

static void pm_launch(struct wm *wm, struct progman *pm)
{
	if (pm->sel_g >= pm->ngroups || pm->sel_i >= pm->groups[pm->sel_g].nitems)
		return;
	struct item *it = &pm->groups[pm->sel_g].items[pm->sel_i];
	launch_cmd(wm, it->label, it->cmd);
}

static void pm_click(struct wm *wm, struct win *w, int x, int y, int dbl)
{
	struct progman *pm = w->pm;
	if (pm->menu_open >= 0) {
		int mx = 6;
		for (int i = 0; i < pm->menu_open; i++)
			mx += (int)strlen(menu_names[i]) * FONT_W + 12;
		int m = pm->menu_open;
		pm->menu_open = -1;
		if (y > MENU_H && x >= mx && x < mx + 200) {
			int i = (y - MENU_H - 3) / 20;
			int n = 0;
			while (n < 5 && menu_items[m][n])
				n++;
			if (i >= 0 && i < n) {
				render_progman(w);
				menu_pick(wm, m, i);
				return;
			}
		}
		render_progman(w);
		if (y >= MENU_H)
			return;
	}
	if (y < MENU_H) {
		for (int i = 0, mx = 6; i < 4; i++) {
			int wd = (int)strlen(menu_names[i]) * FONT_W + 12;
			if (x >= mx && x < mx + wd) {
				pm->menu_open = i;
				break;
			}
			mx += wd;
		}
		render_progman(w);
		return;
	}
	for (int g = 0; g < pm->ngroups; g++) {
		struct rect r = pm->groups[g].r;
		if (!rect_has(r, x, y))
			continue;
		pm->sel_g = g;
		int per_row = MAX(1, (r.w - 8) / CELL_W);
		int cx = (x - r.x - 6) / CELL_W, cy = (y - r.y - TITLE_H - 4) / CELL_H;
		int i = cy * per_row + cx;
		if (y > r.y + TITLE_H && cx < per_row && i >= 0 && i < pm->groups[g].nitems) {
			pm->sel_i = i;
			render_progman(w);
			if (dbl)
				pm_launch(wm, pm);
			return;
		}
		render_progman(w);
	}
}

static void pm_key(struct wm *wm, struct win *w, int k)
{
	struct progman *pm = w->pm;
	if (!pm->ngroups)
		return;
	struct group *g = &pm->groups[pm->sel_g];
	int per_row = MAX(1, (g->r.w - 8) / CELL_W);
	if (pm->menu_open >= 0 && k == 27) {
		pm->menu_open = -1;
	} else if (k == '\t') {
		pm->sel_g = (pm->sel_g + 1) % pm->ngroups;
		pm->sel_i = 0;
	} else if (k == KEY_RIGHT && pm->sel_i + 1 < g->nitems) {
		pm->sel_i++;
	} else if (k == KEY_LEFT && pm->sel_i > 0) {
		pm->sel_i--;
	} else if (k == KEY_DOWN && pm->sel_i + per_row < g->nitems) {
		pm->sel_i += per_row;
	} else if (k == KEY_UP && pm->sel_i - per_row >= 0) {
		pm->sel_i -= per_row;
	} else if (k == '\n') {
		pm_launch(wm, pm);
		return;
	}
	render_progman(w);
}

static struct win *open_progman(struct wm *wm)
{
	struct progman *pm = kzalloc(sizeof *pm);
	if (!pm)
		return NULL;
	pm->menu_open = -1;
	pm_load(pm);
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	int cw = MIN(600, sw - 60), ch = MIN(440, sh - 100);
	struct win *w = win_new(wm, W_PROGMAN, wm->title, 20, 16, cw, ch);
	if (!w) {
		kfree(pm);
		return NULL;
	}
	w->pm = pm;
	w->resizable = 1;
	pm_layout(w);
	render_progman(w);
	wm->progman = w;
	return w;
}

/* ---- File Manager -------------------------------------------------------------------- */

static int fm_cb(void *arg, struct vstat *st)
{
	struct files *f = arg;
	struct vstat *n = krealloc(f->ents, sizeof(struct vstat) * (size_t)(f->n + 1));
	if (!n)
		return 1;
	f->ents = n;
	f->ents[f->n++] = *st;
	return 0;
}

static void fm_load(struct files *f)
{
	kfree(f->ents);
	f->ents = NULL;
	f->n = 0;
	struct vstat up;
	memset(&up, 0, sizeof up);
	up.type = VFS_DIR;
	strcpy(up.name, "..");
	if (strlen(f->dir) > 3)
		fm_cb(f, &up);
	vfs_list(f->dir, fm_cb, f);
	/* directories first, then names */
	for (int i = 1; i < f->n; i++) {
		struct vstat k = f->ents[i];
		int j = i - 1;
		while (j >= 0 && strcmp(f->ents[j].name, "..") &&
		    ((f->ents[j].type != VFS_DIR && k.type == VFS_DIR) ||
		    (f->ents[j].type == k.type && strcasecmp(f->ents[j].name, k.name) > 0))) {
			f->ents[j + 1] = f->ents[j];
			j--;
		}
		f->ents[j + 1] = k;
	}
	f->sel = 0;
	f->top = 0;
}

#define FM_ROW 20
#define FM_TOP 26

static void render_files(struct win *w)
{
	struct files *f = w->fm;
	struct surface *s = &w->content;
	fill(s, 0, 0, s->w, s->h, P(0xFFFFFF));
	fill(s, 0, 0, s->w, FM_TOP - 2, P(theme.face));
	text(s, 6, 4, f->dir, P(0));
	hline(s, 0, FM_TOP - 2, s->w, P(0x808080));
	int rows = (s->h - FM_TOP) / FM_ROW;
	if (f->sel < f->top)
		f->top = f->sel;
	if (f->sel >= f->top + rows)
		f->top = f->sel - rows + 1;
	for (int i = f->top; i < f->n && i < f->top + rows; i++) {
		struct vstat *e = &f->ents[i];
		int y = FM_TOP + (i - f->top) * FM_ROW;
		int sel = i == f->sel;
		if (sel)
			fill(s, 0, y, s->w, FM_ROW, P(theme.select));
		uint32_t fg = P(sel ? 0xFFFFFF : 0);
		/* tiny folder / page marks */
		if (e->type == VFS_DIR) {
			fill(s, 8, y + 6, 14, 10, P(0xE8C050));
			outline(s, 8, y + 6, 14, 10, P(0));
		} else {
			fill(s, 10, y + 3, 10, 14, P(0xFFFFFF));
			outline(s, 10, y + 3, 10, 14, P(0));
		}
		char name[48];
		strncpy0(name, e->name, sizeof name);
		text(s, 30, y + 2, name, fg);
		if (e->type == VFS_FILE) {
			char sz[24];
			fmt_commas(sz, sizeof sz, e->size);
			text(s, s->w - 10 - (int)strlen(sz) * FONT_W, y + 2, sz, fg);
		}
	}
	win_dirty_all(w);
}

static void fm_open(struct wm *wm, struct win *w)
{
	struct files *f = w->fm;
	if (f->sel >= f->n)
		return;
	struct vstat *e = &f->ents[f->sel];
	char path[VFS_PATH_MAX];
	if (!strcmp(e->name, "..")) {
		vfs_dirname(f->dir, path, sizeof path);
	} else {
		vfs_join(f->dir, e->name, path, sizeof path);
	}
	if (e->type == VFS_DIR) {
		strncpy0(f->dir, path, sizeof f->dir);
		fm_load(f);
		snprintf(w->title, sizeof w->title, "Files - %s", f->dir);
		render_files(w);
		return;
	}
	const char *dot = strrchr(e->name, '.');
	char cmd[VFS_PATH_MAX + 16];
	if (dot && (!strcasecmp(dot, ".BAS") || !strcasecmp(dot, ".BAT") || !strcasecmp(dot, ".COM") ||
	    !strcasecmp(dot, ".C")))
		snprintf(cmd, sizeof cmd, "\"%s\"", path);
	else
		snprintf(cmd, sizeof cmd, "EDIT \"%s\"", path);
	launch_cmd(wm, e->name, cmd);
}

struct win *open_files(struct wm *wm, const char *dir)
{
	struct files *f = kzalloc(sizeof *f);
	if (!f)
		return NULL;
	strncpy0(f->dir, dir, sizeof f->dir);
	fm_load(f);
	char title[64];
	snprintf(title, sizeof title, "Files - %s", dir);
	struct win *w = win_new(wm, W_FILES, title, -1, -1, 420, 360);
	if (!w) {
		kfree(f->ents);
		kfree(f);
		return NULL;
	}
	w->fm = f;
	w->resizable = 1;
	render_files(w);
	return w;
}

static void fm_key(struct wm *wm, struct win *w, int k)
{
	struct files *f = w->fm;
	if (k == KEY_UP && f->sel > 0)
		f->sel--;
	else if (k == KEY_DOWN && f->sel + 1 < f->n)
		f->sel++;
	else if (k == '\n') {
		fm_open(wm, w);
		return;
	} else if (k == '\b' && strlen(f->dir) > 3) {
		f->sel = 0;	/* ".." */
		fm_open(wm, w);
		return;
	}
	render_files(w);
}

static void fm_click(struct wm *wm, struct win *w, int x, int y, int dbl)
{
	(void)x;
	struct files *f = w->fm;
	if (y < FM_TOP)
		return;
	int i = f->top + (y - FM_TOP) / FM_ROW;
	if (i >= f->n)
		return;
	f->sel = i;
	render_files(w);
	if (dbl)
		fm_open(wm, w);
}

/* ---- resizing and maximizing ------------------------------------------------------------- */

static int win_min_cw(struct win *w)
{
	if (w->kind == W_TERM)
		return 20 * FONT_W;
	if (w->form)
		return MAX(80, w->form->min_w);
	return 180;
}

static int win_min_ch(struct win *w)
{
	if (w->kind == W_TERM)
		return 4 * FONT_H;
	if (w->form)
		return MAX(40, w->form->min_h);
	return w->kind == W_PROGMAN ? 120 : 80;
}

static int win_can_resize(struct win *w)
{
	return w->resizable && !(w->term && w->term->child);
}

/* Give a window a new frame. Terminals snap to whole character cells and
 * keep the text around the cursor; everything else redraws at the new size. */
static void win_resize(struct win *w, int x, int y, int fw, int fh)
{
	struct wm *wm = w->wm;
	int cw = MAX(win_min_cw(w), fw - 2 * BORDER);
	int ch = MAX(win_min_ch(w), fh - 2 * BORDER - TITLE_H);
	struct termwin *t = w->kind == W_TERM ? w->term : NULL;
	int cols = 0, rows = 0;
	if (t) {
		cols = MIN(cw / FONT_W, 250);
		rows = MIN(ch / FONT_H, 120);
		cw = cols * FONT_W;
		ch = rows * FONT_H;
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
	w->w = FRAME_W(cw);
	w->h = FRAME_H(ch);
	wm_dirty(wm, frame_rect(w));
	if (t) {
		for (int i = 0; i < cols * rows; i++)
			grid[i] = 0x0720;
		int shift = MAX(0, t->cy - (rows - 1));
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
		return;
	}
	switch (w->kind) {
	case W_PROGMAN:
		pm_layout(w);
		render_progman(w);
		break;
	case W_FILES:
		render_files(w);
		break;
	case W_CANVAS: {
		struct event e = { EV_RESIZE, cw, ch, 0, 0 };
		evq_push(&w->events, &e);
		win_dirty_all(w);
		break;
	}
	case W_FORM:
		form_emit(w->form, -1, FE_RESIZE);
		form_settle(w->form);
		break;
	}
}

static void toggle_maximize(struct wm *wm, struct win *w)
{
	if (!win_can_resize(w))
		return;
	if (w->maximized) {
		w->maximized = 0;
		win_resize(w, w->restore.x, w->restore.y, w->restore.w, w->restore.h);
	} else {
		w->restore = frame_rect(w);
		w->maximized = 1;
		win_resize(w, 0, 0, wm->disp->surf->w, wm->disp->surf->h);
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
	int e = 0, corner = 18;
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
	int maxw = w->w - 2 * BORDER - (w->resizable ? 70 : 50);
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
	if (win_can_resize(w)) {
		/* maximize/restore box, and the notches that mark the resize corners */
		int mx = bx - 18;
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

static void compose(struct wm *wm, struct rect r)
{
	struct surface *s = wm->disp->surf;
	r = rect_isect(r, (struct rect){ 0, 0, s->w, s->h });
	if (rect_empty(r))
		return;
	surf_clip(s, r);
	fill(s, r.x, r.y, r.w, r.h, P(wm->desktop));
	/* a quiet watermark */
	const char *mark = wm->depth ? "bdos (nested)" : "bdos";
	text(s, s->w - (int)strlen(mark) * FONT_W - 12, s->h - 24, mark, P(0xB0C8C8));
	for (struct win *w = wm->wins; w; w = w->next) {
		struct rect fr = frame_rect(w);
		if (rect_empty(rect_isect(fr, r)))
			continue;
		draw_frame(s, w, w == wm->focus);
		blit(s, w->x + CLIENT_X, w->y + CLIENT_Y, &w->content, 0, 0, w->content.w, w->content.h);
	}
	if (wm->resizing) {
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
		if (rect_has(frame_rect(w), x, y))
			hit = w;
	return hit;
}

static void close_request(struct wm *wm, struct win *w)
{
	if (w == wm->progman) {
		menu_pick(wm, 0, 3);	/* "Exit Desktop?" */
		return;
	}
	if (w->kind == W_DIALOG) {
		dialog_finish(wm, w, 2);
		return;
	}
	win_destroy(wm, w);
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

static void handle_key(struct wm *wm, int k)
{
	/* desktop-wide keys */
	if (k == (KEY_ALT | 'n')) {
		open_term(wm, "Shell", NULL, 80, 25, -1, -1);
		return;
	}
	if (k == (KEY_ALT | 'w')) {
		next_window(wm);
		return;
	}
	if (k == (KEY_ALT | 'x')) {
		menu_pick(wm, 0, 3);
		return;
	}
	struct win *w = wm->focus;
	if (!w)
		return;
	if (k == (KEY_ALT | 'q')) {
		close_request(wm, w);
		return;
	}
	struct event e = { EV_KEY, 0, 0, 0, k };
	switch (w->kind) {
	case W_PROGMAN: pm_key(wm, w, k); break;
	case W_FILES: fm_key(wm, w, k); break;
	case W_DIALOG: dialog_key(wm, w, k); break;
	case W_FORM: form_key(wm, w, k); break;
	default: send_to_window(w, &e); break;
	}
}

static struct rect ptr_rect(struct wm *wm)
{
	return (struct rect){ wm->mx - 10, wm->my - 10, PTR_W + 12, PTR_H + 12 };
}

static void update_pointer_shape(struct wm *wm, struct win *w)
{
	if (!wm->disp->draws_pointer)
		return;
	int shape = wm->resizing ? wm->rs_edges : (wm->drag || wm->capture) ? 0 : edge_at(w, wm->mx, wm->my);
	if (shape != wm->ptr) {
		wm->ptr = shape;
		wm_dirty(wm, ptr_rect(wm));
	}
}

static void handle_mouse(struct wm *wm, struct event *e)
{
	int x = e->x, y = e->y;
	if (wm->disp->draws_pointer && (x != wm->mx || y != wm->my)) {
		wm_dirty(wm, ptr_rect(wm));
		wm->mx = x;
		wm->my = y;
		wm_dirty(wm, ptr_rect(wm));
	}
	wm->mx = x;
	wm->my = y;
	int down = e->type == EV_DOWN && (e->buttons & 1);
	wm->buttons = e->buttons;

	if (wm->capture) {	/* a slider follows the mouse until release */
		struct win *c = wm->capture;
		if (!(e->buttons & 1)) {
			if (c->form)
				c->form->drag = -1;
			wm->capture = NULL;
		} else {
			form_drag(c, x - c->x - CLIENT_X, y - c->y - CLIENT_Y);
		}
		return;
	}
	if (wm->resizing) {
		if (!(e->buttons & 1)) {
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
		if (!(e->buttons & 1)) {
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

	struct win *w = win_at(wm, x, y);
	update_pointer_shape(wm, w);
	if (down) {
		uint64_t now = uptime_ms();
		int dbl = now - wm->click_ms < (uint64_t)DCLICK_MS && x - wm->click_x < 5 &&
		    wm->click_x - x < 5 && y - wm->click_y < 5 && wm->click_y - y < 5;
		wm->click_ms = dbl ? 0 : now;
		wm->click_x = x;
		wm->click_y = y;
		if (!w)
			return;
		if (w != wm->focus || w != topmost(wm))
			raise_win(wm, w);
		int lx = x - w->x, ly = y - w->y;
		int on_bar = ly >= BORDER && ly < CLIENT_Y;
		if (on_bar && lx >= w->w - BORDER - 18 && lx < w->w - BORDER) {
			close_request(wm, w);
			return;
		}
		if (on_bar && win_can_resize(w) && lx >= w->w - BORDER - 36 && lx < w->w - BORDER - 20) {
			toggle_maximize(wm, w);
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
			if (lx >= BORDER + 2 && lx < BORDER + 18 && dbl)
				close_request(wm, w);	/* double-click the control box */
			else if (dbl && win_can_resize(w))
				toggle_maximize(wm, w);	/* double-click the title */
			else {
				wm->drag = w;
				wm->drag_dx = x - w->x;
				wm->drag_dy = y - w->y;
			}
			return;
		}
		int cx = lx - CLIENT_X, cy = ly - CLIENT_Y;
		switch (w->kind) {
		case W_PROGMAN: pm_click(wm, w, cx, cy, dbl); return;
		case W_FILES: fm_click(wm, w, cx, cy, dbl); return;
		case W_DIALOG: dialog_click(wm, w, cx, cy); return;
		case W_FORM: form_click(wm, w, cx, cy, dbl); return;
		}
	}
	/* motion and buttons inside canvases and nested desktops */
	struct win *target = (e->type == EV_MOVE) ? w : wm->focus;
	if (target && (target->kind == W_CANVAS || (target->term && target->term->child))) {
		struct event le = *e;
		le.x = x - target->x - CLIENT_X;
		le.y = y - target->y - CLIENT_Y;
		if (le.x >= 0 && le.y >= 0 && le.x < target->content.w && le.y < target->content.h)
			send_to_window(target, &le);
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

static void wm_loop(struct wm *wm)
{
	struct surface *s = wm->disp->surf;
	wm_dirty(wm, (struct rect){ 0, 0, s->w, s->h });
	while (wm->running && wm->disp->alive) {
		struct event e;
		int busy = 0;
		while (wm->disp->next_event(wm->disp, &e)) {
			busy = 1;
			if (e.type == EV_KEY)
				handle_key(wm, e.key);
			else if (e.type == EV_CLOSE)
				wm->running = 0;
			else
				handle_mouse(wm, &e);
		}
		check_terms(wm);
		form_tick_all(wm);
		if (!wm->progman && wm->running)
			wm->running = 0;	/* Program Manager closed: leave */
		if (!rect_empty(wm->dirty)) {
			struct rect r = wm->dirty;
			wm->dirty = (struct rect){ 0, 0, 0, 0 };
			compose(wm, r);
			busy = 1;
		}
		if (!busy)
			task_idle();
		else
			task_yield();
	}
}

/* Close everything; wait (briefly) for shells to notice. */
static void wm_shutdown(struct wm *wm)
{
	while (wm->wins)
		win_destroy(wm, wm->wins);
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
	wm->next_wm = all_wms;
	all_wms = wm;
	void *save_desk = me->desk;
	me->desk = wm;

	if (d->attach)
		d->attach(d, wm);
	open_progman(wm);
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
	int k = input_getkey();
	if (k < 0)
		k = serial_console()->getkey(serial_console());
	if (k >= 0) {
		*e = (struct event){ EV_KEY, sd->mx, sd->my, sd->buttons, k };
		return 1;
	}
	struct mouse_event m;
	if (input_getmouse(&m)) {
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
		*e = (struct event){ type, sd->mx, sd->my, m.buttons, 0 };
		return 1;
	}
	return 0;
}

struct term_disp {
	struct display d;
	struct termwin *t;
};

static void td_flush(struct display *d, struct rect r)
{
	struct term_disp *td = (struct term_disp *)d;
	if (td->t->win && !td->t->closed)
		win_dirty(td->t->win, r);
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
		memset(&t->child_ev, 0, sizeof t->child_ev);
		return &td->d;
	}
	*err = E_NOTSUP;
	return NULL;
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
	t->child = 0;
	if (!t->closed && t->win) {
		tw_redraw(t);
		win_dirty_all(t->win);
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

static int sys_win_open(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_close(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_clear(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w || !w->wm)
		return w ? 0 : -2;
	fill(&w->content, 0, 0, w->content.w, w->content.h, argcol(a, argc, 1, 0xFFFFFF));
	win_dirty_all(w);
	return 0;
}

static int sys_win_rect(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_line(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_pixel(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_text(int argc, struct sysval *a, struct sysval *r)
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
static int sys_win_event(int argc, struct sysval *a, struct sysval *r)
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

static int sys_win_info(int argc, struct sysval *a, struct sysval *r, int what)
{
	struct win *w = argc ? canvas_get(a[0].n) : NULL;
	if (!w)
		return -2;
	r->n = what == 0 ? w->last_key : what == 1 ? w->last_x : what == 2 ? w->last_y :
	    what == 3 ? w->content.w : w->content.h;
	return 0;
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

static int sys_desk_color(int argc, struct sysval *a, struct sysval *r)
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
	struct win *w = open_dialog(wm, argc > 1 && a[1].is_str ? a[1].s : "Message", a[0].s, 0, 3);
	if (!w)
		return -3;
	render_dialog(w);
	while (!w->dlg->done)
		task_idle();
	if (!w->closed) {	/* still on screen: take it down */
		w->dlg->purpose = 0;
		win_destroy(wm, w);
	} else {
		kfree(w->dlg);
		surf_free(&w->content);
		kfree(w);
	}
	return 0;
}

static int sys_desk_run(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	struct wm *wm = my_desk();
	if (!wm || !argc || !a[0].is_str)
		return -2;
	launch_cmd(wm, a[0].s, a[0].s);
	return 0;
}

static int sys_desk_size(int argc, struct sysval *a, struct sysval *r)
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
		wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
		if (wm->progman)
			render_progman(wm->progman);
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
		int nw = 0;
		for (struct win *w = wm->wins; w; w = w->next)
			nw++;
		len = snprintf(info, sizeof info, "desktop level %d %dx%d windows %d color #%06x\n",
		    wm->depth, wm->disp->surf->w, wm->disp->surf->h, nw, wm->desktop);
		/* one line per window, bottom first: id x y w h focus "title" */
		for (struct win *w = wm->wins; w && len < (int)sizeof info - 100; w = w->next)
			len += snprintf(info + len, sizeof info - (size_t)len, "window %d %d %d %d %d%s \"%s\"\n",
			    w->id, w->x, w->y, w->w, w->h, w == wm->focus ? " focus" : "", w->title);
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
	if (!t->win || t->closed)
		return (long)n;
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
	return (long)n;
}

static struct chardev desk_dev = { "DESK$", desk_read, desk_write, NULL, 1 };
static struct chardev mouse_dev = { "MOUSE$", mouse_read, NULL, NULL, 1 };
static struct chardev draw_dev = { "DRAW$", NULL, draw_write, NULL, 0 };

static void apply_scheme(void)
{
	const char *name = setting("desktop.scheme");
	const struct scheme *sc = &schemes[0];
	for (size_t i = 0; i < NELEM(schemes); i++)
		if (!strcasecmp(schemes[i].name, name))
			sc = &schemes[i];
	theme.face = sc->face;
	theme.active = sc->active;
	theme.inactive = sc->inactive;
	theme.title_text = sc->title_text;
	theme.select = sc->select;
}

/* repaint every window of every desktop: the colours changed */
static void repaint_all(void)
{
	for (struct wm *wm = all_wms; wm; wm = wm->next_wm) {
		for (struct win *w = wm->wins; w; w = w->next) {
			if (w->pm)
				render_progman(w);
			else if (w->fm)
				render_files(w);
			else if (w->dlg && !w->closed)
				render_dialog(w);
			else if (w->form)
				render_form(w);
		}
		wm_dirty(wm, (struct rect){ 0, 0, wm->disp->surf->w, wm->disp->surf->h });
	}
}

static void wm_settings(const char *key)
{
	if (!*key || !strcmp(key, "mouse.dclick"))
		dclick_ms = (int)MAX(150, MIN(1500, setting_int("mouse.dclick")));
	if (!*key || !strcmp(key, "desktop.scheme")) {
		apply_scheme();
		repaint_all();
	}
	if (*key && !strcmp(key, "desktop.color"))
		for (struct wm *wm = all_wms; wm; wm = wm->next_wm)
			if (!wm->depth)
				wm_set_desktop_colour(wm, setting_rgb("desktop.color"));
}

void desktop_init(void)
{
	setting_watch(wm_settings);
	dev_register(&desk_dev);
	dev_register(&mouse_dev);
	dev_register(&draw_dev);
	for (size_t i = 0; i < NELEM(desk_calls); i++)
		sys_register(&desk_calls[i]);
}
