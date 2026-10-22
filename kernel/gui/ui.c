/*
 * ui.c - pieces every window uses: popup menus (menu bars, right clicks, the
 * control menu), scroll bars, and the small "Are you sure?" and "Name it"
 * dialogs.
 */
#include "wm_int.h"

/* ---- popup menus ------------------------------------------------------------------- */

#define ITEM_H	20
#define SEP_H	8

struct popup_state {
	struct popup p;
	int moved, pressed;
};

/* Labels may mark their keyboard letter with &, as in "Mi&nimize": it's drawn
 * underlined and pressing it picks the item. */
static int label_len(const char *l, const char *end)
{
	int n = 0;
	for (; *l && l != end; l++)
		if (*l != '&')
			n++;
	return n;
}

void text_mnemonic(struct surface *s, int x, int y, const char *l, uint32_t ink)
{
	for (; *l && *l != '\t'; l++) {
		if (*l == '&' && l[1] && l[1] != '\t') {
			l++;
			hline(s, x, y + FONT_H - 2, FONT_W, ink);
		}
		char c[2] = { *l, 0 };
		text(s, x, y, c, ink);
		x += FONT_W;
	}
}

static int mnemonic(const char *l)
{
	const char *a = strchr(l, '&');
	return a && a[1] ? toupper((unsigned char)a[1]) : 0;
}

static int item_h(const struct menu_item *it)
{
	return (it->flags & MI_SEP) ? SEP_H : ITEM_H;
}

void popup_close(struct wm *wm)
{
	struct popup *p = wm->popup;
	if (!p)
		return;
	wm->popup = NULL;
	wm_dirty(wm, p->r);
	struct win *owner = p->owner;
	kfree(p);
	/* the owner's menu bar stops showing a title as open */
	for (struct win *w = wm->wins; w; w = w->next)
		if (w == owner && w->ops && w->ops->repaint)
			w->ops->repaint(w);
}

void popup_open(struct wm *wm, int x, int y, const struct menu_item *items, int n,
    void (*pick)(struct wm *, void *, int), void *arg, struct win *owner)
{
	popup_close(wm);
	struct popup_state *ps = kzalloc(sizeof *ps);
	if (!ps)
		return;
	struct popup *p = &ps->p;
	p->n = MIN(n, MENU_MAX);
	memcpy(p->items, items, sizeof *items * (size_t)p->n);
	int wmax = 0, h = 4;
	for (int i = 0; i < p->n; i++) {
		const char *tab = strchr(p->items[i].label, '\t');
		int lw = label_len(p->items[i].label, tab) + (tab ? 3 + (int)strlen(tab + 1) : 0);
		wmax = MAX(wmax, lw);
		h += item_h(&p->items[i]);
	}
	int w = wmax * FONT_W + 40;
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	if (x + w > sw)
		x = MAX(0, sw - w);
	if (y + h > sh)
		y = MAX(0, sh - h);
	p->r = (struct rect){ x, y, w, h };
	p->sel = -1;
	p->pick = pick;
	p->arg = arg;
	p->owner = owner;
	wm->popup = p;
	wm_dirty(wm, p->r);
}

void popup_draw(struct wm *wm, struct surface *s)
{
	struct popup *p = wm->popup;
	struct rect r = p->r;
	fill(s, r.x, r.y, r.w, r.h, P(theme.face));
	bevel(s, r.x, r.y, r.w, r.h, 1);
	outline(s, r.x - 1, r.y - 1, r.w + 2, r.h + 2, P(0x404040));
	int y = r.y + 2;
	for (int i = 0; i < p->n; i++) {
		struct menu_item *it = &p->items[i];
		if (it->flags & MI_SEP) {
			hline(s, r.x + 4, y + 3, r.w - 8, P(0x808080));
			hline(s, r.x + 4, y + 4, r.w - 8, P(0xFFFFFF));
			y += SEP_H;
			continue;
		}
		int sel = i == p->sel && !(it->flags & MI_DISABLED);
		if (sel)
			fill(s, r.x + 2, y, r.w - 4, ITEM_H, P(theme.select));
		uint32_t ink = P((it->flags & MI_DISABLED) ? 0x808080 : sel ? 0xFFFFFF : 0);
		if (it->flags & MI_CHECKED) {	/* a check mark */
			line(s, r.x + 7, y + 10, r.x + 10, y + 13, ink);
			line(s, r.x + 10, y + 13, r.x + 16, y + 6, ink);
			line(s, r.x + 7, y + 11, r.x + 10, y + 14, ink);
			line(s, r.x + 10, y + 14, r.x + 16, y + 7, ink);
		}
		const char *tab = strchr(it->label, '\t');
		text_mnemonic(s, r.x + 22, y + 2, it->label, ink);
		if (tab)		/* the shortcut, right-aligned */
			text(s, r.x + r.w - 10 - (int)strlen(tab + 1) * FONT_W, y + 2, tab + 1, ink);
		y += ITEM_H;
	}
}

