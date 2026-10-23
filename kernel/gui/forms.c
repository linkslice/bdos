/*
 * forms.c - dialog-box windows built from widgets (form.h): the toolkit
 * behind the Control Panel, the applets and Program Manager's dialogs.
 */
#include "wm_int.h"

/* ---- forms: widget windows (Control Panel and friends) ------------------------------------ */

#define FORM_MAX 64

struct fwidget {
	int kind, x, y, w, h;
	char text[200];
	long val, min, max;
	const char *const *opts;
	int nopts;
	int disabled, hidden, def;
	uint32_t rgb;
	char icon[16];
	char **items;
	int nitems, top;
	long shown_val;		/* list: the selection when top was last fitted to it */
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
	int buttons;
	int (*key_fn)(struct form *f, int key);
	int drag;		/* slider being dragged, -1 none */
	char title[64];
	const char *icon;
	void (*icon_fn)(struct form *f, struct surface *s, int x, int y);
};


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
int form_buttons(struct form *f) { return f->buttons; }
void form_key_hook(struct form *f, int (*fn)(struct form *f, int key)) { f->key_fn = fn; }
int form_is_active(struct form *f) { return f->win && f->wm && f->wm->focus == f->win; }
int form_is_minimized(struct form *f) { return f->win && f->win->minimized; }

void form_pointer(struct form *f, int *x, int *y)
{
	if (!f->win || !f->wm) {
		*x = *y = -10000;
		return;
	}
	*x = f->wm->mx - f->win->x - CLIENT_X;
	*y = f->wm->my - f->win->y - CLIENT_Y;
}

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

int form_list_top(struct form *f, int id) { return W(f, id)->top; }
int form_list_count(struct form *f, int id) { return W(f, id)->nitems; }
int form_list_rows(struct form *f, int id) { return MAX(1, (W(f, id)->h - 4) / 18); }

