/*
 * progman.c - Program Manager, the Windows 3.1 way.
 *
 * Groups are real windows inside Program Manager (MDI): they move, resize,
 * minimize to group icons, maximize, and scroll. Each group *is* a folder in
 * C:\PROGMAN and each icon *is* a file in it: a .LNK file is a program item
 * (its first line is the command; an "icon" extended attribute picks the
 * picture), and any other file or folder shows up as itself and opens the
 * obvious way. New, Delete and Properties change the files; dragging an
 * icon to another group moves the file; dropping files from File Manager
 * makes items for them. Where each group window sits is kept in a "pm.rect"
 * attribute on its folder.
 */
#include "wm_int.h"
#include "../settings.h"

#define PM_ROOT		"C:\\PROGMAN"
#define MAX_GROUPS	16
#define GB		3	/* group frame border */
#define GT		18	/* group title bar */
#define CELL_W		84
#define CELL_H		70
#define GICON_W		84	/* minimized group icons */
#define GICON_H		60

struct pitem {
	char file[VFS_NAME_MAX];
	char label[48];
	char icon[16];
	char cmd[200];
	char dir[200], speed[12];	/* .lnk: where it starts, DOS pace */
	int minimized;			/* .lnk: run minimized */
	int is_lnk, is_dir, order;
};

struct pgroup {
	char name[48];
	char dir[VFS_PATH_MAX];
	struct rect r;		/* frame, in workspace coordinates (before scrolling) */
	int minimized, maximized, placed;
	struct rect restore;
	struct pitem *items;
	int nitems, sel;
	struct scrollbar sb;	/* the icons, when they don't fit */
};

enum { PM_IDLE, PM_MOVE, PM_SIZE, PM_ICON, PM_GSB, PM_WSB };

struct pm {
	struct win *w;
	struct pgroup g[MAX_GROUPS];
	int n;
	int z[MAX_GROUPS];	/* group indices, bottom to top: z[n-1] is active */
	struct menubar mb;
	struct scrollbar hs, vs;
	int show_hs, show_vs;
	int sx, sy;		/* how far the workspace is scrolled */
	int mode, mg, edges, px, py, item;
	struct rect orig;
	struct scrollbar *sbd;	/* the scroll bar being dragged */
	uint64_t last_scan;
};

static const char *const pm_menus[] = { "File", "Options", "Window", "Help" };

static void pm_render(struct win *w);
static struct pm *PM(struct win *w) { return w->priv; }

/* ---- loading groups and items from disk ------------------------------------------- */

static const char *guess_icon(const char *cmd)
{
	const char *dot = strrchr(cmd, '.');
	if (!*cmd)
		return "prompt";
	if (!strncasecmp(cmd, "EDIT", 4))
		return "edit";
	if (!strncasecmp(cmd, "WIN ", 4) || !strncasecmp(cmd, "CTTY ", 5))
		return "network";
	if (!strncasecmp(cmd, "@files", 6))
		return "folder";
	if (!strncasecmp(cmd, "@control", 8))
		return "control";
	if (!strcasecmp(cmd, "@clock"))
		return "clock";
	if (!strcasecmp(cmd, "@pulse"))
		return "pulse";
	if (!strcasecmp(cmd, "@install"))
		return "disk";
	static const struct { const char *cmd, *icon; } apps[] = {
		{ "@help", "help" }, { "@eyes", "eyes" }, { "@taskman", "tasks" }, { "@terminal", "phone" },
		{ "@neko", "cat" }, { "@mines", "mine" }, { "@calc", "calc" }, { "@charmap", "charmap" },
		{ "TELNET", "phone" },
	};
	for (size_t i = 0; i < NELEM(apps); i++)
		if (!strncasecmp(cmd, apps[i].cmd, strlen(apps[i].cmd)))
			return apps[i].icon;
	if (dot && !strncasecmp(dot, ".BAS", 4))
		return "basic";
	if (dot && (!strcasecmp(dot, ".C") || !strcasecmp(dot, ".C\"")))
		return "cfile";
	if (!strncasecmp(cmd, "BASIC", 5))
		return "basic";
	return "prompt";
}

struct load {
	struct pgroup *g;
	int cap;
};

static int load_item(void *arg, struct vstat *st)
{
	struct load *l = arg;
	struct pgroup *g = l->g;
	if (g->nitems == l->cap) {
		int nc = l->cap ? l->cap * 2 : 16;
		struct pitem *ni = krealloc(g->items, sizeof *ni * (size_t)nc);
		if (!ni)
			return 1;
		g->items = ni;
		l->cap = nc;
	}
	struct pitem *it = &g->items[g->nitems++];
	memset(it, 0, sizeof *it);
	strncpy0(it->file, st->name, sizeof it->file);
	it->is_dir = st->type == VFS_DIR;
	it->order = 1000;
	strncpy0(it->label, st->name, sizeof it->label);
	char path[VFS_PATH_MAX], val[32];
	vfs_join(g->dir, st->name, path, sizeof path);
	const char *dot = strrchr(st->name, '.');
	if (!it->is_dir && dot && !strcasecmp(dot, ".LNK")) {
		it->is_lnk = 1;
		char *ld = strrchr(it->label, '.');
		if (ld)
			*ld = 0;
		struct lnk l;
		if (!lnk_read(path, &l)) {
			strncpy0(it->cmd, l.cmd, sizeof it->cmd);
			strncpy0(it->dir, l.dir, sizeof it->dir);
			strncpy0(it->speed, l.speed, sizeof it->speed);
			it->minimized = l.minimized;
		}
		strncpy0(it->icon, guess_icon(it->cmd), sizeof it->icon);
		if (!it->cmd[0])
			it->order = 0;	/* a plain shell comes first */
	} else {
		strncpy0(it->icon, icon_for_file(st->name, it->is_dir), sizeof it->icon);
	}
	if (!vfs_xattr_get(path, "icon", val, sizeof val) && icon_exists(val))
		strncpy0(it->icon, val, sizeof it->icon);
	if (!vfs_xattr_get(path, "order", val, sizeof val))
		it->order = (int)strtol(val, NULL, 10);
	return 0;
}

static void sort_items(struct pgroup *g)
{
	for (int i = 1; i < g->nitems; i++) {
		struct pitem k = g->items[i];
		int j = i - 1;
		int aa = setting_int("pm.autoarrange") != 0;	/* name order, the shell first */
		int ko = aa ? (k.order == 0 ? 0 : 1) : k.order;
		while (j >= 0) {
			int jo = aa ? (g->items[j].order == 0 ? 0 : 1) : g->items[j].order;
			if (!(jo > ko || (jo == ko && strcasecmp(g->items[j].label, k.label) > 0)))
				break;
			g->items[j + 1] = g->items[j];
			j--;
		}
		g->items[j + 1] = k;
	}
}

static void load_group_items(struct pgroup *g)
{
	kfree(g->items);
	g->items = NULL;
	g->nitems = 0;
	struct load l = { g, 0 };
	vfs_list(g->dir, load_item, &l);
	sort_items(g);
	if (g->sel >= g->nitems)
		g->sel = g->nitems ? g->nitems - 1 : 0;
}

struct gscan {
	struct pm *pm;
	char names[MAX_GROUPS][48];
	int n;
};

static int scan_group(void *arg, struct vstat *st)
{
	struct gscan *gs = arg;
	if (st->type == VFS_DIR && gs->n < MAX_GROUPS)
		strncpy0(gs->names[gs->n++], st->name, sizeof gs->names[0]);
	return 0;
}

static void save_layout(struct pgroup *g)
{
	if (!setting_int("pm.savesettings"))
		return;
	char v[48];
	snprintf(v, sizeof v, "%d %d %d %d %d", g->maximized ? g->restore.x : g->r.x,
	    g->maximized ? g->restore.y : g->r.y, g->maximized ? g->restore.w : g->r.w,
	    g->maximized ? g->restore.h : g->r.h, g->minimized ? 1 : g->maximized ? 2 : 0);
	vfs_xattr_set(g->dir, "pm.rect", v);
}

