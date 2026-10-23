/*
 * applets.c - the small desktop programs: Help, Eyes, Task Manager,
 * Terminal (a telnet dialer), Neko, Mines, Calculator and Character Map.
 *
 * Each is a form (form.h) with a custom area or two; launch_cmd() starts
 * them from "@help", "@eyes", "@taskman", "@terminal", "@neko", "@mines",
 * "@calc" and "@charmap" (Program Manager items, or Run...).
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../settings.h"
#include "gfx.h"
#include "form.h"
#include "wm_int.h"

static int raise_existing(struct wm *wm, const char *title)
{
	struct form *f = form_find(wm, title);
	if (!f)
		return 0;
	form_raise(f);
	return 1;
}

static uint64_t rng_state;

static uint32_t rnd(void)
{
	if (!rng_state)
		rng_state = uptime_ms() * 6364136223846793005ull + 1442695040888963407ull;
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 7;
	rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 16);
}

static long isqrt(long v)
{
	if (v <= 0)
		return 0;
	long r = 0, b = 1L << 40;
	while (b > v)
		b >>= 2;
	while (b) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
		b >>= 2;
	}
	return r;
}

/* a filled ellipse centred on cx, cy with radii rx, ry */
static void fill_ellipse(struct surface *s, int cx, int cy, int rx, int ry, uint32_t c)
{
	if (rx < 1 || ry < 1)
		return;
	for (int y = -ry; y <= ry; y++) {
		long w = (long)rx * isqrt((long)ry * ry - (long)y * y) / ry;
		hline(s, cx - (int)w, cy + y, 2 * (int)w + 1, c);
	}
}

/* ---- Help: the files in C:\Docs ----------------------------------------------------------- */

#define HELP_DIR "C:\\Docs"
#define HELP_TOPICS 48

struct help {
	int topics, body, status;
	char names[HELP_TOPICS][VFS_NAME_MAX];
	char paths[HELP_TOPICS][VFS_PATH_MAX];
	int n, shown;
	char *text;		/* the shown file */
	int wrap;		/* the width it was wrapped to */
};

static int help_scan(void *arg, struct vstat *st)
{
	struct help *h = arg;
	const char *dot = strrchr(st->name, '.');
	if (st->type == VFS_DIR || !dot || (strcasecmp(dot, ".TXT") && strcasecmp(dot, ".MD")) ||
	    h->n >= HELP_TOPICS)
		return 0;
	strncpy0(h->names[h->n], st->name, VFS_NAME_MAX);
	*strrchr(h->names[h->n], '.') = 0;
	vfs_join(HELP_DIR, st->name, h->paths[h->n], VFS_PATH_MAX);
	h->n++;
	return 0;
}

/* Join the lines of each paragraph so it can be wrapped to the window:
 * a line ending in a word, followed by one that starts with a word, are one
 * paragraph. Indented lines, headings and their underlines stay as they are. */
static char *help_reflow(const char *t)
{
	size_t n = strlen(t);
	char *o = kmalloc(n + 1);
	if (!o)
		return NULL;
	size_t k = 0;
	for (size_t i = 0; i < n; i++) {
		if (t[i] == '\r')
			continue;
		if (t[i] == '\n' && i > 0 && t[i - 1] != '\n' && t[i - 1] != '\r' && t[i - 1] != ' ') {
			/* this line has text; does the next start a word, and isn't an underline? */
			const char *nx = t + i + 1;
			const char *le = nx;
			while (*le && *le != '\n' && *le != '\r')
				le++;
			int under = nx[0] == '=' || nx[0] == '-';
			/* this line: indented, or an underline itself? */
			size_t ls = i;
			while (ls > 0 && t[ls - 1] != '\n')
				ls--;
			int indented = t[ls] == ' ' || t[ls] == '\t' || t[ls] == '=' || t[ls] == '-';
			int next_word = (nx[0] >= 'a' && nx[0] <= 'z') || (nx[0] >= 'A' && nx[0] <= 'Z') || nx[0] == '(' ||
			    nx[0] == '"' || (nx[0] >= '0' && nx[0] <= '9');
			/* a heading: the next line is its underline */
			const char *nn = *le == '\r' ? le + 2 : *le ? le + 1 : le;
			int heading_next = nn[0] == '=' || nn[0] == '-';
			if (!indented && !under && next_word && !heading_next && le > nx) {
				o[k++] = ' ';
				continue;
			}
		}
		o[k++] = t[i];
	}
	o[k] = 0;
	return o;
}

/* the text, wrapped to the list's width at word boundaries */
static void help_fill(struct form *f)
{
	struct help *h = form_priv(f);
	form_list_clear(f, h->body);
	form_set_val(f, h->body, -1);
	if (!h->text)
		return;
	int fw, fh;
	form_size(f, &fw, &fh);
	int width = MAX(20, (fw - 176 - 8 - 16) / FONT_W);
	h->wrap = width;
	char line[256];
	char *flow = help_reflow(h->text);
	for (const char *p = flow ? flow : h->text; *p; ) {
		const char *e = p;
		while (*e && *e != '\n')
			e++;
		/* one line of the file, in pieces no wider than the window */
		const char *q = p;
		do {
			int len = (int)(e - q);
			if (len > width) {
				int cut = width;
				while (cut > width / 2 && q[cut] != ' ')
					cut--;
				len = q[cut] == ' ' ? cut : width;
			}
			int o = 0;
			for (int i = 0; i < len && o < (int)sizeof line - 1; i++)
				line[o++] = q[i] == '\t' || q[i] == '\r' ? ' ' : q[i];
			line[o] = 0;
			form_list_add(f, h->body, line);
			q += len;
			while (q < e && *q == ' ')
				q++;
		} while (q < e);
		p = *e ? e + 1 : e;
	}
	kfree(flow);
	form_list_set_top(f, h->body, 0);
}

static void help_show(struct form *f, int i)
{
	struct help *h = form_priv(f);
	if (i < 0 || i >= h->n)
		return;
	kfree(h->text);
	size_t len;
	int err;
	h->text = vfs_slurp(h->paths[i], &len, &err);
	h->shown = i;
	char title[80];
	snprintf(title, sizeof title, "Help - %s", h->names[i]);
	form_title(f, title);
	help_fill(f);
}

static void help_layout(struct form *f)
{
	struct help *h = form_priv(f);
	int w, ht;
	form_size(f, &w, &ht);
	form_move(f, h->topics, 8, 8, 160, ht - 16);
	form_move(f, h->body, 176, 8, w - 184, ht - 16);
}

static int help_key(struct form *f, int k)
{
	struct help *h = form_priv(f);
	if (form_focused(f) != h->body)
		return 0;
	int top = form_list_top(f, h->body), page = form_list_rows(f, h->body) - 1;
	switch (k) {
	case KEY_UP: top--; break;
	case KEY_DOWN: top++; break;
	case KEY_PGUP: top -= page; break;
	case KEY_PGDN: case ' ': top += page; break;
	case KEY_HOME: top = 0; break;
	case KEY_END: top = form_list_count(f, h->body); break;
	default: return 0;
	}
	form_list_set_top(f, h->body, top);
	return 1;
}

static void help_event(struct form *f, int id, int ev)
{
	struct help *h = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(h->text);
		kfree(h);
		return;
	}
	if (ev == FE_RESIZE) {
		help_layout(f);
		help_fill(f);
	} else if (ev == FE_CHANGE && id == h->topics) {
		help_show(f, (int)form_val(f, h->topics));
	} else if (ev == FE_CHANGE && id == h->body) {
		form_set_val(f, h->body, -1);	/* reading, not choosing */
	}
}