static int item_at(struct popup *p, int x, int y)
{
	if (!rect_has(p->r, x, y))
		return -1;
	int iy = p->r.y + 2;
	for (int i = 0; i < p->n; i++) {
		int h = item_h(&p->items[i]);
		if (y >= iy && y < iy + h)
			return (p->items[i].flags & (MI_SEP | MI_DISABLED)) ? -1 : i;
		iy += h;
	}
	return -1;
}

static void popup_choose(struct wm *wm, int i)
{
	struct popup *p = wm->popup;
	void (*pick)(struct wm *, void *, int) = p->pick;
	void *arg = p->arg;
	int id = p->items[i].id;
	popup_close(wm);
	if (pick)
		pick(wm, arg, id);
}

int popup_mouse(struct wm *wm, int type, int x, int y)
{
	struct popup *p = wm->popup;
	struct popup_state *ps = (struct popup_state *)p;
	int i = item_at(p, x, y);
	if (type == EV_MOVE) {
		if (i != p->sel) {
			p->sel = i;
			ps->moved = 1;
			wm_dirty(wm, p->r);
		}
		return 1;
	}
	if (type == EV_DOWN) {
		if (!rect_has(p->r, x, y)) {
			popup_close(wm);	/* a click elsewhere just closes it */
			return 1;
		}
		ps->pressed = 1;
		return 1;
	}
	if (type == EV_UP) {
		/* the release of the click that opened the menu doesn't choose anything,
		 * unless you dragged onto an item first */
		if (i >= 0 && (ps->pressed || ps->moved))
			popup_choose(wm, i);
		return 1;
	}
	return 1;
}

int popup_key(struct wm *wm, int k)
{
	struct popup *p = wm->popup;
	if (k == 27) {
		popup_close(wm);
		return 1;
	}
	if (k == KEY_UP || k == KEY_DOWN) {
		int d = k == KEY_DOWN ? 1 : -1;
		for (int tries = 0; tries < p->n; tries++) {
			p->sel = (p->sel + d + p->n) % p->n;
			if (p->sel < 0)
				p->sel = 0;
			if (!(p->items[p->sel].flags & (MI_SEP | MI_DISABLED)))
				break;
		}
		wm_dirty(wm, p->r);
		return 1;
	}
	if (k == '\n' && p->sel >= 0) {
		popup_choose(wm, p->sel);
		return 1;
	}
	/* a letter: the item it's underlined in; failing that, the items that start
	 * with it - picked if there's just one, otherwise stepped through */
	if (k > ' ' && k < 0x7F) {
		int u = toupper(k), first = -1, count = 0, next = -1;
		for (int i = 0; i < p->n; i++) {
			if (p->items[i].flags & (MI_SEP | MI_DISABLED))
				continue;
			if (mnemonic(p->items[i].label) == u) {
				popup_choose(wm, i);
				return 1;
			}
		}
		for (int i = 0; i < p->n; i++) {
			const char *l = p->items[i].label;
			if ((p->items[i].flags & (MI_SEP | MI_DISABLED)) || mnemonic(l) || toupper((unsigned char)l[0]) != u)
				continue;
			if (first < 0)
				first = i;
			if (next < 0 && i > p->sel)
				next = i;
			count++;
		}
		if (count == 1) {
			popup_choose(wm, first);
		} else if (count) {
			p->sel = next >= 0 ? next : first;
			wm_dirty(wm, p->r);
		}
	}
	return 1;
}

/* ---- menu bars ------------------------------------------------------------------------ */