/* where a group goes when it has never been placed: two tidy columns */
static void default_place(struct pm *pm, struct pgroup *g, int idx)
{
	int cw = pm->w->content.w - 24;
	int colw = MAX(CELL_W + 2 * GB + 20, cw / 2);
	int per_row = MAX(1, (colw - 2 * GB - 12) / CELL_W);
	int rows = MAX(1, (g->nitems + per_row - 1) / per_row);
	int h = 2 * GB + GT + rows * CELL_H + 10;
	int col_y[2] = { 8, 8 };
	for (int i = 0; i < idx; i++) {	/* stack under the ones before */
		struct pgroup *o = &pm->g[i];
		if (!o->placed || o->minimized)
			continue;
		int c = o->r.x > cw / 3;
		col_y[c] = MAX(col_y[c], o->r.y + o->r.h + 8);
	}
	int c = col_y[0] <= col_y[1] ? 0 : 1;
	g->r = (struct rect){ 8 + c * (colw + 8), col_y[c], colw, h };
	g->placed = 1;
}

static void pm_scan(struct pm *pm)
{
	struct gscan gs;
	memset(&gs, 0, sizeof gs);
	gs.pm = pm;
	if (vfs_list(PM_ROOT, scan_group, &gs)) {
		vfs_mkdir(PM_ROOT);
		vfs_mkdir(PM_ROOT "\\Main");
		vfs_spew(PM_ROOT "\\Main\\Shell.lnk", "", 0);
		vfs_list(PM_ROOT, scan_group, &gs);
	}
	/* Main first, then the others alphabetically */
	for (int i = 1; i < gs.n; i++) {
		char k[48];
		strncpy0(k, gs.names[i], sizeof k);
		int j = i - 1;
		while (j >= 0 && (strcasecmp(k, "Main") == 0 ||
		    (strcasecmp(gs.names[j], "Main") && strcasecmp(gs.names[j], k) > 0))) {
			strncpy0(gs.names[j + 1], gs.names[j], sizeof k);
			j--;
		}
		strncpy0(gs.names[j + 1], k, sizeof k);
	}
	/* keep what we know about groups that are still there */
	struct pgroup old[MAX_GROUPS];
	char oldz_names[MAX_GROUPS][48];
	int oldn = pm->n;
	memcpy(old, pm->g, sizeof old);
	for (int i = 0; i < oldn; i++)
		strncpy0(oldz_names[i], pm->g[pm->z[i]].name, sizeof oldz_names[i]);
	pm->n = 0;
	for (int i = 0; i < gs.n; i++) {
		struct pgroup *g = &pm->g[pm->n];
		int found = -1;
		for (int k = 0; k < oldn; k++)
			if (old[k].name[0] && !strcasecmp(old[k].name, gs.names[i]))
				found = k;
		if (found >= 0) {
			*g = old[found];
			old[found].name[0] = 0;
			old[found].items = NULL;
		} else {
			memset(g, 0, sizeof *g);
			strncpy0(g->name, gs.names[i], sizeof g->name);
			vfs_join(PM_ROOT, g->name, g->dir, sizeof g->dir);
			g->sb.vertical = 1;
		}
		pm->n++;
		load_group_items(g);
		if (!g->placed) {
			char v[48];
			if (!vfs_xattr_get(g->dir, "pm.rect", v, sizeof v)) {
				const char *p = v;
				int x = (int)strtol(p, &p, 10);
				int y = (int)strtol(p, &p, 10);
				int wd = (int)strtol(p, &p, 10);
				int ht = (int)strtol(p, &p, 10);
				int m = (int)strtol(p, &p, 10);
				if (wd >= 80 && ht >= 40) {
					g->r = (struct rect){ x, y, wd, ht };
					g->minimized = m == 1;
					if (m == 2) {
						g->restore = g->r;
						g->maximized = 1;
					}
					g->placed = 1;
				}
			}
			if (!g->placed)
				default_place(pm, g, pm->n - 1);
		}
	}
	for (int k = 0; k < oldn; k++)
		kfree(old[k].items);
	/* stacking order: as it was for groups we had, new ones on top */
	int nz = 0, used[MAX_GROUPS] = { 0 };
	for (int k = 0; k < oldn; k++)
		for (int i = 0; i < pm->n; i++)
			if (!used[i] && !strcasecmp(pm->g[i].name, oldz_names[k])) {
				pm->z[nz++] = i;
				used[i] = 1;
			}
	for (int i = 0; i < pm->n; i++)
		if (!used[i])
			pm->z[nz++] = i;
	/* Main ends up on top the first time */
	if (!oldn)
		for (int i = 0; i < pm->n; i++)
			if (!strcasecmp(pm->g[pm->z[i]].name, "Main")) {
				int v = pm->z[i];
				memmove(&pm->z[i], &pm->z[i + 1], sizeof(int) * (size_t)(pm->n - 1 - i));
				pm->z[pm->n - 1] = v;
				break;
			}
	pm->last_scan = uptime_ms();
}

/* ---- geometry ------------------------------------------------------------------------- */

static int active(struct pm *pm)
{
	return pm->n ? pm->z[pm->n - 1] : -1;
}

static void activate(struct pm *pm, int gi)
{
	for (int i = 0; i < pm->n; i++)
		if (pm->z[i] == gi) {
			memmove(&pm->z[i], &pm->z[i + 1], sizeof(int) * (size_t)(pm->n - 1 - i));
			pm->z[pm->n - 1] = gi;
			return;
		}
}

static struct rect workspace(struct pm *pm)
{
	struct surface *s = &pm->w->content;
	return (struct rect){ 0, MENU_H, s->w - (pm->show_vs ? SB_W : 0), s->h - MENU_H - (pm->show_hs ? SB_W : 0) };
}

/* a group's frame in client coordinates */
static struct rect gframe(struct pm *pm, struct pgroup *g)
{
	return (struct rect){ g->r.x - pm->sx, MENU_H + g->r.y - pm->sy, g->r.w, g->r.h };
}

static struct rect gclient(struct rect f)
{
	return (struct rect){ f.x + GB, f.y + GB + GT, f.w - 2 * GB, f.h - 2 * GB - GT };
}

static int nmin_before(struct pm *pm, int gi)
{
	int k = 0;
	for (int i = 0; i < gi; i++)
		k += pm->g[i].minimized;
	return k;
}

static struct rect gicon(struct pm *pm, int gi)
{
	struct rect ws = workspace(pm);
	int per_row = MAX(1, ws.w / GICON_W), k = nmin_before(pm, gi);
	return (struct rect){ 4 + (k % per_row) * GICON_W, ws.y + ws.h - GICON_H - (k / per_row) * GICON_H, GICON_W, GICON_H };
}

static int per_row_for(struct rect cl, int with_sb)
{
	return MAX(1, (cl.w - 8 - (with_sb ? SB_W : 0)) / CELL_W);
}

/* lay out a group's icons: does it need its scroll bar? */
static void layout_group(struct pgroup *g, struct rect cl, int *per_row)
{
	int pr = per_row_for(cl, 0);
	int rows = (g->nitems + pr - 1) / pr;
	int need = rows * CELL_H + 6 > cl.h;
	if (need) {
		pr = per_row_for(cl, 1);
		rows = (g->nitems + pr - 1) / pr;
	}
	*per_row = pr;
	g->sb.r = (struct rect){ cl.x + cl.w - SB_W, cl.y, SB_W, cl.h };
	g->sb.total = need ? rows * CELL_H + 6 : cl.h;
	g->sb.visible = cl.h;
	sb_clamp(&g->sb);
}