int help_open(void *wmv, const char *topic)
{
	struct wm *wm = wmv;
	struct form *e = form_find(wm, "Help");
	struct help *h;
	if (!e) {
		for (struct win *w = wm->wins; w && !e; w = w->next)
			if (w->form && !strncmp(w->title, "Help - ", 7))
				e = w->form;
	}
	if (e) {
		h = form_priv(e);
		for (int i = 0; topic && *topic && i < h->n; i++)
			if (!strncasecmp(h->names[i], topic, strlen(topic))) {
				form_set_val(e, h->topics, i);
				help_show(e, i);
			}
		form_raise(e);
		return 0;
	}
	h = kzalloc(sizeof *h);
	if (!h)
		return E_NOMEM;
	vfs_list(HELP_DIR, help_scan, h);
	struct vstat st;
	if (h->n < HELP_TOPICS && !vfs_stat("C:\\Read Me First.txt", &st)) {
		strncpy0(h->names[h->n], "Read Me First", VFS_NAME_MAX);
		strncpy0(h->paths[h->n++], "C:\\Read Me First.txt", VFS_PATH_MAX);
	}
	/* alphabetical */
	for (int i = 1; i < h->n; i++)
		for (int j = i; j > 0 && strcasecmp(h->names[j - 1], h->names[j]) > 0; j--) {
			char t[VFS_PATH_MAX];
			strncpy0(t, h->names[j], VFS_NAME_MAX);
			strncpy0(h->names[j], h->names[j - 1], VFS_NAME_MAX);
			strncpy0(h->names[j - 1], t, VFS_NAME_MAX);
			strncpy0(t, h->paths[j], sizeof t);
			strncpy0(h->paths[j], h->paths[j - 1], VFS_PATH_MAX);
			strncpy0(h->paths[j - 1], t, VFS_PATH_MAX);
		}
	struct form *f = form_new("Help", 840, 520, help_event, h);
	if (!f) {
		kfree(h);
		return E_NOMEM;
	}
	h->topics = form_add(f, FW_LIST, 0, 0, 10, 10, NULL);
	h->body = form_add(f, FW_LIST, 0, 0, 10, 10, NULL);
	for (int i = 0; i < h->n; i++)
		form_list_add(f, h->topics, h->names[i]);
	help_layout(f);
	form_resizable(f, 420, 200);
	form_icon_name(f, "help");
	form_key_hook(f, help_key);
	int pick = 0;
	for (int i = 0; i < h->n; i++)
		if (topic && *topic ? !strncasecmp(h->names[i], topic, strlen(topic)) :
		    !strcasecmp(h->names[i], "Getting Started"))
			pick = i;
	form_set_val(f, h->topics, pick);
	help_show(f, pick);
	form_focus(f, h->topics);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Eyes: they watch the pointer, after xeyes ------------------------------------------- */

struct eyes {
	int area;
	int px, py;		/* where the pointer was last drawn for */
};

/* one eye in r, looking at (tx, ty) */
static void draw_eye(struct surface *s, struct rect r, int tx, int ty)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
	int rx = r.w / 2 - 2, ry = r.h / 2 - 2;
	if (rx < 4 || ry < 4)
		return;
	fill_ellipse(s, cx, cy, rx, ry, col(0));
	fill_ellipse(s, cx, cy, rx - MAX(2, rx / 8), ry - MAX(2, ry / 8), col(0xFFFFFF));
	int pr = MAX(2, MIN(rx, ry) / 3);
	/* the pupil sits as far towards the pointer as the eye allows */
	long dx = tx - cx, dy = ty - cy;
	long ex = rx - pr - MAX(2, rx / 8), ey = ry - pr - MAX(2, ry / 8);
	if (ex < 1 || ey < 1) {
		fill_ellipse(s, cx, cy, pr, pr, col(0));
		return;
	}
	/* scale (dx, dy) onto the ellipse x^2/ex^2 + y^2/ey^2 = 1 if it's outside */
	long n = isqrt((dx * dx * 1024 / (ex * ex)) + (dy * dy * 1024 / (ey * ey)));	/* 32 = on the edge */
	if (n > 32) {
		dx = dx * 32 / n;
		dy = dy * 32 / n;
	}
	fill_ellipse(s, cx + (int)dx, cy + (int)dy, pr, pr, col(0));
}

static void eyes_paint(struct surface *s, struct rect r, int px, int py, uint32_t bg)
{
	fill(s, r.x, r.y, r.w, r.h, col(bg));
	int gap = MAX(2, r.w / 20), w = (r.w - gap * 3) / 2;
	draw_eye(s, (struct rect){ r.x + gap, r.y + gap, w, r.h - 2 * gap }, px, py);
	draw_eye(s, (struct rect){ r.x + 2 * gap + w, r.y + gap, w, r.h - 2 * gap }, px, py);
}

static void eyes_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct eyes *e = form_priv(f);
	form_pointer(f, &e->px, &e->py);
	eyes_paint(s, r, e->px, e->py, ui_colour(0));
}

/* minimized, they still look: the pointer relative to the icon */
static void eyes_icon(struct form *f, struct surface *s, int x, int y)
{
	struct wm *wm = form_wm(f);
	eyes_paint(s, (struct rect){ x, y, 32, 32 }, wm->mx, wm->my, 0xC0C0C0);
}

static void eyes_event(struct form *f, int id, int ev)
{
	(void)id;
	struct eyes *e = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(e);
		return;
	}
	if (ev == FE_RESIZE) {
		int w, h;
		form_size(f, &w, &h);
		form_move(f, e->area, 0, 0, w, h);
	} else if (ev == FE_TICK) {
		int px, py;
		form_pointer(f, &px, &py);
		if ((px != e->px || py != e->py) && !form_is_minimized(f))
			form_redraw(f);
	}
}