void menubar_draw(struct surface *s, struct menubar *mb, int width)
{
	fill(s, 0, 0, width, MENU_H, P(theme.face));
	hline(s, 0, MENU_H - 1, width, P(0x808080));
	for (int i = 0, x = 6; i < mb->n; i++) {
		int wd = (int)strlen(mb->titles[i]) * FONT_W + 12;
		if (mb->open == i) {
			fill(s, x, 1, wd, MENU_H - 2, P(theme.select));
			text(s, x + 6, 2, mb->titles[i], P(0xFFFFFF));
		} else {
			text(s, x + 6, 2, mb->titles[i], P(theme.menu_text));
		}
		hline(s, x + 6, 2 + FONT_H - 2, FONT_W, mb->open == i ? P(0xFFFFFF) : P(theme.menu_text));
		x += wd;
	}
}

int menubar_hit(struct menubar *mb, int x, int *tx, int *tw)
{
	for (int i = 0, mx = 6; i < mb->n; i++) {
		int wd = (int)strlen(mb->titles[i]) * FONT_W + 12;
		if (x >= mx && x < mx + wd) {
			*tx = mx;
			*tw = wd;
			return i;
		}
		mx += wd;
	}
	return -1;
}

/* ---- scroll bars ------------------------------------------------------------------------- */

int sb_max(const struct scrollbar *sb)
{
	return MAX(0, sb->total - sb->visible);
}

void sb_clamp(struct scrollbar *sb)
{
	sb->pos = MAX(0, MIN(sb->pos, sb_max(sb)));
}

static void sb_geom(const struct scrollbar *sb, int *track0, int *track_len, int *thumb0, int *thumb_len)
{
	int len = sb->vertical ? sb->r.h : sb->r.w;
	*track0 = SB_W;
	*track_len = MAX(0, len - 2 * SB_W);
	if (sb->total <= sb->visible || *track_len < 8) {
		*thumb0 = *thumb_len = 0;
		return;
	}
	*thumb_len = MAX(14, (int)((int64_t)*track_len * sb->visible / sb->total));
	*thumb_len = MIN(*thumb_len, *track_len);
	int room = *track_len - *thumb_len;
	*thumb0 = *track0 + (sb_max(sb) ? (int)((int64_t)room * sb->pos / sb_max(sb)) : 0);
}

static void sb_arrow(struct surface *s, int x, int y, int dir, int enabled)
{
	fill(s, x, y, SB_W, SB_W, P(theme.face));
	bevel(s, x, y, SB_W, SB_W, 1);
	uint32_t c = P(enabled ? 0 : 0x808080);
	for (int i = 0; i < 4; i++) {
		int cx = x + SB_W / 2, cy = y + SB_W / 2;
		switch (dir) {
		case 0: hline(s, cx - i, cy - 2 + i, 2 * i + 1, c); break;
		case 1: hline(s, cx - i, cy + 2 - i, 2 * i + 1, c); break;
		case 2: vline(s, cx - 2 + i, cy - i, 2 * i + 1, c); break;
		case 3: vline(s, cx + 2 - i, cy - i, 2 * i + 1, c); break;
		}
	}
}

void sb_draw(struct surface *s, struct scrollbar *sb)
{
	struct rect r = sb->r;
	int t0, tl, h0, hl;
	sb_geom(sb, &t0, &tl, &h0, &hl);
	int en = sb->total > sb->visible;
	/* the track: a light dither, like the real thing */
	fill(s, r.x, r.y, r.w, r.h, P(0xE0E0E0));
	if (sb->vertical) {
		sb_arrow(s, r.x, r.y, 0, en);
		sb_arrow(s, r.x, r.y + r.h - SB_W, 1, en);
		if (hl) {
			fill(s, r.x, r.y + h0, SB_W, hl, P(theme.face));
			bevel(s, r.x, r.y + h0, SB_W, hl, 1);
		}
	} else {
		sb_arrow(s, r.x, r.y, 2, en);
		sb_arrow(s, r.x + r.w - SB_W, r.y, 3, en);
		if (hl) {
			fill(s, r.x + h0, r.y, hl, SB_W, P(theme.face));
			bevel(s, r.x + h0, r.y, hl, SB_W, 1);
		}
	}
}