static void update_scrollbars(struct pm *pm)
{
	struct surface *s = &pm->w->content;
	int maxx = 0, maxy = 0;
	for (int i = 0; i < pm->n; i++) {
		struct pgroup *g = &pm->g[i];
		if (g->minimized || g->maximized)
			continue;
		maxx = MAX(maxx, g->r.x + g->r.w + 4);
		maxy = MAX(maxy, g->r.y + g->r.h + 4);
	}
	int any_min = 0;
	for (int i = 0; i < pm->n; i++)
		any_min |= pm->g[i].minimized;
	if (any_min)
		maxy += GICON_H;
	pm->show_hs = pm->show_vs = 0;
	for (int pass = 0; pass < 2; pass++) {
		struct rect ws = workspace(pm);
		pm->show_hs = maxx > ws.w;
		pm->show_vs = maxy > ws.h;
	}
	struct rect ws = workspace(pm);
	pm->hs = (struct scrollbar){ 0, { 0, s->h - SB_W, ws.w, SB_W }, MAX(maxx, ws.w), ws.w, pm->sx, pm->hs.dragging, pm->hs.grab };
	pm->vs = (struct scrollbar){ 1, { s->w - SB_W, MENU_H, SB_W, ws.h }, MAX(maxy, ws.h), ws.h, pm->sy, pm->vs.dragging, pm->vs.grab };
	sb_clamp(&pm->hs);
	sb_clamp(&pm->vs);
	pm->sx = pm->show_hs ? pm->hs.pos : 0;
	pm->sy = pm->show_vs ? pm->vs.pos : 0;
	/* a maximized group fills the workspace */
	for (int i = 0; i < pm->n; i++)
		if (pm->g[i].maximized)
			pm->g[i].r = (struct rect){ pm->sx, pm->sy, ws.w, ws.h };
}

/* ---- drawing -------------------------------------------------------------------------- */

static void draw_label(struct surface *s, int cx, int cy, const char *lab, int sel)
{
	/* one or two lines of up to 10 characters */
	char l1[12] = "", l2[12] = "";
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
	for (int k = 0; k < 2; k++) {
		const char *ln = k ? l2 : l1;
		if (!*ln)
			continue;
		int lw = (int)strlen(ln) * FONT_W;
		if (sel)
			fill(s, cx + (CELL_W - lw) / 2 - 2, cy + k * 15, lw + 4, 16, P(theme.select));
		text(s, cx + (CELL_W - lw) / 2, cy + k * 15, ln, P(sel ? 0xFFFFFF : 0));
	}
}

static void small_box(struct surface *s, int x, int y, int dir)
{
	fill(s, x, y, GT - 3, GT - 3, P(theme.face));
	bevel(s, x, y, GT - 3, GT - 3, 1);
	int cx = x + (GT - 3) / 2, cy = y + (GT - 3) / 2;
	for (int i = 0; i < 4; i++) {
		if (dir == 0)
			hline(s, cx - i, cy - 2 + i, 2 * i + 1, P(0));
		else
			hline(s, cx - i, cy + 2 - i, 2 * i + 1, P(0));
	}
}

static void draw_group(struct pm *pm, struct surface *s, int gi)
{
	struct pgroup *g = &pm->g[gi];
	struct rect f = gframe(pm, g);
	int act = gi == active(pm) && pm->w->wm->focus == pm->w;
	fill(s, f.x, f.y, f.w, f.h, P(theme.face));
	bevel(s, f.x, f.y, f.w, f.h, 1);
	fill(s, f.x + GB, f.y + GB, f.w - 2 * GB, GT - 1, P(act ? theme.active : theme.inactive));
	char t[48];
	strncpy0(t, g->name, sizeof t);
	int maxc = MAX(1, (f.w - 2 * GB - 2 * GT - 8) / FONT_W);
	if ((int)strlen(t) > maxc)
		t[maxc] = 0;
	text(s, f.x + (f.w - (int)strlen(t) * FONT_W) / 2, f.y + GB + 1, t, P(theme.title_text));
	small_box(s, f.x + f.w - GB - 2 * (GT - 2), f.y + GB + 1, 1);	/* minimize */
	small_box(s, f.x + f.w - GB - (GT - 2), f.y + GB + 1, 0);	/* maximize */
	struct rect cl = gclient(f);
	fill(s, cl.x, cl.y, cl.w, cl.h, P(0xFFFFFF));
	int per_row;
	layout_group(g, cl, &per_row);
	struct rect save = s->clip;
	surf_clip(s, rect_isect(cl, save));
	for (int i = 0; i < g->nitems; i++) {
		int cx = cl.x + 4 + (i % per_row) * CELL_W;
		int cy = cl.y + 4 + (i / per_row) * CELL_H - g->sb.pos;
		if (cy + CELL_H < cl.y || cy > cl.y + cl.h)
			continue;
		draw_icon(s, cx + (CELL_W - 32) / 2, cy + 2, g->items[i].icon);
		draw_label(s, cx, cy + 36, g->items[i].label, i == g->sel && gi == active(pm));
	}
	s->clip = save;
	if (g->sb.total > g->sb.visible)
		sb_draw(s, &g->sb);
}

static void pm_render(struct win *w)
{
	struct pm *pm = PM(w);
	struct surface *s = &w->content;
	update_scrollbars(pm);
	struct rect ws = workspace(pm);
	fill(s, 0, 0, s->w, s->h, P(theme.workspace));
	pm->mb.open = w->wm->popup && w->wm->popup->owner == w ? pm->mb.open : -1;
	menubar_draw(s, &pm->mb, s->w);
	surf_clip(s, ws);
	/* minimized groups: icons along the bottom of the workspace, under the groups */
	for (int gi = 0; gi < pm->n; gi++) {
		if (!pm->g[gi].minimized)
			continue;
		struct rect r = gicon(pm, gi);
		draw_icon(s, r.x + (r.w - 32) / 2, r.y + 2, "group");
		draw_label(s, r.x, r.y + 36, pm->g[gi].name, gi == active(pm));
	}
	for (int i = 0; i < pm->n; i++) {
		int gi = pm->z[i];
		if (!pm->g[gi].minimized)
			draw_group(pm, s, gi);
	}
	surf_noclip(s);
	if (pm->show_hs)
		sb_draw(s, &pm->hs);
	if (pm->show_vs)
		sb_draw(s, &pm->vs);
	if (pm->show_hs && pm->show_vs)
		fill(s, s->w - SB_W, s->h - SB_W, SB_W, SB_W, P(theme.face));
	win_dirty_all(w);
}

/* ---- doing things ------------------------------------------------------------------------- */

static struct pgroup *agroup(struct pm *pm)
{
	int a = active(pm);
	return a >= 0 ? &pm->g[a] : NULL;
}

static void item_path(struct pgroup *g, int i, char *out, size_t n)
{
	vfs_join(g->dir, g->items[i].file, out, n);
}

static void open_item(struct pm *pm, struct pgroup *g, int i)
{
	if (i < 0 || i >= g->nitems)
		return;
	struct pitem *it = &g->items[i];
	char path[VFS_PATH_MAX];
	item_path(g, i, path, sizeof path);
	if (it->is_lnk)
		launch_item(pm->w->wm, it->label, it->cmd, it->dir, it->minimized, it->speed);
	else
		launch_path(pm->w->wm, path);
	if (setting_int("pm.minonuse") && pm->w->wm->progman == pm->w && !pm->w->minimized)
		win_minimize(pm->w, 1);
}

void progman_reload(struct wm *wm)
{
	if (!wm->progman)
		return;
	pm_scan(PM(wm->progman));
	pm_render(wm->progman);
}

static void reload_everywhere(void)
{
	for (struct wm *wm = all_wms; wm; wm = wm->next_wm)
		progman_reload(wm);
	files_refresh_all();
}

/* -- item properties: the dialog for New Item and Properties -- */