int eyes_open(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	struct eyes *e = kzalloc(sizeof *e);
	if (!e)
		return E_NOMEM;
	struct form *f = form_new("Eyes", 150, 100, eyes_event, e);
	if (!f) {
		kfree(e);
		return E_NOMEM;
	}
	e->area = form_add(f, FW_CUSTOM, 0, 0, 150, 100, NULL);
	form_custom(f, e->area, eyes_draw);
	form_resizable(f, 60, 40);
	form_icon_name(f, "eyes");
	form_icon_draw(f, eyes_icon);
	form_tick(f, 60);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Task Manager: the windows here, and every task ----------------------------------------- */

#define TM_MAX 64

struct taskman {
	int wins, tasks, sw, end, cascade, tile, wlabel, tlabel;
	int wid[TM_MAX];	/* window ids, by list row */
	int nw;
};

static struct win *tm_win(struct form *f, int row)
{
	struct taskman *t = form_priv(f);
	if (row < 0 || row >= t->nw)
		return NULL;
	for (struct win *w = form_wm(f)->wins; w; w = w->next)
		if (w->id == t->wid[row] && !w->closed)
			return w;
	return NULL;
}

static void tm_fill(struct form *f)
{
	struct taskman *t = form_priv(f);
	struct wm *wm = form_wm(f);
	long wsel = form_val(f, t->wins), tsel = form_val(f, t->tasks);
	int wtop = form_list_top(f, t->wins), ttop = form_list_top(f, t->tasks);
	form_list_clear(f, t->wins);
	t->nw = 0;
	/* top of the pile first, as Windows lists them */
	struct win *ws[TM_MAX];
	int n = 0;
	for (struct win *w = wm->wins; w && n < TM_MAX; w = w->next)
		if (!w->closed && !(w->form && form_priv(w->form) == t))
			ws[n++] = w;
	for (int i = n - 1; i >= 0; i--) {
		char line[96];
		snprintf(line, sizeof line, "%s%s", ws[i]->title, ws[i]->minimized ? "  (minimized)" : "");
		form_list_add(f, t->wins, line);
		t->wid[t->nw++] = ws[i]->id;
	}
	form_list_clear(f, t->tasks);
	struct task_info *ti = kmalloc(sizeof *ti * 96);
	if (ti) {
		int k = task_snapshot(ti, 96);
		for (int i = 0; i < k; i++) {
			char line[128], core[8] = "-";
			if (ti[i].cpu >= 0)
				snprintf(core, sizeof core, "%d", ti[i].cpu);
			snprintf(line, sizeof line, "%3d  %-22s %3s %8lu ms  %s", ti[i].id, ti[i].name, core,
			    (unsigned long)ti[i].ms, ti[i].cwd);
			form_list_add(f, t->tasks, line);
		}
		kfree(ti);
	}
	form_set_val(f, t->wins, MIN(wsel, (long)t->nw - 1));
	form_set_val(f, t->tasks, tsel);
	form_list_set_top(f, t->wins, wtop);
	form_list_set_top(f, t->tasks, ttop);
}

static void tm_switch(struct form *f)
{
	struct taskman *t = form_priv(f);
	struct win *w = tm_win(f, (int)form_val(f, t->wins));
	if (!w)
		return;
	if (w->minimized)
		win_minimize(w, 0);
	raise_win(form_wm(f), w);
}

static void tm_event(struct form *f, int id, int ev)
{
	struct taskman *t = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(t);
		return;
	}
	if (ev == FE_TICK) {
		tm_fill(f);
	} else if ((ev == FE_CLICK && id == t->sw) || (ev == FE_DBLCLICK && id == t->wins)) {
		tm_switch(f);
	} else if (ev == FE_CLICK && id == t->end) {
		struct win *w = tm_win(f, (int)form_val(f, t->wins));
		if (w)
			close_request(form_wm(f), w);
		tm_fill(f);
	} else if (ev == FE_CLICK && (id == t->cascade || id == t->tile)) {
		tile(form_wm(f), id == t->cascade);
	}
}

static int tm_key(struct form *f, int k)
{
	struct taskman *t = form_priv(f);
	if (k == '\n' && form_focused(f) == t->wins) {
		tm_switch(f);
		return 1;
	}
	if (k == KEY_DEL && form_focused(f) == t->wins) {
		tm_event(f, t->end, FE_CLICK);
		return 1;
	}
	return 0;
}

int taskman_open(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	if (raise_existing(wm, "Task Manager"))
		return 0;
	struct taskman *t = kzalloc(sizeof *t);
	if (!t)
		return E_NOMEM;
	int cw = 560, ch = 420;
	struct form *f = form_new("Task Manager", cw, ch, tm_event, t);
	if (!f) {
		kfree(t);
		return E_NOMEM;
	}
	t->wlabel = form_add(f, FW_LABEL, 10, 8, 0, 0, "Windows on this desktop:");
	t->wins = form_add(f, FW_LIST, 8, 26, cw - 16, 148, NULL);
	t->sw = form_add(f, FW_BUTTON, 8, 182, 96, 26, "Switch To");
	t->end = form_add(f, FW_BUTTON, 112, 182, 96, 26, "End Task");
	t->cascade = form_add(f, FW_BUTTON, cw - 208, 182, 96, 26, "Cascade");
	t->tile = form_add(f, FW_BUTTON, cw - 104, 182, 96, 26, "Tile");
	t->tlabel = form_add(f, FW_LABEL, 10, 222, 0, 0, " ID  Task                   Core   CPU time   Directory");
	t->tasks = form_add(f, FW_LIST, 8, 240, cw - 16, ch - 248, NULL);
	form_default(f, t->sw);
	form_icon_name(f, "tasks");
	form_key_hook(f, tm_key);
	tm_fill(f);
	form_focus(f, t->wins);
	form_tick(f, 1000);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Terminal: a phone book of telnet hosts -------------------------------------------------- */

#define PHONE_FILE "C:\\BDOS\\PHONE.TXT"
#define PHONE_MAX 32

struct dialer {
	int list, name, host, port, dial, save, del;
	char names[PHONE_MAX][32], hosts[PHONE_MAX][64];
	int ports[PHONE_MAX];
	int n;
};

static void phone_load(struct dialer *d)
{
	size_t len;
	int err;
	char *body = vfs_slurp(PHONE_FILE, &len, &err);
	d->n = 0;
	if (!body) {	/* a few to start with */
		static const struct { const char *n, *h; int p; } start[] = {
			{ "Synchronet BBS", "vert.synchro.net", 23 },
			{ "Level 29 BBS", "bbs.fozztexx.com", 23 },
			{ "Telehack", "telehack.com", 23 },
		};
		for (size_t i = 0; i < NELEM(start); i++) {
			strncpy0(d->names[d->n], start[i].n, sizeof d->names[0]);
			strncpy0(d->hosts[d->n], start[i].h, sizeof d->hosts[0]);
			d->ports[d->n++] = start[i].p;
		}
		return;
	}
	/* Name=host port, one a line */
	for (char *p = body; *p && d->n < PHONE_MAX; ) {
		char *e = p;
		while (*e && *e != '\n')
			e++;
		char save = *e;
		*e = 0;
		char *eq = strchr(p, '=');
		if (eq && p[0] != ';') {
			*eq = 0;
			char *h = eq + 1, *sp;
			while (*h == ' ')
				h++;
			int port = 23;
			if ((sp = strchr(h, ' '))) {
				*sp = 0;
				port = (int)strtol(sp + 1, NULL, 10);
			}
			char *r = strchr(h, '\r');
			if (r)
				*r = 0;
			strncpy0(d->names[d->n], p, sizeof d->names[0]);
			strncpy0(d->hosts[d->n], h, sizeof d->hosts[0]);
			d->ports[d->n++] = port > 0 && port < 65536 ? port : 23;
		}
		*e = save;
		p = *e ? e + 1 : e;
	}
	kfree(body);
}

static void phone_save(struct dialer *d)
{
	char *buf = kmalloc(PHONE_MAX * 112 + 64);
	if (!buf)
		return;
	int n = snprintf(buf, 64, "; Terminal's phone book: Name=host port\r\n");
	for (int i = 0; i < d->n; i++)
		n += snprintf(buf + n, 112, "%s=%s %d\r\n", d->names[i], d->hosts[i], d->ports[i]);
	vfs_spew(PHONE_FILE, buf, (size_t)n);
	kfree(buf);
}

static void phone_list(struct form *f)
{
	struct dialer *d = form_priv(f);
	long keep = form_val(f, d->list);
	form_list_clear(f, d->list);
	for (int i = 0; i < d->n; i++) {
		char line[128], port[8] = "";
		if (d->ports[i] != 23)
			snprintf(port, sizeof port, ":%d", d->ports[i]);
		snprintf(line, sizeof line, "%-20s %s%s", d->names[i], d->hosts[i], port);
		form_list_add(f, d->list, line);
	}
	form_set_val(f, d->list, MAX(0, MIN(keep, (long)d->n - 1)));
}

static void phone_pick(struct form *f, int i)
{
	struct dialer *d = form_priv(f);
	if (i < 0 || i >= d->n)
		return;
	char port[8];
	snprintf(port, sizeof port, "%d", d->ports[i]);
	form_set_text(f, d->name, d->names[i]);
	form_set_text(f, d->host, d->hosts[i]);
	form_set_text(f, d->port, port);
}

static void phone_dial(struct form *f)
{
	struct dialer *d = form_priv(f);
	const char *host = form_text(f, d->host), *name = form_text(f, d->name);
	if (!*host) {
		wm_message(form_wm(f), "Terminal", "Type a host name or address to call.");
		return;
	}
	int port = (int)strtol(form_text(f, d->port), NULL, 10);
	char cmd[128];
	snprintf(cmd, sizeof cmd, "TELNET %s %d", host, port > 0 ? port : 23);
	wm_launch(form_wm(f), *name ? name : host, cmd);
}

static void dialer_event(struct form *f, int id, int ev)
{
	struct dialer *d = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(d);
		return;
	}
	if (ev == FE_CHANGE && id == d->list) {
		phone_pick(f, (int)form_val(f, d->list));
	} else if ((ev == FE_DBLCLICK && id == d->list) || (ev == FE_CLICK && id == d->dial)) {
		phone_dial(f);
	} else if (ev == FE_CLICK && id == d->save) {
		const char *name = form_text(f, d->name), *host = form_text(f, d->host);
		if (!*name || !*host || strchr(name, '=')) {
			wm_message(form_wm(f), "Terminal", "An entry needs a name (without =) and a host.");
			return;
		}
		int i = 0;
		while (i < d->n && strcasecmp(d->names[i], name))
			i++;
		if (i == d->n) {
			if (d->n >= PHONE_MAX)
				return;
			d->n++;
		}
		strncpy0(d->names[i], name, sizeof d->names[0]);
		strncpy0(d->hosts[i], host, sizeof d->hosts[0]);
		int port = (int)strtol(form_text(f, d->port), NULL, 10);
		d->ports[i] = port > 0 && port < 65536 ? port : 23;
		phone_save(d);
		phone_list(f);
		form_set_val(f, d->list, i);
	} else if (ev == FE_CLICK && id == d->del) {
		int i = (int)form_val(f, d->list);
		if (i < 0 || i >= d->n)
			return;
		for (int k = i; k + 1 < d->n; k++) {
			memcpy(d->names[k], d->names[k + 1], sizeof d->names[0]);
			memcpy(d->hosts[k], d->hosts[k + 1], sizeof d->hosts[0]);
			d->ports[k] = d->ports[k + 1];
		}
		d->n--;
		phone_save(d);
		phone_list(f);
		phone_pick(f, (int)form_val(f, d->list));
	}
}