void form_list_set_top(struct form *f, int id, int top)
{
	struct fwidget *d = W(f, id);
	d->top = MAX(0, MIN(top, d->nitems - form_list_rows(f, id)));
	d->shown_val = d->val;
	form_redraw(f);
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
	if (f->win) {
		f->win->resizable = 1;
		f->win->min_cw = min_w;
		f->win->min_ch = min_h;
	}
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
	w->ops = &form_ops;
	w->resizable = f->min_w > 0;
	w->min_cw = f->min_w;
	w->min_ch = f->min_h;
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

void arrow_glyph(struct surface *s, int cx, int cy, int dir, uint32_t c)
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
	case FW_EDIT: {
		sunken(s, d->x, d->y, d->w, d->h, d->disabled ? theme.face : 0xFFFFFF);
		/* long text scrolls: show the end, where you're typing */
		int fit = MAX(1, (d->w - 10) / FONT_W);
		int len = (int)strlen(d->text);
		const char *shown = len > fit ? d->text + len - fit : d->text;
		text(s, d->x + 4, d->y + (d->h - FONT_H) / 2, shown, ink);
		if (foc)
			fill(s, d->x + 4 + (int)strlen(shown) * FONT_W, d->y + 3, 2, d->h - 6, P(0));
		break;
	}
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
		if (d->val != d->shown_val) {	/* a new selection: show it (the wheel may not) */
			if (d->val < d->top)
				d->top = (int)d->val;
			if (d->val >= d->top + rows)
				d->top = (int)d->val - rows + 1;
			d->shown_val = d->val;
		}
		d->top = MAX(0, MIN(d->top, d->nitems - rows));
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

void render_form(struct win *w)
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
	if (f->key_fn && f->key_fn(f, k)) {
		form_settle(f);
		return;
	}
	struct fwidget *d = f->focus >= 0 ? &f->wd[f->focus] : NULL;
	if (k == 27) {
		form_close(f);
	} else if (k == '\t') {
		form_next_focus(f, 1);
	} else if (k == KEY_SHIFT_TAB) {
		form_next_focus(f, -1);
	} else if (d && d->kind == FW_EDIT && (k == 3 || (k == KEY_INS && (input_mods() & 2)))) {
		clip_text_set(d->text, strlen(d->text));	/* Ctrl+C: the whole field */
	} else if (d && d->kind == FW_EDIT && (k == 22 || (k == KEY_INS && (input_mods() & 1)))) {
		size_t l = strlen(d->text);		/* Ctrl+V: the first line of the clipboard */
		for (const char *p = clip_text_get(); *p && *p != '\r' && *p != '\n' && l + 1 < sizeof d->text; p++)
			if ((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F)
				d->text[l++] = *p;
		d->text[l] = 0;
		form_emit(f, f->focus, FE_CHANGE);
	} else if (d && d->kind == FW_EDIT && k == 24) {	/* Ctrl+X */
		clip_text_set(d->text, strlen(d->text));
		d->text[0] = 0;
		form_emit(f, f->focus, FE_CHANGE);
	} else if (d && d->kind == FW_EDIT && k == '\b') {
		size_t l = strlen(d->text);
		if (l) {
			d->text[l - 1] = 0;
			form_emit(f, f->focus, FE_CHANGE);
		}
	} else if (d && d->kind == FW_EDIT && k >= 0x20 && k < 0x7F) {
		size_t l = strlen(d->text);
		if (l + 1 < sizeof d->text) {
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
		f->drag = hit;	/* the desktop sends moves here until release */
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

void form_tick_all(struct wm *wm)
{
	uint64_t now = uptime_ms();
	for (struct win *w = wm->wins; w; ) {
		struct win *next = w->next;
		struct form *f = w->form;
		if (f && !f->dead && f->tick_ms && now >= f->next_tick) {
			f->next_tick = now + (uint64_t)f->tick_ms;
			form_emit(f, -1, FE_TICK);
			form_settle(f);
			if (f->icon_fn && !f->dead)
				win_icon_dirty(w);
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


/* ---- the window side ---- */

static void form_key_op(struct win *w, int k) { form_key(w->wm, w, k); }

static void form_mouse_op(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	if (!w->form)
		return;
	if (type == EV_DOWN && (buttons & 1)) {
		w->form->buttons = buttons;
		form_click(w->wm, w, x, y, dbl);
	}
	else if (type == EV_MOVE && (buttons & 1))
		form_drag(w, x, y);
	else if (type == EV_UP && w->form)
		w->form->drag = -1;
}

/* a right click: custom areas hear it, anything else gets the window menu */
static void form_context_op(struct win *w, int x, int y)
{
	struct form *f = w->form;
	if (f && !f->dead)
		for (int i = f->n - 1; i >= 0; i--) {
			struct fwidget *d = &f->wd[i];
			if (d->kind != FW_CUSTOM || d->hidden || x < d->x || x >= d->x + d->w || y < d->y ||
			    y >= d->y + d->h)
				continue;
			f->px = x - d->x;
			f->py = y - d->y;
			f->buttons = 2;
			form_emit(f, i, FE_RCLICK);
			form_settle(f);
			return;
		}
	control_menu(w->wm, w, w->x + CLIENT_X + x, w->y + CLIENT_Y + y);
}

static void form_resized_op(struct win *w)
{
	if (!w->form)
		return;
	form_emit(w->form, -1, FE_RESIZE);
	form_settle(w->form);
}

static void form_repaint_op(struct win *w) { render_form(w); }

static void form_destroy_op(struct win *w)
{
	if (w->form)
		form_free(w->form);	/* tells its handler if it didn't know */
	w->form = NULL;
}

static void form_draw_icon_op(struct win *w, struct surface *s, int x, int y)
{
	struct form *f = w->form;
	if (f && f->icon_fn)
		f->icon_fn(f, s, x, y);
	else
		draw_icon(s, x, y, f && f->icon ? f->icon : "control");
}

void form_icon_name(struct form *f, const char *icon)
{
	f->icon = icon;
}

void form_icon_draw(struct form *f, void (*fn)(struct form *f, struct surface *s, int x, int y))
{
	f->icon_fn = fn;
}

/* the wheel scrolls the list under the pointer, or nudges a slider or spin box */
static void form_wheel_op(struct win *w, int x, int y, int clicks)
{
	struct form *f = w->form;
	if (!f || f->dead)
		return;
	for (int i = f->n - 1; i >= 0; i--) {
		struct fwidget *d = &f->wd[i];
		if (d->hidden || d->disabled || x < d->x || x >= d->x + d->w || y < d->y || y >= d->y + d->h)
			continue;
		if (d->kind == FW_LIST) {
			d->top -= clicks * 3;
			d->top = MAX(0, MIN(d->top, d->nitems - MAX(1, (d->h - 4) / 18)));
			render_form(w);
			return;
		}
		if (d->kind == FW_SLIDER || d->kind == FW_SPIN) {
			long v = MAX(d->min, MIN(d->val + clicks, d->max));
			if (v != d->val) {
				d->val = v;
				form_emit(f, i, FE_CHANGE);
				form_settle(f);
			}
			return;
		}
	}
}

const struct winops form_ops = {
	.draw_icon = form_draw_icon_op,
	.wheel = form_wheel_op,
	.key = form_key_op,
	.mouse = form_mouse_op,
	.context = form_context_op,
	.resized = form_resized_op,
	.repaint = form_repaint_op,
	.destroy = form_destroy_op,
	.icon = "control",
};