static const char *const icon_names[] = {
	"prompt", "edit", "basic", "cfile", "folder", "file", "gear", "game", "book", "chip",
	"computer", "network", "group", "control", "clock", "calendar", "mouse", "keyboard",
	"palette", "desktop", "trash",
};

struct props {
	struct wm *wm;
	char dir[VFS_PATH_MAX];
	char file[VFS_NAME_MAX];	/* "" for a new item */
	int is_lnk;
	int desc, cmd, icon, preview, ro, hidden, ok, cancel;
	int wdir, minimized, speed;
};

/* the paces a DOS program can be held to (dos.speed's words) */
static const char *const pm_speed_names[] = { "Usual (Control Panel)", "XT (4.77 MHz)", "AT (286)", "386", "486",
	"Pentium", "As fast as possible" };
static const char *const pm_speed_keys[] = { "", "xt", "at", "386", "486", "pentium", "max" };

static void props_preview(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct props *p = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, P(0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	long i = form_val(f, p->icon);
	draw_icon(s, r.x + (r.w - 32) / 2, r.y + (r.h - 32) / 2, icon_names[i < 0 ? 0 : i]);
}

static void props_event(struct form *f, int id, int ev)
{
	struct props *p = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(p);
		return;
	}
	if (ev != FE_CLICK)
		return;
	if (id == p->cancel) {
		form_close(f);
		return;
	}
	if (id != p->ok)
		return;
	const char *desc = form_text(f, p->desc);
	if (!*desc || strchr(desc, '\\') || strchr(desc, '/') || strchr(desc, ':')) {
		wm_message(p->wm, "Program Manager", "The description becomes the file's name, so it\ncan't be empty or contain \\ / or :.");
		return;
	}
	char oldpath[VFS_PATH_MAX], newpath[VFS_PATH_MAX], name[VFS_NAME_MAX];
	if (p->is_lnk)
		snprintf(name, sizeof name, "%s.lnk", desc);
	else
		strncpy0(name, desc, sizeof name);
	vfs_join(p->dir, name, newpath, sizeof newpath);
	if (p->file[0]) {
		vfs_join(p->dir, p->file, oldpath, sizeof oldpath);
		if (strcasecmp(oldpath, newpath)) {
			struct vstat st;
			if (!vfs_stat(newpath, &st)) {
				wm_message(p->wm, "Program Manager", "There's already something with that name here.");
				return;
			}
			int r = vfs_rename(oldpath, newpath);
			if (r) {
				wm_message(p->wm, "Program Manager", vfs_strerror(r));
				return;
			}
		}
	} else {
		struct vstat st;
		if (!vfs_stat(newpath, &st)) {
			wm_message(p->wm, "Program Manager", "There's already an item with that description.");
			return;
		}
	}
	if (p->is_lnk) {
		struct lnk l;
		memset(&l, 0, sizeof l);
		strncpy0(l.cmd, form_text(f, p->cmd), sizeof l.cmd);
		strncpy0(l.dir, form_text(f, p->wdir), sizeof l.dir);
		l.minimized = (int)form_val(f, p->minimized);
		long sp = form_val(f, p->speed);
		strncpy0(l.speed, pm_speed_keys[sp >= 0 && sp < (long)NELEM(pm_speed_keys) ? sp : 0], sizeof l.speed);
		lnk_write(newpath, &l);
		vfs_xattr_set(newpath, "icon", icon_names[form_val(f, p->icon)]);
	} else {
		struct vstat st;
		if (!vfs_stat(newpath, &st)) {
			uint8_t a = st.attr & ~(A_RO | A_HIDDEN);
			if (form_val(f, p->ro))
				a |= A_RO;
			if (form_val(f, p->hidden))
				a |= A_HIDDEN;
			vfs_setattr(newpath, a);
		}
	}
	form_close(f);
	reload_everywhere();
}