int dialer_open(void *wmv, const char *arg)
{
	struct wm *wm = wmv;
	if (raise_existing(wm, "Terminal"))
		return 0;
	struct dialer *d = kzalloc(sizeof *d);
	if (!d)
		return E_NOMEM;
	phone_load(d);
	int cw = 460, ch = 340;
	struct form *f = form_new("Terminal", cw, ch, dialer_event, d);
	if (!f) {
		kfree(d);
		return E_NOMEM;
	}
	form_add(f, FW_LABEL, 10, 8, 0, 0, "Phone book (double-click to call):");
	d->list = form_add(f, FW_LIST, 8, 26, cw - 16, 148, NULL);
	form_add(f, FW_LABEL, 10, 190, 0, 0, "Name:");
	d->name = form_add(f, FW_EDIT, 70, 186, cw - 78, 24, "");
	form_add(f, FW_LABEL, 10, 222, 0, 0, "Host:");
	d->host = form_add(f, FW_EDIT, 70, 218, cw - 190, 24, "");
	form_add(f, FW_LABEL, cw - 110, 222, 0, 0, "Port:");
	d->port = form_add(f, FW_EDIT, cw - 64, 218, 56, 24, "23");
	d->dial = form_add(f, FW_BUTTON, cw - 96, ch - 38, 88, 26, "Dial");
	d->save = form_add(f, FW_BUTTON, 8, ch - 38, 88, 26, "Save");
	d->del = form_add(f, FW_BUTTON, 104, ch - 38, 88, 26, "Delete");
	form_add(f, FW_LABEL, 10, 256, 0, 0, "Calls open in a window; Ctrl+] hangs up.");
	form_default(f, d->dial);
	form_icon_name(f, "phone");
	phone_list(f);
	if (arg && *arg) {	/* @terminal host [port] */
		char h[64];
		strncpy0(h, arg, sizeof h);
		char *sp = strchr(h, ' ');
		if (sp) {
			*sp = 0;
			form_set_text(f, d->port, sp + 1);
		}
		form_set_text(f, d->host, h);
		form_set_text(f, d->name, h);
	} else {
		phone_pick(f, 0);
	}
	form_focus(f, d->list);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Neko: a cat that chases the pointer around its window ------------------------------------ */

static const char *const neko_sit[16] = {
	"................", "................", "................", "....k.....k.....",
	"....kk...kk.....", "....kwkkkwk.....", "...kwwwwwwwk....", "...kwkwwwkwk....",
	"...kwwwpwwwk....", "....kwwwwwk.....", ".....kwwwk......", "....kwwwwwk.....",
	"...kwwwwwwwk.kk.", "...kwwkwkwwkkwk.", "...kkkkkkkkkkk..", "................",
};
static const char *const neko_run1[16] = {
	"................", "................", "................", "................",
	"...........k.k..", "..........kwkwk.", ".kk.......kwwwk.", "k..kkkkkkkwkwwk.",
	"....kwwwwwwwwpk.", "....kwwwwwwwwk..", "....kwwwwwwwk...", "...kwk.kwk.kwk..",
	"..kk....k...kk..", "................", "................", "................",
};
static const char *const neko_run2[16] = {
	"................", "................", "................", "................",
	"...........k.k..", "..........kwkwk.", "..........kwwwk.", ".kkkkkkkkkwkwwk.",
	"k...kwwwwwwwwpk.", "....kwwwwwwwwk..", "....kwwwwwwwk...", "....kwk..kwk....",
	"...kk.....kk....", "................", "................", "................",
};
static const char *const neko_sleep[16] = {
	"................", "................", "................", "................",
	"................", "................", "................", "................",
	"......kkkkk.....", "....kkwwwwwkk...", "...kwwwwwwwwwk..", "..kwwkwwwwwwwk..",
	"..kwkkkwwwwwwk..", "..kwwwwkkwwwk...", "...kkkkkkkkk....", "................",
};

struct neko {
	int area;
	int x, y;		/* the cat's top left, in the window */
	int frame, left, idle, ticks;
	const char *const *pose;
};

static void neko_sprite(struct surface *s, int x, int y, const char *const *rows, int mirror)
{
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 16; i++) {
			char c = rows[j][mirror ? 15 - i : i];
			uint32_t px = c == 'k' ? 0x000000 : c == 'w' ? 0xFFFFFF : c == 'p' ? 0xF090A0 : 0x1000000;
			if (px < 0x1000000)
				fill(s, x + i * 2, y + j * 2, 2, 2, col(px));
		}
}

static void neko_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct neko *n = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xF4F0E0));
	neko_sprite(s, r.x + n->x, r.y + n->y, n->pose ? n->pose : neko_sit, n->left);
	if (n->pose == neko_sleep && (n->ticks / 6) % 2)
		text(s, r.x + n->x + 26, r.y + n->y + 4, (n->ticks / 12) % 2 ? "z" : "Z", col(0x404040));
}