int sb_mouse(struct scrollbar *sb, int type, int x, int y, int step)
{
	int old = sb->pos;
	int c = sb->vertical ? y - sb->r.y : x - sb->r.x;
	int len = sb->vertical ? sb->r.h : sb->r.w;
	int t0, tl, h0, hl;
	sb_geom(sb, &t0, &tl, &h0, &hl);
	if (type == EV_DOWN) {
		if (c < SB_W)
			sb->pos -= step;
		else if (c >= len - SB_W)
			sb->pos += step;
		else if (hl && c < h0)
			sb->pos -= MAX(step, sb->visible - step);
		else if (hl && c >= h0 + hl)
			sb->pos += MAX(step, sb->visible - step);
		else if (hl) {
			sb->dragging = 1;
			sb->grab = c - h0;
		}
	} else if (type == EV_MOVE && sb->dragging) {
		int room = tl - hl;
		if (room > 0)
			sb->pos = (int)((int64_t)(c - sb->grab - t0) * sb_max(sb) / room);
	} else if (type == EV_UP) {
		sb->dragging = 0;
	}
	sb_clamp(sb);
	return sb->pos != old;
}

/* ---- "are you sure?" and "what should it be called?" -------------------------------------- */

struct ask {
	int yes, no, edit;
	void (*confirm_done)(struct wm *, void *, int);
	void (*prompt_done)(struct wm *, void *, const char *);
	void *arg;
	int answered;
};

static void ask_event(struct form *f, int id, int ev)
{
	struct ask *a = form_priv(f);
	if (ev == FE_CLOSE) {
		if (!a->answered && a->confirm_done)
			a->confirm_done(form_wm(f), a->arg, 0);
		kfree(a);
		return;
	}
	if (ev != FE_CLICK)
		return;
	if (id == a->yes) {
		a->answered = 1;
		if (a->confirm_done)
			a->confirm_done(form_wm(f), a->arg, 1);
		if (a->prompt_done)
			a->prompt_done(form_wm(f), a->arg, form_text(f, a->edit));
		form_close(f);
	} else if (id == a->no) {
		a->answered = 1;
		if (a->confirm_done)
			a->confirm_done(form_wm(f), a->arg, 0);
		form_close(f);
	}
}

/* lay message text out in lines of at most `cols` characters */
static int wrap(const char *msg, int cols, char lines[][72], int max)
{
	int n = 0;
	const char *p = msg;
	while (*p && n < max) {
		int len = 0, brk = -1;
		while (p[len] && p[len] != '\n' && len < cols) {
			if (p[len] == ' ')
				brk = len;
			len++;
		}
		if (p[len] && p[len] != '\n' && brk > 0)
			len = brk;
		memcpy(lines[n], p, (size_t)len);
		lines[n++][len] = 0;
		p += len;
		while (*p == ' ')
			p++;
		if (*p == '\n')
			p++;
	}
	return n;
}

void ui_confirm(struct wm *wm, const char *title, const char *msg, const char *yes,
    void (*done)(struct wm *wm, void *arg, int yes), void *arg)
{
	struct ask *a = kzalloc(sizeof *a);
	if (!a)
		return;
	char lines[8][72];
	int n = wrap(msg, 50, lines, 8);
	int cw = 440, ch = 24 + n * 18 + 56;
	struct form *f = form_new(title, cw, ch, ask_event, a);
	if (!f) {
		kfree(a);
		return;
	}
	a->confirm_done = done;
	a->arg = arg;
	for (int i = 0; i < n; i++)
		form_add(f, FW_LABEL, 18, 16 + i * 18, 0, 0, lines[i]);
	a->yes = form_add(f, FW_BUTTON, cw / 2 - 96, ch - 40, 88, 26, yes ? yes : "OK");
	a->no = form_add(f, FW_BUTTON, cw / 2 + 8, ch - 40, 88, 26, "Cancel");
	form_default(f, a->yes);
	form_focus(f, a->yes);
	form_show(wm, f, -1, -1);
}

void ui_prompt(struct wm *wm, const char *title, const char *label, const char *initial,
    void (*done)(struct wm *wm, void *arg, const char *text), void *arg)
{
	struct ask *a = kzalloc(sizeof *a);
	if (!a)
		return;
	int cw = 440, ch = 116;
	struct form *f = form_new(title, cw, ch, ask_event, a);
	if (!f) {
		kfree(a);
		return;
	}
	a->prompt_done = done;
	a->arg = arg;
	form_add(f, FW_LABEL, 14, 14, 0, 0, label);
	a->edit = form_add(f, FW_EDIT, 14, 36, cw - 28, 24, initial ? initial : "");
	a->yes = form_add(f, FW_BUTTON, cw - 196, ch - 40, 88, 26, "OK");
	a->no = form_add(f, FW_BUTTON, cw - 100, ch - 40, 88, 26, "Cancel");
	form_default(f, a->yes);
	form_focus(f, a->edit);
	form_show(wm, f, -1, -1);
}