static void open_props(struct wm *wm, struct pgroup *g, int i)
{
	struct props *p = kzalloc(sizeof *p);
	if (!p)
		return;
	p->wm = wm;
	strncpy0(p->dir, g->dir, sizeof p->dir);
	struct pitem *it = i >= 0 && i < g->nitems ? &g->items[i] : NULL;
	p->is_lnk = !it || it->is_lnk;
	if (it)
		strncpy0(p->file, it->file, sizeof p->file);
	int cw = 440, ch = p->is_lnk ? 310 : 186;
	char title[64];
	snprintf(title, sizeof title, it ? "Properties - %s" : "New Program Item", it ? it->label : "");
	struct form *f = form_new(title, cw, ch, props_event, p);
	if (!f) {
		kfree(p);
		return;
	}
	if (p->is_lnk) {
		form_add(f, FW_LABEL, 14, 18, 0, 0, "Description:");
		p->desc = form_add(f, FW_EDIT, 126, 14, cw - 140, 24, it ? it->label : "");
		form_add(f, FW_LABEL, 14, 50, 0, 0, "Command line:");
		p->cmd = form_add(f, FW_EDIT, 126, 46, cw - 140, 24, it ? it->cmd : "");
		form_add(f, FW_LABEL, 14, 82, 0, 0, "Working dir:");
		p->wdir = form_add(f, FW_EDIT, 126, 78, cw - 140, 24, it ? it->dir : "");
		form_add(f, FW_LABEL, 14, 124, 0, 0, "Icon:");
		p->icon = form_add(f, FW_CHOICE, 126, 120, 170, 24, NULL);
		form_options(f, p->icon, icon_names, (int)NELEM(icon_names));
		for (int k = 0; it && k < (int)NELEM(icon_names); k++)
			if (!strcasecmp(icon_names[k], it->icon))
				form_set_val(f, p->icon, k);
		p->preview = form_add(f, FW_CUSTOM, 310, 110, 48, 44, NULL);
		form_custom(f, p->preview, props_preview);
		form_add(f, FW_LABEL, 14, 164, 0, 0, "DOS speed:");
		p->speed = form_add(f, FW_CHOICE, 126, 160, 220, 24, NULL);
		form_options(f, p->speed, pm_speed_names, (int)NELEM(pm_speed_names));
		for (int k = 1; it && k < (int)NELEM(pm_speed_keys); k++)
			if (!strcasecmp(pm_speed_keys[k], it->speed))
				form_set_val(f, p->speed, k);
		p->minimized = form_add(f, FW_CHECK, 126, 196, 200, 22, "Run minimized");
		form_set_val(f, p->minimized, it && it->minimized);
		form_add(f, FW_LABEL, 14, 232, 0, 0, "Leave the command empty for a shell, and the");
		form_add(f, FW_LABEL, 14, 248, 0, 0, "working directory empty to start where you are.");
	} else {
		char path[VFS_PATH_MAX], info[96], sz[32];
		item_path(g, i, path, sizeof path);
		struct vstat st;
		vfs_stat(path, &st);
		fmt_commas(sz, sizeof sz, st.size);
		int y, mo, d, h, mi, s;
		epoch_to_date(st.mtime, &y, &mo, &d, &h, &mi, &s);
		snprintf(info, sizeof info, st.type == VFS_DIR ? "Folder, changed %04d-%02d-%02d %02d:%02d" :
		    "%s bytes, changed %04d-%02d-%02d %02d:%02d", sz, y, mo, d, h, mi);
		if (st.type == VFS_DIR)
			snprintf(info, sizeof info, "Folder, changed %04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
		form_add(f, FW_LABEL, 14, 18, 0, 0, "Name:");
		p->desc = form_add(f, FW_EDIT, 80, 14, cw - 94, 24, it->file);
		form_add(f, FW_LABEL, 14, 52, 0, 0, info);
		p->ro = form_add(f, FW_CHECK, 14, 78, 160, 20, "Read only");
		form_set_val(f, p->ro, (st.attr & A_RO) != 0);
		p->hidden = form_add(f, FW_CHECK, 180, 78, 160, 20, "Hidden");
		form_set_val(f, p->hidden, (st.attr & A_HIDDEN) != 0);
		p->icon = -1;
		form_add(f, FW_LABEL, 14, 110, 0, 0, path);
	}
	p->ok = form_add(f, FW_BUTTON, cw - 184, ch - 38, 80, 26, "OK");
	p->cancel = form_add(f, FW_BUTTON, cw - 96, ch - 38, 80, 26, "Cancel");
	form_default(f, p->ok);
	form_focus(f, p->desc);
	form_show(wm, f, -1, -1);
}

/* -- new group, delete -- */

static void new_group_done(struct wm *wm, void *arg, const char *name)
{
	(void)arg;
	if (!*name || strchr(name, '\\') || strchr(name, '/') || strchr(name, ':')) {
		wm_message(wm, "New Group", "A group name can't be empty or contain \\ / or :.");
		return;
	}
	char dir[VFS_PATH_MAX];
	vfs_join(PM_ROOT, name, dir, sizeof dir);
	int r = vfs_mkdir(dir);
	if (r) {
		wm_message(wm, "New Group", r == E_EXIST ? "There's already a group with that name." : vfs_strerror(r));
		return;
	}
	reload_everywhere();
	if (wm->progman) {
		struct pm *pm = PM(wm->progman);
		for (int i = 0; i < pm->n; i++)
			if (!strcasecmp(pm->g[i].name, name)) {
				struct pgroup *g = &pm->g[i];
				g->r = (struct rect){ 30 + pm->sx, 30 + pm->sy, 280, 160 };
				save_layout(g);
				activate(pm, i);
			}
		pm_render(wm->progman);
	}
}

struct delreq {
	char path[VFS_PATH_MAX];
};

static void delete_done(struct wm *wm, void *arg, int yes)
{
	struct delreq *d = arg;
	if (yes) {
		int r = fop_delete(d->path);
		if (r)
			wm_message(wm, "Delete", vfs_strerror(r));
		reload_everywhere();
	}
	kfree(d);
}

static void ask_delete_item(struct pm *pm, struct pgroup *g, int i)
{
	if (i < 0 || i >= g->nitems)
		return;
	struct delreq *d = kzalloc(sizeof *d);
	if (!d)
		return;
	item_path(g, i, d->path, sizeof d->path);
	char msg[400];
	struct pitem *it = &g->items[i];
	if (it->is_lnk)
		snprintf(msg, sizeof msg, "Delete the item '%s' from %s?", it->label, g->name);
	else
		snprintf(msg, sizeof msg, "Delete %s '%s'? It's a real %s, not just an icon:\n%s",
		    it->is_dir ? "the folder" : "the file", it->file, it->is_dir ? "folder, and everything in it goes" : "file",
		    d->path);
	ui_confirm(pm->w->wm, "Delete", msg, "Delete", delete_done, d);
}

static void ask_delete_group(struct pm *pm, struct pgroup *g)
{
	struct delreq *d = kzalloc(sizeof *d);
	if (!d)
		return;
	strncpy0(d->path, g->dir, sizeof d->path);
	char msg[200];
	snprintf(msg, sizeof msg, "Delete the group '%s' and the %d item%s in it?", g->name, g->nitems,
	    g->nitems == 1 ? "" : "s");
	ui_confirm(pm->w->wm, "Delete Group", msg, "Delete", delete_done, d);
}

static void arrange(struct pm *pm, int cascade)
{
	struct rect ws = workspace(pm);
	int n = 0;
	for (int i = 0; i < pm->n; i++)
		n += !pm->g[i].minimized;
	int k = 0;
	int any_min = 0;
	for (int i = 0; i < pm->n; i++)
		any_min |= pm->g[i].minimized;
	int avail_h = ws.h - (any_min ? GICON_H : 0);
	for (int zi = 0; zi < pm->n; zi++) {
		struct pgroup *g = &pm->g[pm->z[zi]];
		if (g->minimized)
			continue;
		g->maximized = 0;
		if (cascade) {
			g->r = (struct rect){ 6 + k * 22, 6 + k * 22, MAX(220, ws.w * 2 / 3), MAX(140, avail_h * 2 / 3) };
		} else {
			int cols = n > 2 ? 2 : n, rows = (n + cols - 1) / MAX(1, cols);
			int cwid = ws.w / MAX(1, cols), rh = avail_h / MAX(1, rows);
			g->r = (struct rect){ (k % cols) * cwid, (k / cols) * rh, cwid, rh };
		}
		save_layout(g);
		k++;
	}
	pm->sx = pm->sy = 0;
	pm->hs.pos = pm->vs.pos = 0;
}

/* ---- menus ----------------------------------------------------------------------------- */

enum {
	PM_NEW_ITEM = 1, PM_NEW_GROUP, PM_OPEN, PM_DELETE, PM_PROPS, PM_RUN, PM_SHELL, PM_FILES, PM_EXIT,
	PM_CONTROL, PM_THEMES, PM_DESKTOP, PM_DATE,
	PM_CASCADE, PM_TILE, PM_ARRANGE, PM_CLOCK, PM_ABOUT, PM_KEYS,
	PM_DEL_GROUP, PM_RESTORE_G, PM_MAX_G, PM_MIN_G,
	PM_AUTOARRANGE, PM_MINONUSE, PM_SAVESETTINGS, PM_HELP,
	PM_GROUP0 = 100,
};

static void pm_save_window(struct win *w);

static void pm_pick(struct wm *wm, void *arg, int id)
{
	struct win *w = arg;
	if (!wm->progman || wm->progman != w)
		return;
	struct pm *pm = PM(w);
	struct pgroup *g = agroup(pm);
	extern int control_open(void *wm, const char *applet);
	extern void clock_open(struct wm *wm);
	switch (id) {
	case PM_NEW_ITEM: if (g) open_props(wm, g, -1); break;
	case PM_NEW_GROUP: ui_prompt(wm, "New Program Group", "Group name:", "", new_group_done, NULL); break;
	case PM_OPEN: if (g) open_item(pm, g, g->sel); break;
	case PM_DELETE: if (g) ask_delete_item(pm, g, g->sel); break;
	case PM_PROPS: if (g && g->nitems) open_props(wm, g, g->sel); break;
	case PM_RUN: open_dialog(wm, "Run", "Command line:", 1, 1); break;
	case PM_SHELL: open_term(wm, "Shell", NULL, 80, 25, -1, -1); break;
	case PM_FILES: open_files(wm, "C:\\"); break;
	case PM_EXIT: exit_desktop(wm); break;
	case PM_CONTROL: control_open(wm, NULL); break;
	case PM_THEMES: control_open(wm, "themes"); break;
	case PM_DESKTOP: control_open(wm, "desktop"); break;
	case PM_DATE: control_open(wm, "date"); break;
	case PM_CASCADE: arrange(pm, 1); break;
	case PM_TILE: arrange(pm, 0); break;
	case PM_ARRANGE:
		for (int i = 0; g && i < g->nitems; i++) {
			char path[VFS_PATH_MAX], v[12];
			item_path(g, i, path, sizeof path);
			snprintf(v, sizeof v, "%d", i + 1);
			vfs_xattr_set(path, "order", v);
		}
		break;
	case PM_CLOCK: clock_open(wm); break;
	case PM_AUTOARRANGE:
		setting_set_int("pm.autoarrange", !setting_int("pm.autoarrange"));
		pm_scan(pm);
		break;
	case PM_MINONUSE: setting_set_int("pm.minonuse", !setting_int("pm.minonuse")); break;
	case PM_SAVESETTINGS:
		setting_set_int("pm.savesettings", !setting_int("pm.savesettings"));
		if (setting_int("pm.savesettings")) {	/* from now on: what's there now */
			for (int i = 0; i < pm->n; i++)
				save_layout(&pm->g[i]);
			pm_save_window(w);
		}
		break;
	case PM_HELP: launch_cmd(wm, "Help", "@help"); break;
	case PM_ABOUT: about(wm); break;
	case PM_KEYS:
		open_dialog(wm, "Keyboard",
		    "Alt+N  new shell        Alt+W  next window\nAlt+Q  close window     Alt+X  exit desktop\n"
		    "Alt+Space  window menu  Tab  next group\nArrows, Enter, Del work on the icons.", 0, 0);
		break;
	case PM_DEL_GROUP: if (g) ask_delete_group(pm, g); break;
	case PM_RESTORE_G: if (g) { g->minimized = 0; if (g->maximized) { g->maximized = 0; g->r = g->restore; } save_layout(g); } break;
	case PM_MAX_G: if (g && !g->maximized) { g->minimized = 0; g->restore = g->r; g->maximized = 1; save_layout(g); } break;
	case PM_MIN_G: if (g) { g->minimized = 1; save_layout(g); } break;
	default:
		if (id >= PM_GROUP0 && id < PM_GROUP0 + pm->n) {
			struct pgroup *t = &pm->g[id - PM_GROUP0];
			t->minimized = 0;
			activate(pm, id - PM_GROUP0);
		}
	}
	if (wm->progman == w)
		pm_render(w);
}

static void open_menu(struct pm *pm, int which, int x, int y)
{
	struct win *w = pm->w;
	struct pgroup *g = agroup(pm);
	int have_item = g && g->nitems && !g->minimized;
	struct menu_item it[MENU_MAX];
	int n = 0;
#define ADD(l, f, i) menu_put(it, &n, l, f, i)
	switch (which) {
	case 0:
		ADD("New Program Item...", g ? 0 : MI_DISABLED, PM_NEW_ITEM);
		ADD("New Program Group...", 0, PM_NEW_GROUP);
		ADD("", MI_SEP, 0);
		ADD("Open\tEnter", have_item ? 0 : MI_DISABLED, PM_OPEN);
		ADD("Delete\tDel", have_item ? 0 : MI_DISABLED, PM_DELETE);
		ADD("Properties...", have_item ? 0 : MI_DISABLED, PM_PROPS);
		ADD("Delete Group...", g ? 0 : MI_DISABLED, PM_DEL_GROUP);
		ADD("", MI_SEP, 0);
		ADD("Run...", 0, PM_RUN);
		ADD("New Shell\tAlt+N", 0, PM_SHELL);
		ADD("File Manager", 0, PM_FILES);
		ADD("", MI_SEP, 0);
		ADD("Exit Desktop\tAlt+X", 0, PM_EXIT);
		break;
	case 1:
		ADD("Auto Arrange", setting_int("pm.autoarrange") ? MI_CHECKED : 0, PM_AUTOARRANGE);
		ADD("Minimize on Use", setting_int("pm.minonuse") ? MI_CHECKED : 0, PM_MINONUSE);
		ADD("Save Settings on Exit", setting_int("pm.savesettings") ? MI_CHECKED : 0, PM_SAVESETTINGS);
		ADD("", MI_SEP, 0);
		ADD("Control Panel", 0, PM_CONTROL);
		ADD("Themes...", 0, PM_THEMES);
		ADD("Desktop...", 0, PM_DESKTOP);
		ADD("Date && Time...", 0, PM_DATE);
		break;
	case 2:
		ADD("Cascade", 0, PM_CASCADE);
		ADD("Tile", 0, PM_TILE);
		ADD("Arrange Icons", g ? 0 : MI_DISABLED, PM_ARRANGE);
		ADD("", MI_SEP, 0);
		ADD("Clock", 0, PM_CLOCK);
		ADD("", MI_SEP, 0);
		for (int i = 0; i < pm->n && n < MENU_MAX; i++) {
			it[n] = (struct menu_item){ "", i == active(pm) ? MI_CHECKED : 0, PM_GROUP0 + i };
			strncpy0(it[n].label, pm->g[i].name, sizeof it[n].label);
			n++;
		}
		break;
	case 3:
		ADD("Contents\tF1", 0, PM_HELP);
		ADD("", MI_SEP, 0);
		ADD("About bdos", 0, PM_ABOUT);
		ADD("Keyboard", 0, PM_KEYS);
		break;
	}
#undef ADD
	pm->mb.open = which;
	popup_open(w->wm, x, y, it, n, pm_pick, w, w);
	pm_render(w);
}

/* ---- input ---------------------------------------------------------------------------------- */

enum { HIT_NONE, HIT_TITLE, HIT_MIN, HIT_MAX, HIT_EDGE, HIT_CLIENT, HIT_SB, HIT_ICON };

static int hit_group(struct pm *pm, int x, int y, int *gi_out, int *edges)
{
	struct rect ws = workspace(pm);
	if (!rect_has(ws, x, y))
		return HIT_NONE;
	for (int zi = pm->n - 1; zi >= 0; zi--) {
		int gi = pm->z[zi];
		struct pgroup *g = &pm->g[gi];
		if (g->minimized)
			continue;
		struct rect f = gframe(pm, g);
		if (!rect_has(f, x, y))
			continue;
		*gi_out = gi;
		int lx = x - f.x, ly = y - f.y, e = 0;
		if (!g->maximized) {
			if (lx < GB + 1) e |= 1;
			if (lx >= f.w - GB - 1) e |= 2;
			if (ly < GB + 1) e |= 4;
			if (ly >= f.h - GB - 1) e |= 8;
		}
		if (e) {
			*edges = e;
			return HIT_EDGE;
		}
		if (ly < GB + GT) {
			if (lx >= f.w - GB - 2 * (GT - 2) && lx < f.w - GB - (GT - 2))
				return HIT_MIN;
			if (lx >= f.w - GB - (GT - 2))
				return HIT_MAX;
			return HIT_TITLE;
		}
		struct rect cl = gclient(f);
		if (g->sb.total > g->sb.visible && x >= cl.x + cl.w - SB_W)
			return HIT_SB;
		return HIT_CLIENT;
	}
	for (int gi = 0; gi < pm->n; gi++)
		if (pm->g[gi].minimized && rect_has(gicon(pm, gi), x, y)) {
			*gi_out = gi;
			return HIT_ICON;
		}
	return HIT_NONE;
}

static int item_at(struct pm *pm, struct pgroup *g, int x, int y)
{
	struct rect cl = gclient(gframe(pm, g));
	int per_row;
	layout_group(g, cl, &per_row);
	int c = (x - cl.x - 4) / CELL_W, r = (y - cl.y - 4 + g->sb.pos) / CELL_H;
	if (x < cl.x + 4 || c >= per_row || r < 0)
		return -1;
	int i = r * per_row + c;
	return i < g->nitems ? i : -1;
}

static void pm_mouse(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	struct pm *pm = PM(w);
	struct wm *wm = w->wm;
	if (type == EV_DOWN) {
		if (uptime_ms() - pm->last_scan > 1500)
			pm_scan(pm);	/* catch changes made from a shell */
		if (y < MENU_H) {
			int tx, tw, m = menubar_hit(&pm->mb, x, &tx, &tw);
			if (m >= 0)
				open_menu(pm, m, w->x + CLIENT_X + tx, w->y + CLIENT_Y + MENU_H);
			return;
		}
		if (pm->show_hs && rect_has(pm->hs.r, x, y)) {
			pm->sbd = &pm->hs;
			pm->mode = PM_WSB;
			sb_mouse(&pm->hs, EV_DOWN, x, y, 32);
			pm->sx = pm->hs.pos;
			pm_render(w);
			return;
		}
		if (pm->show_vs && rect_has(pm->vs.r, x, y)) {
			pm->sbd = &pm->vs;
			pm->mode = PM_WSB;
			sb_mouse(&pm->vs, EV_DOWN, x, y, 32);
			pm->sy = pm->vs.pos;
			pm_render(w);
			return;
		}
		int gi = -1, edges = 0, h = hit_group(pm, x, y, &gi, &edges);
		if (gi >= 0)
			activate(pm, gi);
		struct pgroup *g = gi >= 0 ? &pm->g[gi] : NULL;
		pm->px = x;
		pm->py = y;
		switch (h) {
		case HIT_ICON:
			if (dbl) {
				g->minimized = 0;
				save_layout(g);
			}
			break;
		case HIT_MIN:
			g->minimized = 1;
			save_layout(g);
			break;
		case HIT_MAX:
			if (g->maximized) {
				g->maximized = 0;
				g->r = g->restore;
			} else {
				g->restore = g->r;
				g->maximized = 1;
			}
			save_layout(g);
			break;
		case HIT_TITLE:
			if (dbl) {
				if (g->maximized) {
					g->maximized = 0;
					g->r = g->restore;
				} else {
					g->restore = g->r;
					g->maximized = 1;
				}
				save_layout(g);
			} else if (!g->maximized) {
				pm->mode = PM_MOVE;
				pm->mg = gi;
				pm->orig = g->r;
			}
			break;
		case HIT_EDGE:
			pm->mode = PM_SIZE;
			pm->mg = gi;
			pm->edges = edges;
			pm->orig = g->r;
			break;
		case HIT_SB:
			pm->mode = PM_GSB;
			pm->sbd = &g->sb;
			sb_mouse(&g->sb, EV_DOWN, x, y, CELL_H / 2);
			break;
		case HIT_CLIENT: {
			int i = item_at(pm, g, x, y);
			if (i >= 0) {
				g->sel = i;
				if (dbl) {
					open_item(pm, g, i);
				} else {
					pm->mode = PM_ICON;	/* might become a drag */
					pm->mg = gi;
					pm->item = i;
				}
			}
			break;
		}
		}
		pm_render(w);
		return;
	}
	if (type == EV_MOVE && (buttons & 1)) {
		int dx = x - pm->px, dy = y - pm->py;
		struct pgroup *g = pm->mode == PM_MOVE || pm->mode == PM_SIZE || pm->mode == PM_ICON ?
		    &pm->g[pm->mg] : NULL;
		switch (pm->mode) {
		case PM_MOVE:
			g->r.x = MAX(-g->r.w + 40, pm->orig.x + dx);
			g->r.y = MAX(0, pm->orig.y + dy);
			pm_render(w);
			break;
		case PM_SIZE: {
			struct rect o = pm->orig, r = o;
			int minw = 2 * GB + 3 * GT + 40, minh = 2 * GB + GT + 30;
			if (pm->edges & 1) { r.w = MAX(minw, o.w - dx); r.x = o.x + o.w - r.w; }
			if (pm->edges & 2) r.w = MAX(minw, o.w + dx);
			if (pm->edges & 4) { r.h = MAX(minh, o.h - dy); r.y = o.y + o.h - r.h; }
			if (pm->edges & 8) r.h = MAX(minh, o.h + dy);
			g->r = r;
			pm_render(w);
			break;
		}
		case PM_ICON:
			if (dx * dx + dy * dy > 25) {
				char path[VFS_PATH_MAX + 2];
				item_path(g, pm->item, path, sizeof path);
				pm->mode = PM_IDLE;
				wm_dnd_start(wm, w, path, g->items[pm->item].icon);
			}
			break;
		case PM_GSB:
		case PM_WSB:
			if (pm->sbd && sb_mouse(pm->sbd, EV_MOVE, x, y, 0)) {
				if (pm->sbd == &pm->hs)
					pm->sx = pm->hs.pos;
				if (pm->sbd == &pm->vs)
					pm->sy = pm->vs.pos;
				pm_render(w);
			}
			break;
		}
		return;
	}
	if (type == EV_UP) {
		if ((pm->mode == PM_MOVE || pm->mode == PM_SIZE) && pm->mg < pm->n)
			save_layout(&pm->g[pm->mg]);
		if (pm->sbd)
			sb_mouse(pm->sbd, EV_UP, x, y, 0);
		pm->sbd = NULL;
		pm->mode = PM_IDLE;
		pm_render(w);
	}
}

static void pm_context(struct win *w, int x, int y)
{
	struct pm *pm = PM(w);
	int gi = -1, edges = 0, h = hit_group(pm, x, y, &gi, &edges);
	int sx = w->x + CLIENT_X + x, sy = w->y + CLIENT_Y + y;
	struct menu_item it[12];
	int n = 0;
#define ADD(l, f, i) menu_put(it, &n, l, f, i)
	if (gi >= 0) {
		activate(pm, gi);
		struct pgroup *g = &pm->g[gi];
		if (h == HIT_ICON) {
			ADD("Restore", 0, PM_RESTORE_G);
			ADD("Maximize", 0, PM_MAX_G);
			ADD("", MI_SEP, 0);
			ADD("Delete Group...", 0, PM_DEL_GROUP);
		} else {
			int i = h == HIT_CLIENT ? item_at(pm, g, x, y) : -1;
			if (i >= 0) {
				g->sel = i;
				ADD("Open", 0, PM_OPEN);
				ADD("Properties...", 0, PM_PROPS);
				ADD("Delete", 0, PM_DELETE);
				ADD("", MI_SEP, 0);
			}
			ADD("New Program Item...", 0, PM_NEW_ITEM);
			ADD("Arrange Icons", 0, PM_ARRANGE);
			ADD("", MI_SEP, 0);
			ADD(g->maximized ? "Restore" : "Maximize", 0, g->maximized ? PM_RESTORE_G : PM_MAX_G);
			ADD("Minimize", 0, PM_MIN_G);
			ADD("Delete Group...", 0, PM_DEL_GROUP);
		}
	} else if (y >= MENU_H) {
		ADD("New Program Group...", 0, PM_NEW_GROUP);
		ADD("", MI_SEP, 0);
		ADD("Cascade", 0, PM_CASCADE);
		ADD("Tile", 0, PM_TILE);
	} else {
		control_menu(w->wm, w, sx, sy);
		return;
	}
#undef ADD
	pm_render(w);
	popup_open(w->wm, sx, sy, it, n, pm_pick, w, w);
}

static void pm_key(struct win *w, int k)
{
	struct pm *pm = PM(w);
	struct pgroup *g = agroup(pm);
	if (k == KEY_F1) {
		launch_cmd(w->wm, "Help", "@help");
		return;
	}
	if (k == '\t' || k == KEY_SHIFT_TAB) {	/* next group */
		if (pm->n > 1) {
			if (k == '\t') {
				int bottom = pm->z[0];
				activate(pm, bottom);
			} else {
				int top = pm->z[pm->n - 1];
				memmove(&pm->z[1], &pm->z[0], sizeof(int) * (size_t)(pm->n - 1));
				pm->z[0] = top;
			}
		}
		pm_render(w);
		return;
	}
	if (!g)
		return;
	if (g->minimized) {
		if (k == '\n') {
			g->minimized = 0;
			save_layout(g);
		}
		pm_render(w);
		return;
	}
	int per_row;
	layout_group(g, gclient(gframe(pm, g)), &per_row);
	if (k == KEY_RIGHT && g->sel + 1 < g->nitems) g->sel++;
	else if (k == KEY_LEFT && g->sel > 0) g->sel--;
	else if (k == KEY_DOWN && g->sel + per_row < g->nitems) g->sel += per_row;
	else if (k == KEY_UP && g->sel - per_row >= 0) g->sel -= per_row;
	else if (k == '\n') {
		open_item(pm, g, g->sel);
		return;
	} else if (k == KEY_DEL) {
		ask_delete_item(pm, g, g->sel);
		return;
	} else if (k == KEY_F1 + 1) {	/* F2: properties (rename) */
		if (g->nitems)
			open_props(w->wm, g, g->sel);
		return;
	}
	/* keep the selection in view */
	struct rect cl = gclient(gframe(pm, g));
	int row_y = (g->sel / per_row) * CELL_H;
	if (row_y < g->sb.pos)
		g->sb.pos = row_y;
	if (row_y + CELL_H > g->sb.pos + cl.h)
		g->sb.pos = row_y + CELL_H - cl.h + 6;
	sb_clamp(&g->sb);
	pm_render(w);
}

/* files dropped on Program Manager */
static int pm_drop(struct win *w, int x, int y, const char *paths, int copy)
{
	(void)copy;
	struct pm *pm = PM(w);
	int gi = -1, edges = 0, h = hit_group(pm, x, y, &gi, &edges);
	if (gi < 0 || h == HIT_NONE)
		return 0;
	struct pgroup *g = &pm->g[gi];
	char *buf = kstrdup(paths);
	if (!buf)
		return 0;
	for (char *p = buf; *p; ) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (*p) {
			char from_dir[VFS_PATH_MAX];
			vfs_dirname(p, from_dir, sizeof from_dir);
			int from_pm = !strncasecmp(from_dir, PM_ROOT "\\", strlen(PM_ROOT) + 1) &&
			    !strchr(from_dir + strlen(PM_ROOT) + 1, '\\');
			if (from_pm) {
				/* an icon from another group: move the file */
				if (strcasecmp(from_dir, g->dir)) {
					int r = fop_copy(p, g->dir, FOP_MOVE);
					if (r == E_EXIST)
						wm_message(w->wm, "Program Manager", "That group already has an item with that name.");
					else if (r)
						wm_message(w->wm, "Program Manager", vfs_strerror(r));
				}
			} else {
				/* a file from somewhere else: make an item that opens it */
				char name[VFS_NAME_MAX], lnk[VFS_PATH_MAX], cmd[VFS_PATH_MAX + 16];
				strncpy0(name, vfs_basename(p), sizeof name);
				char *dot = strrchr(name, '.');
				struct vstat st;
				int is_dir = !vfs_stat(p, &st) && st.type == VFS_DIR;
				const char *icon = icon_for_file(name, is_dir);
				int runnable = dot && (!strcasecmp(dot, ".BAS") || !strcasecmp(dot, ".C") ||
				    !strcasecmp(dot, ".COM") || !strcasecmp(dot, ".EXE") || !strcasecmp(dot, ".BAT"));
				if (is_dir)
					snprintf(cmd, sizeof cmd, "@files %s", p);
				else if (runnable)
					snprintf(cmd, sizeof cmd, "\"%s\"", p);
				else
					snprintf(cmd, sizeof cmd, "EDIT \"%s\"", p);
				if (dot && !is_dir)
					*dot = 0;
				strcat0(name, ".lnk", sizeof name);
				if (!fop_unique(g->dir, name, lnk, sizeof lnk)) {
					vfs_spew(lnk, cmd, strlen(cmd));
					vfs_xattr_set(lnk, "icon", icon);
				}
			}
		}
		if (!e)
			break;
		p = e + 1;
	}
	kfree(buf);
	activate(pm, gi);
	reload_everywhere();
	return 1;
}