static void neko_event(struct form *f, int id, int ev)
{
	(void)id;
	struct neko *n = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(n);
		return;
	}
	int w, h;
	form_size(f, &w, &h);
	if (ev == FE_RESIZE) {
		form_move(f, n->area, 0, 0, w, h);
		n->x = MIN(n->x, w - 32);
		n->y = MIN(n->y, h - 32);
		return;
	}
	if (ev == FE_CLICK && id == n->area) {	/* poke it awake */
		n->idle = 0;
		n->pose = neko_sit;
		form_redraw(f);
		return;
	}
	if (ev != FE_TICK)
		return;
	n->ticks++;
	const char *const *was = n->pose;
	int ox = n->x, oy = n->y;
	int px, py;
	form_pointer(f, &px, &py);
	int inside = px >= 0 && py >= 0 && px < w && py < h;
	long dx = px - (n->x + 16), dy = py - (n->y + 24);
	long dist = isqrt(dx * dx + dy * dy);
	if (inside && dist > 24) {
		int step = 9;
		n->x += (int)(dx * step / dist);
		n->y += (int)(dy * step / dist);
		n->x = MAX(0, MIN(n->x, w - 32));
		n->y = MAX(0, MIN(n->y, h - 32));
		if (dx < -4)
			n->left = 1;
		else if (dx > 4)
			n->left = 0;
		n->frame ^= 1;
		n->pose = n->frame ? neko_run1 : neko_run2;
		n->idle = 0;
	} else {
		n->idle++;
		n->pose = n->idle < 40 ? neko_sit : neko_sleep;
	}
	if (n->pose != was || n->x != ox || n->y != oy || n->pose == neko_sleep)
		form_redraw(f);
}

int neko_open(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	struct neko *n = kzalloc(sizeof *n);
	if (!n)
		return E_NOMEM;
	n->x = 140;
	n->y = 100;
	n->pose = neko_sit;
	struct form *f = form_new("Neko", 320, 240, neko_event, n);
	if (!f) {
		kfree(n);
		return E_NOMEM;
	}
	n->area = form_add(f, FW_CUSTOM, 0, 0, 320, 240, NULL);
	form_custom(f, n->area, neko_draw);
	form_resizable(f, 80, 60);
	form_icon_name(f, "cat");
	form_tick(f, 100);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Mines ----------------------------------------------------------------------------------- */

#define MC 24			/* cell size */
#define MHEAD 44		/* counters and the face */
#define MMAXW 30
#define MMAXH 16

static const struct { const char *name; int w, h, mines; } mine_levels[] = {
	{ "Beginner (9x9, 10 mines)", 9, 9, 10 },
	{ "Intermediate (16x16, 40)", 16, 16, 40 },
	{ "Expert (30x16, 99)", 30, 16, 99 },
};
static const char *const mine_level_names[] = { "Beginner", "Intermediate", "Expert" };

enum { MS_READY, MS_PLAYING, MS_WON, MS_LOST };

struct mines {
	int level, choice, area;
	int w, h, nmines;
	uint8_t mine[MMAXW * MMAXH], open[MMAXW * MMAXH], flag[MMAXW * MMAXH];
	int state, flags, opened, boom;
	uint64_t started;
	int secs;
};

static int mine_count(struct mines *m, int x, int y)
{
	int n = 0;
	for (int j = -1; j <= 1; j++)
		for (int i = -1; i <= 1; i++) {
			int a = x + i, b = y + j;
			if ((i || j) && a >= 0 && b >= 0 && a < m->w && b < m->h)
				n += m->mine[b * m->w + a];
		}
	return n;
}

static void mines_new(struct mines *m)
{
	m->w = mine_levels[m->level].w;
	m->h = mine_levels[m->level].h;
	m->nmines = mine_levels[m->level].mines;
	memset(m->mine, 0, sizeof m->mine);
	memset(m->open, 0, sizeof m->open);
	memset(m->flag, 0, sizeof m->flag);
	m->state = MS_READY;
	m->flags = m->opened = m->secs = 0;
	m->boom = -1;
}

/* the first click is never a mine, nor next to one */
static void mines_lay(struct mines *m, int sx, int sy)
{
	int placed = 0;
	while (placed < m->nmines) {
		int x = (int)(rnd() % (uint32_t)m->w), y = (int)(rnd() % (uint32_t)m->h);
		if (m->mine[y * m->w + x] || (x >= sx - 1 && x <= sx + 1 && y >= sy - 1 && y <= sy + 1))
			continue;
		m->mine[y * m->w + x] = 1;
		placed++;
	}
	m->state = MS_PLAYING;
	m->started = uptime_ms();
}

static void mines_open(struct mines *m, int x, int y)
{
	static int16_t stack[MMAXW * MMAXH];
	int sp = 0;
	stack[sp++] = (int16_t)(y * m->w + x);
	while (sp) {
		int c = stack[--sp], cx = c % m->w, cy = c / m->w;
		if (m->open[c] || m->flag[c])
			continue;
		m->open[c] = 1;
		if (m->mine[c]) {
			m->boom = c;
			m->state = MS_LOST;
			return;
		}
		m->opened++;
		if (mine_count(m, cx, cy))
			continue;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++) {
				int a = cx + i, b = cy + j;
				if (a >= 0 && b >= 0 && a < m->w && b < m->h && !m->open[b * m->w + a] && sp < (int)NELEM(stack))
					stack[sp++] = (int16_t)(b * m->w + a);
			}
	}
	if (m->opened == m->w * m->h - m->nmines) {
		m->state = MS_WON;
		for (int i = 0; i < m->w * m->h; i++)
			m->flag[i] = m->mine[i];
		m->flags = m->nmines;
	}
}

static void lcd(struct surface *s, int x, int y, int v)
{
	char b[8];
	snprintf(b, sizeof b, "%03d", MAX(-99, MIN(v, 999)));
	fill(s, x, y, 3 * FONT_W + 8, FONT_H + 6, col(0));
	text(s, x + 4, y + 3, b, col(0xFF2020));
}

static void mines_face(struct surface *s, int cx, int cy, int state)
{
	fill(s, cx - 15, cy - 15, 30, 30, col(0xC0C0C0));
	bevel(s, cx - 15, cy - 15, 30, 30, 1);
	fill_ellipse(s, cx, cy, 11, 11, col(0));
	fill_ellipse(s, cx, cy, 10, 10, col(0xF0E030));
	if (state == MS_LOST) {
		for (int e = -1; e <= 1; e += 2) {
			line(s, cx + e * 4 - 2, cy - 5, cx + e * 4 + 2, cy - 1, col(0));
			line(s, cx + e * 4 + 2, cy - 5, cx + e * 4 - 2, cy - 1, col(0));
		}
		hline(s, cx - 4, cy + 5, 9, col(0));
		plot(s, cx - 5, cy + 6, col(0));
		plot(s, cx + 5, cy + 6, col(0));
	} else {
		if (state == MS_WON) {	/* sunglasses */
			fill(s, cx - 8, cy - 4, 7, 4, col(0));
			fill(s, cx + 1, cy - 4, 7, 4, col(0));
			hline(s, cx - 9, cy - 4, 19, col(0));
		} else {
			fill(s, cx - 5, cy - 4, 2, 3, col(0));
			fill(s, cx + 3, cy - 4, 2, 3, col(0));
		}
		hline(s, cx - 3, cy + 6, 7, col(0));
		plot(s, cx - 4, cy + 5, col(0));
		plot(s, cx + 4, cy + 5, col(0));
		plot(s, cx - 5, cy + 4, col(0));
		plot(s, cx + 5, cy + 4, col(0));
	}
}