static void pm_resized(struct win *w)
{
	pm_render(w);
}

/* the Program Manager window remembers where it was, and whether it was an icon */
static void pm_save_window(struct win *w)
{
	if (w->wm->depth || !setting_int("pm.savesettings"))
		return;
	struct rect r = w->maximized ? w->restore : frame_rect(w);
	char v[64];
	snprintf(v, sizeof v, "%d %d %d %d %d %d", r.x, r.y, r.w, r.h, w->minimized, w->maximized);
	vfs_xattr_set(PM_ROOT, "pm.window", v);
}

static void pm_destroy(struct win *w)
{
	struct pm *pm = PM(w);
	pm_save_window(w);
	for (int i = 0; i < pm->n; i++)
		kfree(pm->g[i].items);
	kfree(pm);
	w->priv = NULL;
}

/* the wheel: the group under the pointer if its icons scroll, else the workspace */
static void pm_wheel(struct win *w, int x, int y, int clicks)
{
	struct pm *pm = PM(w);
	int gi = -1, edges = 0;
	int hit = hit_group(pm, x, y, &gi, &edges);
	if ((hit == HIT_CLIENT || hit == HIT_SB) && gi >= 0 && pm->g[gi].sb.total > pm->g[gi].sb.visible) {
		struct pgroup *g = &pm->g[gi];
		g->sb.pos -= clicks * CELL_H;
		sb_clamp(&g->sb);
	} else if (pm->show_vs) {
		pm->vs.pos -= clicks * 3 * 16;
		sb_clamp(&pm->vs);
		pm->sy = pm->vs.pos;
	} else {
		return;
	}
	pm_render(w);
}

static const struct winops pm_ops = {
	.key = pm_key,
	.wheel = pm_wheel,
	.mouse = pm_mouse,
	.context = pm_context,
	.resized = pm_resized,
	.repaint = pm_render,
	.destroy = pm_destroy,
	.drop = pm_drop,
	.icon = "group",
};

static int sscanf6(const char *s, int *a, int *b, int *c, int *d, int *e, int *f)
{
	int *out[6] = { a, b, c, d, e, f };
	for (int i = 0; i < 6; i++) {
		while (*s == ' ')
			s++;
		if (!*s)
			return i;
		const char *end;
		*out[i] = (int)strtol(s, &end, 10);
		if (end == s)
			return i;
		s = end;
	}
	return 6;
}

/* Everything in the Startup group starts with the desktop, as in Windows */
struct startup {
	struct wm *wm;
	char names[16][VFS_NAME_MAX];
	int n;
};

static int startup_one(void *arg, struct vstat *st)
{
	struct startup *s = arg;
	if (s->n < 16 && st->name[0] != '.')
		strncpy0(s->names[s->n++], st->name, VFS_NAME_MAX);
	return 0;
}

void progman_startup(struct wm *wm)
{
	struct startup *s = kzalloc(sizeof *s);
	if (!s)
		return;
	s->wm = wm;
	vfs_list(PM_ROOT "\\Startup", startup_one, s);
	for (int i = 0; i < s->n; i++) {
		char path[VFS_PATH_MAX];
		vfs_join(PM_ROOT "\\Startup", s->names[i], path, sizeof path);
		launch_path(wm, path);
	}
	kfree(s);
}