static void mines_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct mines *m = form_priv(f);
	static const uint32_t ncol[9] = { 0, 0x0000FF, 0x008000, 0xFF0000, 0x000080, 0x800000, 0x008080, 0x000000,
		0x808080 };
	fill(s, r.x, r.y, r.w, r.h, col(0xC0C0C0));
	bevel(s, r.x + 4, r.y + 4, r.w - 8, MHEAD - 8, 0);
	lcd(s, r.x + 10, r.y + 10, m->nmines - m->flags);
	lcd(s, r.x + r.w - 3 * FONT_W - 18, r.y + 10, m->secs);
	mines_face(s, r.x + r.w / 2, r.y + MHEAD / 2, m->state);
	int gx = r.x + (r.w - m->w * MC) / 2, gy = r.y + MHEAD;
	for (int y = 0; y < m->h; y++)
		for (int x = 0; x < m->w; x++) {
			int c = y * m->w + x, px = gx + x * MC, py = gy + y * MC;
			int show_mine = m->mine[c] && (m->open[c] || (m->state == MS_LOST && !m->flag[c]));
			if (!m->open[c] && !show_mine) {
				fill(s, px, py, MC, MC, col(0xC0C0C0));
				bevel(s, px, py, MC, MC, 1);
				if (m->flag[c]) {
					int wrong = m->state == MS_LOST && !m->mine[c];
					vline(s, px + 12, py + 5, 13, col(0));
					fill(s, px + 6, py + 5, 7, 6, col(0xFF0000));
					hline(s, px + 7, py + 18, 10, col(0));
					if (wrong) {
						line(s, px + 4, py + 4, px + 19, py + 19, col(0));
						line(s, px + 19, py + 4, px + 4, py + 19, col(0));
					}
				}
				continue;
			}
			fill(s, px, py, MC, MC, col(c == m->boom ? 0xFF0000 : 0xC0C0C0));
			outline(s, px, py, MC + 1, MC + 1, col(0x808080));
			if (show_mine) {
				fill_ellipse(s, px + 12, py + 12, 6, 6, col(0));
				hline(s, px + 3, py + 12, 19, col(0));
				vline(s, px + 12, py + 3, 19, col(0));
				fill(s, px + 9, py + 9, 2, 2, col(0xFFFFFF));
				continue;
			}
			int n = mine_count(m, x, y);
			if (n) {
				char b[2] = { (char)('0' + n), 0 };
				text(s, px + 9, py + 4, b, col(ncol[n]));
				text(s, px + 10, py + 4, b, col(ncol[n]));
			}
		}
}

static void mines_click(struct form *f, int right)
{
	struct mines *m = form_priv(f);
	int px, py, w, h;
	form_point(f, &px, &py);
	form_size(f, &w, &h);
	if (!right && py < MHEAD && px >= w / 2 - 15 && px < w / 2 + 15) {	/* the face: start again */
		mines_new(m);
		form_redraw(f);
		return;
	}
	if (m->state == MS_WON || m->state == MS_LOST)
		return;
	int gx = (w - m->w * MC) / 2;
	int x = (px - gx) / MC, y = (py - MHEAD) / MC;
	if (px < gx || py < MHEAD || x >= m->w || y >= m->h)
		return;
	int c = y * m->w + x;
	if (right) {
		if (!m->open[c]) {
			m->flag[c] ^= 1;
			m->flags += m->flag[c] ? 1 : -1;
		}
	} else if (m->state == MS_READY) {
		mines_lay(m, x, y);
		mines_open(m, x, y);
	} else if (m->open[c]) {
		/* a number with all its flags down: open the rest around it */
		int fl = 0;
		for (int j = -1; j <= 1; j++)
			for (int i = -1; i <= 1; i++) {
				int a = x + i, b = y + j;
				if (a >= 0 && b >= 0 && a < m->w && b < m->h)
					fl += m->flag[b * m->w + a];
			}
		if (fl == mine_count(m, x, y))
			for (int j = -1; j <= 1; j++)
				for (int i = -1; i <= 1; i++) {
					int a = x + i, b = y + j;
					if (a >= 0 && b >= 0 && a < m->w && b < m->h && m->state == MS_PLAYING)
						mines_open(m, a, b);
				}
	} else if (!m->flag[c]) {
		mines_open(m, x, y);
	}
	form_redraw(f);
}

int mines_open_app(void *wmv, const char *arg);

static void mines_event(struct form *f, int id, int ev)
{
	struct mines *m = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(m);
		return;
	}
	if (ev == FE_TICK && m->state == MS_PLAYING) {
		int s = (int)((uptime_ms() - m->started) / 1000);
		if (s != m->secs) {
			m->secs = s;
			form_redraw(f);
		}
	} else if (ev == FE_CLICK && id == m->area) {
		mines_click(f, (form_buttons(f) & (MOD_SHIFT | MOD_CTRL)) != 0);	/* Shift+click flags too */
	} else if (ev == FE_RCLICK && id == m->area) {
		mines_click(f, 1);
	} else if (ev == FE_CHANGE && id == m->choice && form_val(f, m->choice) != m->level) {
		char v[4];
		snprintf(v, sizeof v, "%ld", form_val(f, m->choice));
		setting_set("mines.level", v);
		struct wm *wm = form_wm(f);
		form_close(f);
		mines_open_app(wm, NULL);	/* a new board, a new size */
	}
}

static int mines_key(struct form *f, int k)
{
	struct mines *m = form_priv(f);
	if (k == KEY_F1 + 1) {	/* F2: a new game, as in Windows */
		mines_new(m);
		form_redraw(f);
		return 1;
	}
	return 0;
}

int mines_open_app(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	if (raise_existing(wm, "Mines"))
		return 0;
	struct mines *m = kzalloc(sizeof *m);
	if (!m)
		return E_NOMEM;
	m->level = (int)MAX(0, MIN(setting_int("mines.level"), (long)NELEM(mine_levels) - 1));
	mines_new(m);
	int gw = m->w * MC, cw = MAX(gw + 16, 236), ch = 36 + MHEAD + m->h * MC + 8;
	struct form *f = form_new("Mines", cw, ch, mines_event, m);
	if (!f) {
		kfree(m);
		return E_NOMEM;
	}
	m->choice = form_add(f, FW_CHOICE, 6, 6, cw - 12, 24, NULL);
	form_options(f, m->choice, mine_level_names, (int)NELEM(mine_level_names));
	form_set_val(f, m->choice, m->level);
	m->area = form_add(f, FW_CUSTOM, 0, 36, cw, ch - 36, NULL);
	form_custom(f, m->area, mines_draw);
	form_icon_name(f, "mine");
	form_key_hook(f, mines_key);
	form_tick(f, 250);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Calculator ---------------------------------------------------------------------------- */

/* numbers are millionths in 64 bits: exact for money and most homework */
#define CSCALE 1000000LL

static const char *const calc_keys[25] = {
	"MC", "MR", "M+", "<-", "C",
	"7", "8", "9", "/", "sqrt",
	"4", "5", "6", "*", "%",
	"1", "2", "3", "-", "1/x",
	"0", "+/-", ".", "+", "=",
};

struct calc {
	int display, btn0;
	char entry[24];		/* what's being typed, or "" */
	long long acc, shown, mem;
	char op;		/* pending: + - * / or 0 */
	int error;
};

static int calc_fmt(long long v, char *out, size_t n)
{
	int neg = v < 0;
	unsigned long long u = neg ? (unsigned long long)-v : (unsigned long long)v;
	unsigned long long ip = u / CSCALE, fp = u % CSCALE;
	char frac[8];
	snprintf(frac, sizeof frac, "%06llu", fp);
	int k = 6;
	while (k > 0 && frac[k - 1] == '0')
		frac[--k] = 0;
	return snprintf(out, n, "%s%llu%s%s", neg ? "-" : "", ip, k ? "." : "", frac);
}

static long long calc_parse(const char *s, int *ok)
{
	long long ip = 0, fp = 0, div = CSCALE;
	int neg = 0, any = 0;
	*ok = 1;
	while (*s == ' ')
		s++;
	if (*s == '-') {
		neg = 1;
		s++;
	}
	for (; *s >= '0' && *s <= '9'; s++, any = 1) {
		if (ip > 9000000000000LL / 10)
			*ok = 0;
		ip = ip * 10 + (*s - '0');
	}
	if (*s == '.')
		for (s++; *s >= '0' && *s <= '9'; s++, any = 1)
			if (div > 1) {
				div /= 10;
				fp += (*s - '0') * div;
			}
	if (!any)
		*ok = 0;
	long long v = ip * CSCALE + fp;
	return neg ? -v : v;
}

static long long calc_value(struct calc *c)
{
	if (!c->entry[0])
		return c->shown;
	int ok;
	return calc_parse(c->entry, &ok);
}

/* 128-bit division without the compiler's helper (there's no libgcc here) */
static __int128 div128(__int128 n, __int128 d)
{
	int neg = (n < 0) != (d < 0);
	unsigned __int128 un = n < 0 ? (unsigned __int128)-n : (unsigned __int128)n;
	unsigned __int128 ud = d < 0 ? (unsigned __int128)-d : (unsigned __int128)d, q = 0, r = 0;
	for (int i = 127; i >= 0; i--) {
		r = (r << 1) | ((un >> i) & 1);
		if (r >= ud) {
			r -= ud;
			q |= (unsigned __int128)1 << i;
		}
	}
	return neg ? -(__int128)q : (__int128)q;
}

static int calc_apply(struct calc *c, char op, long long a, long long b, long long *out)
{
	__int128 r;
	switch (op) {
	case '+': r = (__int128)a + b; break;
	case '-': r = (__int128)a - b; break;
	case '*': r = div128((__int128)a * b, CSCALE); break;
	case '/':
		if (!b)
			return -1;
		r = div128((__int128)a * CSCALE, b);
		break;
	default: r = b;
	}
	if (r > 999999999999LL * CSCALE || r < -999999999999LL * CSCALE)
		return -1;
	*out = (long long)r;
	(void)c;
	return 0;
}

static void calc_press(struct form *f, const char *k)
{
	struct calc *c = form_priv(f);
	size_t l = strlen(c->entry);
	if (c->error && strcmp(k, "C"))
		return;
	if ((k[0] >= '0' && k[0] <= '9' && !k[1]) || !strcmp(k, ".")) {
		if (k[0] == '.' && strchr(c->entry, '.'))
			return;
		if (l < 13) {
			if (!l && k[0] == '.')
				c->entry[l++] = '0';
			c->entry[l] = k[0];
			c->entry[l + 1] = 0;
		}
	} else if (!strcmp(k, "<-")) {
		if (l)
			c->entry[l - 1] = 0;
	} else if (!strcmp(k, "C")) {
		c->entry[0] = 0;
		c->acc = c->shown = 0;
		c->op = 0;
		c->error = 0;
	} else if (!strcmp(k, "+/-")) {
		if (l) {
			if (c->entry[0] == '-')
				memmove(c->entry, c->entry + 1, l);
			else if (l < 14) {
				memmove(c->entry + 1, c->entry, l + 1);
				c->entry[0] = '-';
			}
		} else {
			c->shown = -c->shown;
		}
	} else if (strchr("+-*/=", k[0]) && !k[1]) {
		long long v = calc_value(c), r = v;
		if (c->op && (c->entry[0] || k[0] == '=')) {
			if (calc_apply(c, c->op, c->acc, v, &r))
				c->error = 1;
		}
		c->shown = c->acc = r;
		c->op = k[0] == '=' ? 0 : k[0];
		c->entry[0] = 0;
	} else if (!strcmp(k, "%")) {	/* a percentage of what's pending: 200 + 10 % = 220 */
		long long v = calc_value(c), r;
		if (calc_apply(c, '*', c->op ? c->acc : CSCALE, v, &r) || calc_apply(c, '/', r, 100 * CSCALE, &r))
			c->error = 1;
		else {
			calc_fmt(r, c->entry, sizeof c->entry);
		}
	} else if (!strcmp(k, "sqrt")) {
		long long v = calc_value(c);
		if (v < 0)
			c->error = 1;
		else {
			/* sqrt(v / S) * S = sqrt(v * S) */
			__int128 x = (__int128)v * CSCALE, r = 0, b = (__int128)1 << 100;
			while (b > x)
				b >>= 2;
			while (b) {
				if (x >= r + b) {
					x -= r + b;
					r = (r >> 1) + b;
				} else {
					r >>= 1;
				}
				b >>= 2;
			}
			c->shown = (long long)r;
			c->entry[0] = 0;
		}
	} else if (!strcmp(k, "1/x")) {
		long long r;
		if (calc_apply(c, '/', CSCALE, calc_value(c), &r))
			c->error = 1;
		else {
			c->shown = r;
			c->entry[0] = 0;
		}
	} else if (!strcmp(k, "MC")) {
		c->mem = 0;
	} else if (!strcmp(k, "MR")) {
		calc_fmt(c->mem, c->entry, sizeof c->entry);
	} else if (!strcmp(k, "M+")) {
		long long r;
		if (!calc_apply(c, '+', c->mem, calc_value(c), &r))
			c->mem = r;
	}
	form_redraw(f);
}

static void calc_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct calc *c = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	char b[32];
	if (c->error)
		strcpy(b, "Error");
	else if (c->entry[0])
		strncpy0(b, c->entry, sizeof b);
	else
		calc_fmt(c->shown, b, sizeof b);
	text(s, r.x + r.w - 8 - (int)strlen(b) * FONT_W, r.y + (r.h - FONT_H) / 2, b, col(0));
	if (c->mem)
		text(s, r.x + 6, r.y + (r.h - FONT_H) / 2, "M", col(0x808080));
	if (c->op) {
		char o[2] = { c->op, 0 };
		text(s, r.x + 22, r.y + (r.h - FONT_H) / 2, o, col(0x808080));
	}
}

static int calc_key(struct form *f, int k)
{
	struct calc *c = form_priv(f);
	char b[2] = { (char)k, 0 };
	if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '=' ||
	    k == '%') {
		calc_press(f, b);
	} else if (k == '\n') {
		calc_press(f, "=");
	} else if (k == '\b') {
		calc_press(f, "<-");
	} else if (k == 27 || k == KEY_DEL) {
		calc_press(f, "C");
	} else if (k == 3) {	/* Ctrl+C */
		char v[32];
		if (c->entry[0])
			strncpy0(v, c->entry, sizeof v);
		else
			calc_fmt(c->shown, v, sizeof v);
		clip_text_set(v, strlen(v));
	} else if (k == 22) {	/* Ctrl+V */
		int ok;
		long long v = calc_parse(clip_text_get(), &ok);
		if (ok)
			calc_fmt(v, c->entry, sizeof c->entry);
		form_redraw(f);
	} else {
		return 0;
	}
	return 1;
}

static void calc_event(struct form *f, int id, int ev)
{
	struct calc *c = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(c);
		return;
	}
	if (ev == FE_CLICK && id >= c->btn0 && id < c->btn0 + 25)
		calc_press(f, calc_keys[id - c->btn0]);
}