struct win *open_progman(struct wm *wm)
{
	struct pm *pm = kzalloc(sizeof *pm);
	if (!pm)
		return NULL;
	pm->mb = (struct menubar){ pm_menus, 4, -1 };
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	int cw = MIN(600, sw - 60), ch = MIN(440, sh - 100);
	struct win *w = win_new(wm, W_PROGMAN, wm->title, 20, 16, cw, ch);
	if (!w) {
		kfree(pm);
		return NULL;
	}
	w->priv = pm;
	w->ops = &pm_ops;
	w->resizable = 1;
	w->min_cw = 240;
	w->min_ch = 140;
	pm->w = w;
	w->menubar = &pm->mb;
	wm->progman = w;
	pm_scan(pm);
	char v[64];
	int x, y, fw, fh, mn, mx;
	if (!wm->depth && !vfs_xattr_get(PM_ROOT, "pm.window", v, sizeof v) &&
	    sscanf6(v, &x, &y, &fw, &fh, &mn, &mx) == 6 && fw >= 200 && fh >= 120 && x >= -fw / 2 && y >= 0 &&
	    x + fw / 2 < sw && y + 20 < sh) {
		win_resize(w, x, y, MIN(fw, sw), MIN(fh, sh));
		if (mx)
			toggle_maximize(wm, w);
		if (mn)
			win_minimize(w, 1);
	}
	pm_render(w);
	return w;
}