int calc_open(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	if (raise_existing(wm, "Calculator"))
		return 0;
	struct calc *c = kzalloc(sizeof *c);
	if (!c)
		return E_NOMEM;
	int bw = 52, bh = 32, gap = 6, cw = 5 * bw + 4 * gap + 20, ch = 56 + 5 * (bh + gap) + 6;
	struct form *f = form_new("Calculator", cw, ch, calc_event, c);
	if (!f) {
		kfree(c);
		return E_NOMEM;
	}
	c->display = form_add(f, FW_CUSTOM, 10, 10, cw - 20, 34, NULL);
	form_custom(f, c->display, calc_draw);
	for (int i = 0; i < 25; i++) {
		int id = form_add(f, FW_BUTTON, 10 + (i % 5) * (bw + gap), 56 + (i / 5) * (bh + gap), bw, bh,
		    calc_keys[i]);
		if (!i)
			c->btn0 = id;
	}
	form_icon_name(f, "calc");
	form_key_hook(f, calc_key);
	form_show(wm, f, -1, -1);
	return 0;
}

/* ---- Character Map: the 256 characters of the font ------------------------------------------- */

#define CM_COLS 32
#define CM_W 20
#define CM_H 24

struct charmap {
	int grid, picked, preview, info, sel, copy, back, clear;
	int cur;
	char buf[64];
	int n;
};

static void cm_grid(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct charmap *m = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	for (int c = 0; c < 256; c++) {
		int x = r.x + (c % CM_COLS) * CM_W, y = r.y + (c / CM_COLS) * CM_H;
		int on = c == m->cur;
		if (on)
			fill(s, x, y, CM_W, CM_H, col(0x000080));
		if (c)
			glyph(s, x + (CM_W - FONT_W) / 2, y + (CM_H - FONT_H) / 2, (unsigned char)c,
			    col(on ? 0xFFFFFF : 0), 0, 0);
		outline(s, x, y, CM_W + 1, CM_H + 1, col(0xA0A0A0));
	}
}

static void cm_preview(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct charmap *m = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	struct surface g;
	if (surf_init(&g, FONT_W, FONT_H))
		return;
	fill(&g, 0, 0, FONT_W, FONT_H, col(0xFFFFFF));
	glyph(&g, 0, 0, (unsigned char)m->cur, col(0), col(0xFFFFFF), 1);
	int sc = MAX(1, MIN((r.w - 8) / FONT_W, (r.h - 8) / FONT_H));
	int ox = r.x + (r.w - FONT_W * sc) / 2, oy = r.y + (r.h - FONT_H * sc) / 2;
	for (int y = 0; y < FONT_H; y++)
		for (int x = 0; x < FONT_W; x++)
			if (g.px[(size_t)y * (size_t)g.pitch + x] != col(0xFFFFFF))
				fill(s, ox + x * sc, oy + y * sc, sc, sc, col(0));
	surf_free(&g);
}

static void cm_picked(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct charmap *m = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	for (int i = 0; i < m->n && 6 + (i + 1) * FONT_W < r.w; i++)
		glyph(s, r.x + 6 + i * FONT_W, r.y + (r.h - FONT_H) / 2, (unsigned char)m->buf[i], col(0), 0, 0);
	fill(s, r.x + 6 + m->n * FONT_W, r.y + 4, 1, r.h - 8, col(0));
}

static void cm_info(struct form *f)
{
	struct charmap *m = form_priv(f);
	char b[64];
	snprintf(b, sizeof b, "Character %d (0x%02X)%s", m->cur, m->cur, m->cur < 32 ? ", a control code" : "");
	form_set_text(f, m->info, b);
	form_redraw(f);
}

static void cm_add(struct form *f, int c)
{
	struct charmap *m = form_priv(f);
	if (c && m->n < (int)sizeof m->buf - 1)
		m->buf[m->n++] = (char)c;
	form_redraw(f);
}

static void cm_event(struct form *f, int id, int ev)
{
	struct charmap *m = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(m);
		return;
	}
	if ((ev == FE_CLICK || ev == FE_DBLCLICK) && id == m->grid) {
		int px, py;
		form_point(f, &px, &py);
		int c = (py / CM_H) * CM_COLS + px / CM_W;
		if (c >= 0 && c < 256) {
			m->cur = c;
			if (ev == FE_DBLCLICK)
				cm_add(f, c);
			cm_info(f);
		}
	} else if (ev == FE_CLICK && id == m->sel) {
		cm_add(f, m->cur);
	} else if (ev == FE_CLICK && id == m->back) {
		if (m->n)
			m->n--;
		form_redraw(f);
	} else if (ev == FE_CLICK && id == m->clear) {
		m->n = 0;
		form_redraw(f);
	} else if (ev == FE_CLICK && id == m->copy) {
		if (m->n)
			clip_text_set(m->buf, (size_t)m->n);
		else {
			char one = (char)m->cur;
			clip_text_set(&one, 1);
		}
	}
}

static int cm_key(struct form *f, int k)
{
	struct charmap *m = form_priv(f);
	int c = m->cur;
	switch (k) {
	case KEY_LEFT: c--; break;
	case KEY_RIGHT: c++; break;
	case KEY_UP: c -= CM_COLS; break;
	case KEY_DOWN: c += CM_COLS; break;
	case '\n': cm_add(f, c); return 1;
	case '\b': cm_event(f, m->back, FE_CLICK); return 1;
	case 3: cm_event(f, m->copy, FE_CLICK); return 1;
	default:
		if (k > 0x20 && k < 0x7F) {	/* typing a character finds it */
			c = k;
			break;
		}
		return 0;
	}
	m->cur = (c + 256) % 256;
	cm_info(f);
	return 1;
}

int charmap_open(void *wmv, const char *arg)
{
	(void)arg;
	struct wm *wm = wmv;
	if (raise_existing(wm, "Character Map"))
		return 0;
	struct charmap *m = kzalloc(sizeof *m);
	if (!m)
		return E_NOMEM;
	m->cur = 'A';
	int gw = CM_COLS * CM_W + 1, gh = 8 * CM_H + 1, cw = gw + 16, ch = gh + 140;
	struct form *f = form_new("Character Map", cw, ch, cm_event, m);
	if (!f) {
		kfree(m);
		return E_NOMEM;
	}
	m->grid = form_add(f, FW_CUSTOM, 8, 8, gw, gh, NULL);
	form_custom(f, m->grid, cm_grid);
	m->preview = form_add(f, FW_CUSTOM, 8, gh + 16, 56, 72, NULL);
	form_custom(f, m->preview, cm_preview);
	m->info = form_add(f, FW_LABEL, 76, gh + 20, 0, 0, "");
	form_add(f, FW_LABEL, 76, gh + 46, 0, 0, "To copy:");
	m->picked = form_add(f, FW_CUSTOM, 150, gh + 42, cw - 158, 26, NULL);
	form_custom(f, m->picked, cm_picked);
	m->sel = form_add(f, FW_BUTTON, 76, gh + 80, 88, 26, "Select");
	m->back = form_add(f, FW_BUTTON, 172, gh + 80, 88, 26, "Back");
	m->clear = form_add(f, FW_BUTTON, 268, gh + 80, 88, 26, "Clear");
	m->copy = form_add(f, FW_BUTTON, cw - 96, gh + 80, 88, 26, "Copy");
	form_add(f, FW_LABEL, 8, ch - 26, 0, 0, "Double-click or Enter adds a character; Copy puts them on the clipboard.");
	form_default(f, m->copy);
	form_icon_name(f, "charmap");
	form_key_hook(f, cm_key);
	cm_info(f);
	form_show(wm, f, -1, -1);
	return 0;
}
