/*
 * filemgr.c - File Manager, the Windows 3.1 way: a drive bar, a folder tree
 * on the left, the files on the right, and a status bar.
 *
 * Open, copy, move, delete, rename, new folder and properties come from the
 * File menu, from right-clicking, from the keyboard (Enter, F7 move, F8 copy,
 * Del, F2 rename, F5 refresh) or by dragging: drop files on another File
 * Manager window or a folder to move them (copy, if it's another drive, or
 * with Ctrl held), on a Program Manager group to make an icon for them, or
 * on a shell to type their names. Cut, Copy and Paste (Ctrl+X, C, V) work
 * through a clipboard all File Manager windows share; click, Ctrl+click,
 * Shift+click or drag a box around files to select them.
 */
#include "wm_int.h"

#define ROW_H		18
#define DRIVEBAR_H	26
#define STATUS_H	20
#define MAX_TREE	512

struct trow {
	char path[VFS_PATH_MAX];
	char name[48];
	int depth, expanded, has_kids;
};

struct fent {
	struct vstat st;
	int sel, was;		/* was: the selection when a box drag began */
};

enum { SORT_NAME, SORT_TYPE, SORT_SIZE, SORT_DATE };
enum { FMM_IDLE, FMM_PRESS, FMM_TREE_SB, FMM_LIST_SB, FMM_BAND };

struct fm {
	struct win *w;
	char dir[VFS_PATH_MAX];
	/* the tree */
	struct trow *rows;
	int nrows;
	char (*open)[VFS_PATH_MAX];	/* expanded folders */
	int nopen;
	struct scrollbar tsb;
	/* the files */
	struct fent *ents;
	int n, cursor, anchor;
	struct scrollbar lsb;
	int details, sort, hidden;
	int focus_tree;
	struct menubar mb;
	int mode, px, py, press_item;
	int bx0, by0, bx1, by1;		/* a box being dragged: list content coordinates */
	char drives[26];
	int ndrives;
};

static const char *const fm_menus[] = { "File", "Edit", "View" };

/* the clipboard: '\n'-separated paths, and whether they were cut (Paste moves them) */
static struct {
	char *paths;
	int n, cut;
} clip;

static int clip_has_cut(const char *path)
{
	if (!clip.paths || !clip.cut)
		return 0;
	size_t l = strlen(path);
	for (const char *p = clip.paths; *p; ) {
		const char *e = strchr(p, '\n');
		size_t pl = e ? (size_t)(e - p) : strlen(p);
		if (pl == l && !strncasecmp(p, path, l))
			return 1;
		if (!e)
			break;
		p = e + 1;
	}
	return 0;
}
static const struct winops fm_ops;
static void fm_render(struct win *w);
static struct fm *FM(struct win *w) { return w->priv; }

/* ---- geometry ------------------------------------------------------------------------- */

static struct rect tree_rect(struct win *w)
{
	struct surface *s = &w->content;
	int tw = MAX(120, s->w * 35 / 100);
	return (struct rect){ 0, MENU_H + DRIVEBAR_H, tw, s->h - MENU_H - DRIVEBAR_H - STATUS_H };
}

static struct rect list_rect(struct win *w)
{
	struct surface *s = &w->content;
	struct rect t = tree_rect(w);
	return (struct rect){ t.w + 4, t.y, s->w - t.w - 4, t.h };
}

/* ---- the tree ------------------------------------------------------------------------- */

static int is_open(struct fm *fm, const char *path)
{
	for (int i = 0; i < fm->nopen; i++)
		if (!strcasecmp(fm->open[i], path))
			return 1;
	return 0;
}

static void set_open(struct fm *fm, const char *path, int on)
{
	for (int i = 0; i < fm->nopen; i++)
		if (!strcasecmp(fm->open[i], path)) {
			if (!on) {
				memmove(fm->open[i], fm->open[i + 1], sizeof fm->open[0] * (size_t)(fm->nopen - i - 1));
				fm->nopen--;
			}
			return;
		}
	if (!on)
		return;
	void *n = krealloc(fm->open, sizeof fm->open[0] * (size_t)(fm->nopen + 1));
	if (!n)
		return;
	fm->open = n;
	strncpy0(fm->open[fm->nopen++], path, VFS_PATH_MAX);
}

struct dirs {
	char (*names)[48];
	int n, cap;
	int hidden;
};

static int collect_dir(void *arg, struct vstat *st)
{
	struct dirs *d = arg;
	if (st->type != VFS_DIR || (!d->hidden && (st->attr & A_HIDDEN)))
		return 0;
	if (d->n == d->cap) {
		int nc = d->cap ? d->cap * 2 : 16;
		void *nv = krealloc(d->names, sizeof *d->names * (size_t)nc);
		if (!nv)
			return 1;
		d->names = nv;
		d->cap = nc;
	}
	strncpy0(d->names[d->n++], st->name, 48);
	return 0;
}

static void add_rows(struct fm *fm, const char *path, int depth)
{
	struct dirs d = { NULL, 0, 0, fm->hidden };
	vfs_list(path, collect_dir, &d);
	for (int i = 1; i < d.n; i++) {	/* alphabetical */
		char k[48];
		strncpy0(k, d.names[i], sizeof k);
		int j = i - 1;
		while (j >= 0 && strcasecmp(d.names[j], k) > 0) {
			strncpy0(d.names[j + 1], d.names[j], 48);
			j--;
		}
		strncpy0(d.names[j + 1], k, 48);
	}
	for (int i = 0; i < d.n && fm->nrows < MAX_TREE; i++) {
		struct trow *r = &fm->rows[fm->nrows++];
		vfs_join(path, d.names[i], r->path, sizeof r->path);
		strncpy0(r->name, d.names[i], sizeof r->name);
		r->depth = depth;
		r->expanded = is_open(fm, r->path);
		r->has_kids = 1;	/* we find out when it's opened */
		if (r->expanded)
			add_rows(fm, r->path, depth + 1);
	}
	kfree(d.names);
}

static void build_tree(struct fm *fm)
{
	if (!fm->rows)
		fm->rows = kmalloc(sizeof(struct trow) * MAX_TREE);
	if (!fm->rows)
		return;
	fm->nrows = 0;
	/* everything on the way to the current folder is open */
	char p[VFS_PATH_MAX];
	strncpy0(p, fm->dir, sizeof p);
	while (strlen(p) > 3) {
		set_open(fm, p, 1);
		char up[VFS_PATH_MAX];
		vfs_dirname(p, up, sizeof up);
		if (!strcasecmp(up, p))
			break;
		strncpy0(p, up, sizeof p);
	}
	struct trow *root = &fm->rows[fm->nrows++];
	snprintf(root->path, sizeof root->path, "%c:\\", fm->dir[0]);
	snprintf(root->name, sizeof root->name, "%c:\\", fm->dir[0]);
	root->depth = 0;
	root->expanded = 1;
	root->has_kids = 1;
	add_rows(fm, root->path, 1);
}

/* ---- the file list ------------------------------------------------------------------------ */

static int collect_ent(void *arg, struct vstat *st)
{
	struct fm *fm = arg;
	if (!fm->hidden && (st->attr & A_HIDDEN))
		return 0;
	void *ne = krealloc(fm->ents, sizeof(struct fent) * (size_t)(fm->n + 1));
	if (!ne)
		return 1;
	fm->ents = ne;
	fm->ents[fm->n].st = *st;
	fm->ents[fm->n].sel = 0;
	fm->n++;
	return 0;
}

static int ent_cmp(struct fm *fm, const struct fent *a, const struct fent *b)
{
	if (!strcmp(a->st.name, ".."))
		return -1;
	if (!strcmp(b->st.name, ".."))
		return 1;
	int ad = a->st.type == VFS_DIR, bd = b->st.type == VFS_DIR;
	if (ad != bd)
		return bd - ad;		/* folders first */
	switch (fm->sort) {
	case SORT_TYPE: {
		const char *ea = strrchr(a->st.name, '.'), *eb = strrchr(b->st.name, '.');
		int c = strcasecmp(ea ? ea : "", eb ? eb : "");
		if (c)
			return c;
		break;
	}
	case SORT_SIZE:
		if (a->st.size != b->st.size)
			return a->st.size > b->st.size ? -1 : 1;
		break;
	case SORT_DATE:
		if (a->st.mtime != b->st.mtime)
			return a->st.mtime > b->st.mtime ? -1 : 1;
		break;
	}
	return strcasecmp(a->st.name, b->st.name);
}

static void load_list(struct fm *fm, int keep_selection)
{
	/* remember what was selected, by name */
	char (*was)[VFS_NAME_MAX] = NULL;
	int nwas = 0;
	char cur_name[VFS_NAME_MAX] = "";
	if (keep_selection && fm->n) {
		was = kmalloc(sizeof *was * (size_t)fm->n);
		for (int i = 0; was && i < fm->n; i++)
			if (fm->ents[i].sel)
				strncpy0(was[nwas++], fm->ents[i].st.name, VFS_NAME_MAX);
		if (fm->cursor < fm->n)
			strncpy0(cur_name, fm->ents[fm->cursor].st.name, sizeof cur_name);
	}
	kfree(fm->ents);
	fm->ents = NULL;
	fm->n = 0;
	if (strlen(fm->dir) > 3) {
		struct vstat up;
		memset(&up, 0, sizeof up);
		up.type = VFS_DIR;
		strcpy(up.name, "..");
		collect_ent(fm, &up);
	}
	int r = vfs_list(fm->dir, collect_ent, fm);
	if (r && r != E_NOENT) {
		/* the folder went away or the drive did: fall back to the root */
	}
	for (int i = 1; i < fm->n; i++) {
		struct fent k = fm->ents[i];
		int j = i - 1;
		while (j >= 0 && ent_cmp(fm, &fm->ents[j], &k) > 0) {
			fm->ents[j + 1] = fm->ents[j];
			j--;
		}
		fm->ents[j + 1] = k;
	}
	fm->cursor = 0;
	for (int i = 0; i < fm->n; i++) {
		for (int k = 0; k < nwas; k++)
			if (!strcasecmp(was[k], fm->ents[i].st.name))
				fm->ents[i].sel = 1;
		if (cur_name[0] && !strcasecmp(cur_name, fm->ents[i].st.name))
			fm->cursor = i;
	}
	kfree(was);
	if (!keep_selection)
		fm->lsb.pos = 0;
}

static void fm_title(struct fm *fm)
{
	snprintf(fm->w->title, sizeof fm->w->title, "File Manager - %s", fm->dir);
	win_dirty_all(fm->w);
}

static void list_drive(struct vfs_drive *d, void *arg)
{
	struct fm *fm = arg;
	if (fm->ndrives < 26)
		fm->drives[fm->ndrives++] = d->letter;
}

static void go(struct fm *fm, const char *dir)
{
	struct vstat st;
	if (vfs_stat(dir, &st) || st.type != VFS_DIR) {
		wm_message(fm->w->wm, "File Manager", "That folder isn't there.");
		return;
	}
	char canon[VFS_PATH_MAX];
	if (vfs_canon(dir, canon, sizeof canon))
		strncpy0(canon, dir, sizeof canon);
	strncpy0(fm->dir, canon, sizeof fm->dir);
	build_tree(fm);
	load_list(fm, 0);
	fm_title(fm);
}

static void reload(struct fm *fm)
{
	struct vstat st;
	if (vfs_stat(fm->dir, &st) || st.type != VFS_DIR) {
		/* deleted from under us: up until something exists */
		char up[VFS_PATH_MAX];
		while (strlen(fm->dir) > 3) {
			vfs_dirname(fm->dir, up, sizeof up);
			strncpy0(fm->dir, up, sizeof fm->dir);
			if (!vfs_stat(fm->dir, &st))
				break;
		}
	}
	fm->ndrives = 0;
	vfs_drives(list_drive, fm);
	build_tree(fm);
	load_list(fm, 1);
	fm_title(fm);
}

/* this desktop's File Managers: look again */
void files_refresh(struct wm *wm)
{
	for (struct win *w = wm->wins; w; w = w->next)
		if (w->ops == &fm_ops && !w->closed) {
			reload(FM(w));
			fm_render(w);
		}
}

/* every desktop's: each does its own, under its own lock */
void files_refresh_all(void)
{
	for (struct wm *wm = all_wms; wm; wm = wm->next_wm)
		wm->want_files = 1;
}

/* the selection as '\n'-separated full paths ("" if none) */
static char *selection(struct fm *fm, int *count)
{
	size_t cap = 1;
	int n = 0;
	for (int i = 0; i < fm->n; i++)
		if (fm->ents[i].sel && strcmp(fm->ents[i].st.name, ".."))
			cap += VFS_PATH_MAX + 1;
	char *out = kmalloc(cap);
	if (!out)
		return NULL;
	out[0] = 0;
	size_t o = 0;
	for (int i = 0; i < fm->n; i++) {
		if (!fm->ents[i].sel || !strcmp(fm->ents[i].st.name, ".."))
			continue;
		char p[VFS_PATH_MAX];
		vfs_join(fm->dir, fm->ents[i].st.name, p, sizeof p);
		o += (size_t)snprintf(out + o, cap - o, "%s\n", p);
		n++;
	}
	if (count)
		*count = n;
	return out;
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void tiny_icon(struct surface *s, int x, int y, int dir, const char *name)
{
	if (dir) {
		fill(s, x, y + 3, 14, 10, P(0xE8C050));
		outline(s, x, y + 3, 14, 10, P(0));
		fill(s, x + 1, y + 1, 6, 3, P(0xE8C050));
	} else {
		const char *dot = strrchr(name, '.');
		int prog = dot && (!strcasecmp(dot, ".COM") || !strcasecmp(dot, ".EXE") || !strcasecmp(dot, ".BAT") ||
		    !strcasecmp(dot, ".BAS") || !strcasecmp(dot, ".C"));
		fill(s, x + 2, y, 10, 14, P(prog ? 0xC8D8F0 : 0xFFFFFF));
		outline(s, x + 2, y, 10, 14, P(0));
		if (prog)
			fill(s, x + 2, y, 10, 3, P(0x1D3A8A));
		else
			for (int k = 0; k < 3; k++)
				hline(s, x + 4, y + 4 + k * 3, 6, P(0x808080));
	}
}

static void fm_render(struct win *w)
{
	struct fm *fm = FM(w);
	struct surface *s = &w->content;
	int foc = w->wm->focus == w;
	fill(s, 0, 0, s->w, s->h, P(theme.face));
	fm->mb.open = w->wm->popup && w->wm->popup->owner == w ? fm->mb.open : -1;
	menubar_draw(s, &fm->mb, s->w);
	/* drive bar */
	int x = 6;
	for (int i = 0; i < fm->ndrives; i++) {
		char d[4] = { fm->drives[i], ':', 0 };
		int cur = toupper(fm->dir[0]) == fm->drives[i];
		fill(s, x, MENU_H + 3, 34, 20, P(cur ? theme.select : theme.face));
		bevel(s, x, MENU_H + 3, 34, 20, !cur);
		fill(s, x + 4, MENU_H + 10, 8, 6, P(cur ? 0xFFFFFF : 0x606060));
		text(s, x + 14, MENU_H + 5, d, P(cur ? 0xFFFFFF : 0));
		x += 38;
	}
	char where[VFS_PATH_MAX];
	strncpy0(where, fm->dir, sizeof where);
	int maxc = MAX(1, (s->w - x - 12) / FONT_W);
	if ((int)strlen(where) > maxc)
		memmove(where, where + strlen(where) - (size_t)maxc, (size_t)maxc + 1);
	text(s, x + 8, MENU_H + 5, where, P(0));

	/* the tree */
	struct rect tr = tree_rect(w);
	fill(s, tr.x, tr.y, tr.w, tr.h, P(0xFFFFFF));
	bevel(s, tr.x, tr.y, tr.w, tr.h, 0);
	fm->tsb = (struct scrollbar){ 1, { tr.x + tr.w - SB_W - 1, tr.y + 1, SB_W, tr.h - 2 }, fm->nrows * ROW_H + 4,
	    tr.h - 4, fm->tsb.pos, fm->tsb.dragging, fm->tsb.grab };
	sb_clamp(&fm->tsb);
	int tsb_on = fm->tsb.total > fm->tsb.visible;
	struct rect save = s->clip;
	surf_clip(s, (struct rect){ tr.x + 2, tr.y + 2, tr.w - 4 - (tsb_on ? SB_W : 0), tr.h - 4 });
	for (int i = 0; i < fm->nrows; i++) {
		struct trow *r = &fm->rows[i];
		int ry = tr.y + 2 + i * ROW_H - fm->tsb.pos;
		if (ry + ROW_H < tr.y || ry > tr.y + tr.h)
			continue;
		int rx = tr.x + 4 + r->depth * 14;
		/* the dotted lines to the parent */
		if (r->depth)
			for (int k = 0; k < 8; k += 2)
				plot(s, rx - 8 + k, ry + 9, P(0x808080));
		/* +/- */
		if (r->depth) {
			fill(s, rx - 2, ry + 5, 9, 9, P(0xFFFFFF));
			outline(s, rx - 2, ry + 5, 9, 9, P(0x808080));
			hline(s, rx, ry + 9, 5, P(0));
			if (!r->expanded)
				vline(s, rx + 2, ry + 7, 5, P(0));
		}
		tiny_icon(s, rx + 10, ry + 2, 1, "");
		int cur = !strcasecmp(r->path, fm->dir);
		int tw = (int)strlen(r->name) * FONT_W;
		if (cur)
			fill(s, rx + 27, ry + 1, tw + 4, ROW_H - 2, P(foc && fm->focus_tree ? theme.select : 0xC0C0C0));
		text(s, rx + 29, ry + 1, r->name, P(cur && foc && fm->focus_tree ? 0xFFFFFF : 0));
	}
	s->clip = save;
	if (tsb_on)
		sb_draw(s, &fm->tsb);

	/* the files */
	struct rect lr = list_rect(w);
	fill(s, lr.x, lr.y, lr.w, lr.h, P(0xFFFFFF));
	bevel(s, lr.x, lr.y, lr.w, lr.h, 0);
	fm->lsb = (struct scrollbar){ 1, { lr.x + lr.w - SB_W - 1, lr.y + 1, SB_W, lr.h - 2 }, fm->n * ROW_H + 4,
	    lr.h - 4, fm->lsb.pos, fm->lsb.dragging, fm->lsb.grab };
	sb_clamp(&fm->lsb);
	int lsb_on = fm->lsb.total > fm->lsb.visible;
	int inner_w = lr.w - 4 - (lsb_on ? SB_W : 0);
	surf_clip(s, (struct rect){ lr.x + 2, lr.y + 2, inner_w, lr.h - 4 });
	for (int i = 0; i < fm->n; i++) {
		struct fent *e = &fm->ents[i];
		int ry = lr.y + 2 + i * ROW_H - fm->lsb.pos;
		if (ry + ROW_H < lr.y || ry > lr.y + lr.h)
			continue;
		int hl = e->sel;
		if (hl)
			fill(s, lr.x + 2, ry, inner_w, ROW_H, P(foc && !fm->focus_tree ? theme.select : 0xC0C0C0));
		uint32_t ink = P(hl && foc && !fm->focus_tree ? 0xFFFFFF : 0);
		if (clip.cut) {
			char full[VFS_PATH_MAX];
			vfs_join(fm->dir, e->st.name, full, sizeof full);
			if (clip_has_cut(full))
				ink = P(hl && foc && !fm->focus_tree ? 0xD0D0D0 : 0x808080);	/* on its way */
		}
		tiny_icon(s, lr.x + 6, ry + 2, e->st.type == VFS_DIR, e->st.name);
		char name[64];
		strncpy0(name, e->st.name, sizeof name);
		int namecols = fm->details ? MAX(8, (inner_w - 30 - 200) / FONT_W) : (inner_w - 30) / FONT_W;
		if ((int)strlen(name) > namecols && namecols > 3) {
			name[namecols - 1] = '~';
			name[namecols] = 0;
		}
		text(s, lr.x + 24, ry + 1, name, ink);
		if (fm->details && strcmp(e->st.name, "..")) {
			char sz[24] = "", dt[24];
			if (e->st.type != VFS_DIR)
				fmt_commas(sz, sizeof sz, e->st.size);
			int y, mo, d, h, mi, sec;
			epoch_to_date(e->st.mtime, &y, &mo, &d, &h, &mi, &sec);
			snprintf(dt, sizeof dt, "%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
			int right = lr.x + 2 + inner_w;
			text(s, right - 16 * FONT_W - 4, ry + 1, dt, ink);
			text(s, right - 17 * FONT_W - 8 - (int)strlen(sz) * FONT_W, ry + 1, sz, ink);
		}
		if (i == fm->cursor && foc && !fm->focus_tree)
			for (int k = 0; k < inner_w; k += 2) {
				plot(s, lr.x + 2 + k, ry, P(hl ? 0xFFFFFF : 0));
				plot(s, lr.x + 2 + k, ry + ROW_H - 1, P(hl ? 0xFFFFFF : 0));
			}
	}
	if (fm->mode == FMM_BAND) {
		/* the box being dragged, dotted */
		int x0 = MIN(fm->bx0, fm->bx1), x1 = MAX(fm->bx0, fm->bx1);
		int y0 = MIN(fm->by0, fm->by1) - fm->lsb.pos, y1 = MAX(fm->by0, fm->by1) - fm->lsb.pos;
		for (int k = x0; k <= x1; k += 2) {
			plot(s, k, y0, P(0));
			plot(s, k, y1, P(0));
		}
		for (int k = y0; k <= y1; k += 2) {
			plot(s, x0, k, P(0));
			plot(s, x1, k, P(0));
		}
	}
	s->clip = save;
	if (lsb_on)
		sb_draw(s, &fm->lsb);

	/* status bar */
	int sy = s->h - STATUS_H;
	hline(s, 0, sy, s->w, P(0x808080));
	hline(s, 0, sy + 1, s->w, P(0xFFFFFF));
	int nsel = 0, nfiles = 0;
	uint64_t bytes = 0, selbytes = 0;
	for (int i = 0; i < fm->n; i++) {
		if (!strcmp(fm->ents[i].st.name, ".."))
			continue;
		nfiles++;
		bytes += fm->ents[i].st.size;
		if (fm->ents[i].sel) {
			nsel++;
			selbytes += fm->ents[i].st.size;
		}
	}
	char status[200], b1[32];
	fmt_commas(b1, sizeof b1, nsel ? selbytes : bytes);
	if (nsel)
		snprintf(status, sizeof status, "Selected %d of %d item(s), %s bytes", nsel, nfiles, b1);
	else
		snprintf(status, sizeof status, "%d item(s), %s bytes", nfiles, b1);
	if (clip.paths && clip.n) {
		char more[48];
		snprintf(more, sizeof more, "   %d %s - Paste puts %s here", clip.n, clip.cut ? "cut" : "copied",
		    clip.n == 1 ? "it" : "them");
		strcat0(status, more, sizeof status);
	}
	text(s, 8, sy + 3, status, P(0));
	win_dirty_all(w);
}

/* ---- operations -------------------------------------------------------------------------- */

struct batch {
	char *paths;
	char dest[VFS_PATH_MAX];
	int flags;
};

static void run_batch(struct wm *wm, struct batch *b)
{
	int errors = 0, last = 0;
	for (char *p = b->paths; *p; ) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (*p) {
			int r = fop_copy(p, b->dest, b->flags);
			if (r) {
				errors++;
				last = r;
			}
		}
		if (!e)
			break;
		p = e + 1;
	}
	if (errors) {
		char msg[160];
		snprintf(msg, sizeof msg, "%d item(s) couldn't be %s:\n%s", errors, (b->flags & FOP_MOVE) ? "moved" : "copied",
		    last == E_INVAL ? "a folder can't go inside itself." : vfs_strerror(last));
		wm_message(wm, "File Manager", msg);
	}
	kfree(b->paths);
	kfree(b);
	files_refresh_all();
	progman_reload(wm);
}

static void replace_done(struct wm *wm, void *arg, int yes)
{
	struct batch *b = arg;
	if (!yes) {
		kfree(b->paths);
		kfree(b);
		return;
	}
	b->flags |= FOP_REPLACE;
	run_batch(wm, b);
}

/* copy or move a '\n' list of paths into dest, asking before replacing anything */
static void transfer(struct wm *wm, const char *paths, const char *dest, int move)
{
	struct batch *b = kzalloc(sizeof *b);
	if (!b)
		return;
	b->paths = kstrdup(paths);
	strncpy0(b->dest, dest, sizeof b->dest);
	b->flags = move ? FOP_MOVE : FOP_DUP;
	int clashes = 0;
	char *copy = kstrdup(paths);
	for (char *p = copy; p && *p; ) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (*p && fop_exists_in(p, dest))
			clashes++;
		if (!e)
			break;
		p = e + 1;
	}
	kfree(copy);
	if (clashes) {
		char msg[200];
		snprintf(msg, sizeof msg, "%d item(s) with the same name%s already in\n%s\n\nReplace them?", clashes,
		    clashes == 1 ? " is" : "s are", dest);
		ui_confirm(wm, move ? "Confirm Move" : "Confirm Copy", msg, "Replace", replace_done, b);
		return;
	}
	run_batch(wm, b);
}

struct fm_ref {
	struct win *w;
	char *paths;
	int move;
};

static int fm_alive(struct wm *wm, struct win *w)
{
	for (struct win *v = wm->wins; v; v = v->next)
		if (v == w)
			return 1;
	return 0;
}

static void copyto_done(struct wm *wm, void *arg, const char *text)
{
	struct fm_ref *r = arg;
	char dest[VFS_PATH_MAX];
	const char *cwd = r->w && fm_alive(wm, r->w) ? FM(r->w)->dir : "C:\\";
	if (*text && !vfs_resolve(cwd, text, dest, sizeof dest)) {
		struct vstat st;
		if (vfs_stat(dest, &st) || st.type != VFS_DIR)
			wm_message(wm, "File Manager", "Copy and Move need an existing folder to put things in.");
		else
			transfer(wm, r->paths, dest, r->move);
	}
	kfree(r->paths);
	kfree(r);
}

static void ask_transfer(struct fm *fm, int move)
{
	int n;
	char *paths = selection(fm, &n);
	if (!paths || !n) {
		kfree(paths);
		return;
	}
	struct fm_ref *r = kzalloc(sizeof *r);
	if (!r) {
		kfree(paths);
		return;
	}
	r->w = fm->w;
	r->paths = paths;
	r->move = move;
	char label[80];
	snprintf(label, sizeof label, "%s %d item(s) to:", move ? "Move" : "Copy", n);
	ui_prompt(fm->w->wm, move ? "Move" : "Copy", label, fm->dir, copyto_done, r);
}

/* Cut, Copy: the selection onto the clipboard */
static void clip_take(struct fm *fm, int cut)
{
	int n;
	char *paths = selection(fm, &n);
	if (!paths || !n) {
		kfree(paths);
		return;
	}
	kfree(clip.paths);
	clip.paths = paths;
	clip.n = n;
	clip.cut = cut;
	files_refresh_all();		/* cut files grey out, everywhere */
}

/* Paste: what's on the clipboard into dest (copies stay; cut ones move) */
static void clip_paste(struct fm *fm, const char *dest)
{
	if (!clip.paths || !clip.n)
		return;
	transfer(fm->w->wm, clip.paths, dest, clip.cut);
	if (clip.cut) {			/* moved: nothing left to paste */
		kfree(clip.paths);
		clip.paths = NULL;
		clip.n = clip.cut = 0;
	}
}

static void delete_done(struct wm *wm, void *arg, int yes)
{
	struct fm_ref *r = arg;
	if (yes) {
		int errors = 0, last = 0;
		for (char *p = r->paths; *p; ) {
			char *e = strchr(p, '\n');
			if (e)
				*e = 0;
			if (*p) {
				int x = fop_delete(p);
				if (x) {
					errors++;
					last = x;
				}
			}
			if (!e)
				break;
			p = e + 1;
		}
		if (errors) {
			char msg[120];
			snprintf(msg, sizeof msg, "%d item(s) couldn't be deleted:\n%s", errors, vfs_strerror(last));
			wm_message(wm, "File Manager", msg);
		}
		files_refresh_all();
		progman_reload(wm);
	}
	kfree(r->paths);
	kfree(r);
}

static void ask_delete(struct fm *fm)
{
	int n;
	char *paths = selection(fm, &n);
	if (!paths || !n) {
		kfree(paths);
		return;
	}
	int dirs = 0;
	const char *first = NULL;
	for (int i = 0; i < fm->n; i++)
		if (fm->ents[i].sel && strcmp(fm->ents[i].st.name, "..")) {
			dirs += fm->ents[i].st.type == VFS_DIR;
			if (!first)
				first = fm->ents[i].st.name;
		}
	char msg[300];
	if (n == 1)
		snprintf(msg, sizeof msg, "Delete %s '%s'%s?", dirs ? "the folder" : "", first,
		    dirs ? " and everything in it" : "");
	else
		snprintf(msg, sizeof msg, "Delete these %d items%s?", n, dirs ? ", including folders and everything in them" : "");
	struct fm_ref *r = kzalloc(sizeof *r);
	if (!r) {
		kfree(paths);
		return;
	}
	r->paths = paths;
	ui_confirm(fm->w->wm, "Confirm Delete", msg, "Delete", delete_done, r);
}

struct rename_req {
	char from[VFS_PATH_MAX];
	char dir[VFS_PATH_MAX];
};

static void rename_done(struct wm *wm, void *arg, const char *name)
{
	struct rename_req *r = arg;
	if (*name && !strchr(name, '\\') && !strchr(name, '/') && !strchr(name, ':')) {
		char to[VFS_PATH_MAX];
		vfs_join(r->dir, name, to, sizeof to);
		int x = vfs_rename(r->from, to);
		if (x)
			wm_message(wm, "Rename", x == E_EXIST ? "There's already something with that name." : vfs_strerror(x));
		files_refresh_all();
		progman_reload(wm);
	} else if (*name) {
		wm_message(wm, "Rename", "A name can't contain \\ / or :.");
	}
	kfree(r);
}

static struct fent *one_selected(struct fm *fm)
{
	struct fent *f = NULL;
	for (int i = 0; i < fm->n; i++)
		if (fm->ents[i].sel && strcmp(fm->ents[i].st.name, "..")) {
			if (f)
				return NULL;
			f = &fm->ents[i];
		}
	return f;
}

static void ask_rename(struct fm *fm)
{
	struct fent *e = one_selected(fm);
	if (!e) {
		wm_message(fm->w->wm, "Rename", "Select one file or folder to rename.");
		return;
	}
	struct rename_req *r = kzalloc(sizeof *r);
	if (!r)
		return;
	vfs_join(fm->dir, e->st.name, r->from, sizeof r->from);
	strncpy0(r->dir, fm->dir, sizeof r->dir);
	char label[80];
	snprintf(label, sizeof label, "Rename %s to:", e->st.name);
	ui_prompt(fm->w->wm, "Rename", label, e->st.name, rename_done, r);
}

static void mkdir_done(struct wm *wm, void *arg, const char *name)
{
	char *dir = arg;
	if (*name) {
		char p[VFS_PATH_MAX];
		if (vfs_join(dir, name, p, sizeof p) || strchr(name, '\\') || strchr(name, '/'))
			wm_message(wm, "New Folder", "That isn't a name a folder can have.");
		else {
			int r = vfs_mkdir(p);
			if (r)
				wm_message(wm, "New Folder", r == E_EXIST ? "There's already something with that name." : vfs_strerror(r));
		}
		files_refresh_all();
	}
	kfree(dir);
}

static void ask_mkdir(struct fm *fm)
{
	char *dir = kstrdup(fm->dir);
	if (dir)
		ui_prompt(fm->w->wm, "New Folder", "Name of the new folder:", "", mkdir_done, dir);
}

/* -- properties -- */

struct fprops {
	char path[VFS_PATH_MAX];
	int name, ro, hidden, sys, arch, ok, cancel;
	int speed;		/* DOS programs: their pace, -1 when not a DOS program */
};

/* what a DOS program can be held to (see dos.speed) */
static const char *const speed_names[] = { "Default", "XT (4.77 MHz)", "AT (286)", "386", "486", "Pentium", "As fast as possible" };
static const char *const speed_keys[] = { "", "xt", "at", "386", "486", "pentium", "max" };

static int is_dos_program(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!strcasecmp(dot, ".COM") || !strcasecmp(dot, ".EXE"));
}

static void fprops_event(struct form *f, int id, int ev)
{
	struct fprops *p = form_priv(f);
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
	struct vstat st;
	if (!vfs_stat(p->path, &st)) {
		uint8_t a = st.attr & ~(A_RO | A_HIDDEN | A_SYSTEM | A_ARCHIVE);
		a |= form_val(f, p->ro) ? A_RO : 0;
		a |= form_val(f, p->hidden) ? A_HIDDEN : 0;
		a |= form_val(f, p->sys) ? A_SYSTEM : 0;
		a |= form_val(f, p->arch) ? A_ARCHIVE : 0;
		vfs_setattr(p->path, a);
	}
	if (p->speed >= 0) {
		long v = form_val(f, p->speed);
		if (v >= 0 && v < (long)NELEM(speed_keys))
			vfs_xattr_set(p->path, "dos.speed", speed_keys[v]);
	}
	const char *nn = form_text(f, p->name);
	if (*nn && strcasecmp(nn, vfs_basename(p->path))) {
		char dir[VFS_PATH_MAX], to[VFS_PATH_MAX];
		vfs_dirname(p->path, dir, sizeof dir);
		vfs_join(dir, nn, to, sizeof to);
		int r = vfs_rename(p->path, to);
		if (r) {
			wm_message(form_wm(f), "Properties", vfs_strerror(r));
			return;
		}
	}
	form_close(f);
	files_refresh_all();
	progman_reload(form_wm(f));
}

static void open_fprops(struct fm *fm)
{
	struct fent *e = one_selected(fm);
	if (!e)
		return;
	struct fprops *p = kzalloc(sizeof *p);
	if (!p)
		return;
	vfs_join(fm->dir, e->st.name, p->path, sizeof p->path);
	char title[80];
	snprintf(title, sizeof title, "Properties - %s", e->st.name);
	int dos = e->st.type != VFS_DIR && is_dos_program(e->st.name);
	int cw = 440, ch = dos ? 250 : 210;
	struct form *f = form_new(title, cw, ch, fprops_event, p);
	if (!f) {
		kfree(p);
		return;
	}
	form_add(f, FW_LABEL, 14, 18, 0, 0, "Name:");
	p->name = form_add(f, FW_EDIT, 80, 14, cw - 94, 24, e->st.name);
	char info[120], sz[32];
	fmt_commas(sz, sizeof sz, e->st.size);
	int y, mo, d, h, mi, s;
	epoch_to_date(e->st.mtime, &y, &mo, &d, &h, &mi, &s);
	if (e->st.type == VFS_DIR)
		snprintf(info, sizeof info, "Folder.  Last changed %04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
	else
		snprintf(info, sizeof info, "%s bytes.  Last changed %04d-%02d-%02d %02d:%02d", sz, y, mo, d, h, mi);
	form_add(f, FW_LABEL, 14, 50, 0, 0, info);
	form_add(f, FW_LABEL, 14, 70, 0, 0, fm->dir);
	form_add(f, FW_GROUP, 8, 92, cw - 16, 66, "Attributes");
	p->ro = form_add(f, FW_CHECK, 20, 112, 140, 20, "Read only");
	p->hidden = form_add(f, FW_CHECK, 20, 132, 140, 20, "Hidden");
	p->sys = form_add(f, FW_CHECK, 220, 112, 140, 20, "System");
	p->arch = form_add(f, FW_CHECK, 220, 132, 140, 20, "Archive");
	form_set_val(f, p->ro, (e->st.attr & A_RO) != 0);
	form_set_val(f, p->hidden, (e->st.attr & A_HIDDEN) != 0);
	form_set_val(f, p->sys, (e->st.attr & A_SYSTEM) != 0);
	form_set_val(f, p->arch, (e->st.attr & A_ARCHIVE) != 0);
	p->speed = -1;
	if (dos) {
		form_add(f, FW_LABEL, 14, 172, 0, 0, "Run at:");
		p->speed = form_add(f, FW_CHOICE, 80, 168, 220, 24, NULL);
		form_options(f, p->speed, speed_names, (int)NELEM(speed_names));
		char v[32] = "";
		vfs_xattr_get(p->path, "dos.speed", v, sizeof v);
		int sel = 0;
		for (int i = 1; i < (int)NELEM(speed_keys); i++)
			if (!strcasecmp(v, speed_keys[i]))
				sel = i;
		form_set_val(f, p->speed, sel);
	}
	p->ok = form_add(f, FW_BUTTON, cw - 184, ch - 38, 80, 26, "OK");
	p->cancel = form_add(f, FW_BUTTON, cw - 96, ch - 38, 80, 26, "Cancel");
	form_default(f, p->ok);
	form_show(fm->w->wm, f, -1, -1);
}

static void open_entry(struct fm *fm, int i)
{
	if (i < 0 || i >= fm->n)
		return;
	struct fent *e = &fm->ents[i];
	char p[VFS_PATH_MAX];
	if (!strcmp(e->st.name, "..")) {
		vfs_dirname(fm->dir, p, sizeof p);
		go(fm, p);
		return;
	}
	vfs_join(fm->dir, e->st.name, p, sizeof p);
	if (e->st.type == VFS_DIR)
		go(fm, p);
	else
		launch_path(fm->w->wm, p);
}

static void select_only(struct fm *fm, int i)
{
	for (int k = 0; k < fm->n; k++)
		fm->ents[k].sel = k == i;
	fm->cursor = fm->anchor = i;
}

static void keep_cursor_visible(struct fm *fm)
{
	int y = fm->cursor * ROW_H;
	if (y < fm->lsb.pos)
		fm->lsb.pos = y;
	if (y + ROW_H > fm->lsb.pos + fm->lsb.visible)
		fm->lsb.pos = y + ROW_H - fm->lsb.visible + 4;
	sb_clamp(&fm->lsb);
}

/* ---- menus ----------------------------------------------------------------------------- */

enum {
	FM_OPEN = 1, FM_MOVE, FM_COPY, FM_DELETE, FM_RENAME, FM_PROPS, FM_MKDIR, FM_SELALL, FM_CLOSE,
	FM_NAMES, FM_DETAILS, FM_BYNAME, FM_BYTYPE, FM_BYSIZE, FM_BYDATE, FM_HIDDEN, FM_REFRESH, FM_NEWWIN,
	FM_EDIT, FM_TREE_OPEN, FM_UP, FM_CUT, FM_CLIPCOPY, FM_PASTE, FM_PASTE_INTO, FM_SELNONE, FM_INVERT,
};

static void fm_pick(struct wm *wm, void *arg, int id)
{
	struct win *w = arg;
	if (!fm_alive(wm, w))
		return;
	struct fm *fm = FM(w);
	switch (id) {
	case FM_OPEN: open_entry(fm, fm->cursor); break;
	case FM_EDIT: {
		char p[VFS_PATH_MAX], cmd[VFS_PATH_MAX + 16];
		if (fm->cursor < fm->n) {
			vfs_join(fm->dir, fm->ents[fm->cursor].st.name, p, sizeof p);
			snprintf(cmd, sizeof cmd, "EDIT \"%s\"", p);
			launch_cmd(wm, fm->ents[fm->cursor].st.name, cmd);
		}
		break;
	}
	case FM_MOVE: ask_transfer(fm, 1); break;
	case FM_COPY: ask_transfer(fm, 0); break;
	case FM_DELETE: ask_delete(fm); break;
	case FM_RENAME: ask_rename(fm); break;
	case FM_PROPS: open_fprops(fm); break;
	case FM_MKDIR: ask_mkdir(fm); break;
	case FM_SELALL:
		for (int i = 0; i < fm->n; i++)
			fm->ents[i].sel = strcmp(fm->ents[i].st.name, "..") != 0;
		break;
	case FM_SELNONE:
		for (int i = 0; i < fm->n; i++)
			fm->ents[i].sel = 0;
		break;
	case FM_INVERT:
		for (int i = 0; i < fm->n; i++)
			fm->ents[i].sel = !fm->ents[i].sel && strcmp(fm->ents[i].st.name, "..");
		break;
	case FM_CUT: clip_take(fm, 1); break;
	case FM_CLIPCOPY: clip_take(fm, 0); break;
	case FM_PASTE: clip_paste(fm, fm->dir); return;
	case FM_PASTE_INTO: {
		struct fent *e = fm->cursor < fm->n ? &fm->ents[fm->cursor] : NULL;
		char p[VFS_PATH_MAX];
		if (e && e->st.type == VFS_DIR && strcmp(e->st.name, "..") &&
		    !vfs_join(fm->dir, e->st.name, p, sizeof p))
			clip_paste(fm, p);
		return;
	}
	case FM_CLOSE: win_destroy(wm, w); return;
	case FM_NAMES: fm->details = 0; break;
	case FM_DETAILS: fm->details = 1; break;
	case FM_BYNAME: fm->sort = SORT_NAME; load_list(fm, 1); break;
	case FM_BYTYPE: fm->sort = SORT_TYPE; load_list(fm, 1); break;
	case FM_BYSIZE: fm->sort = SORT_SIZE; load_list(fm, 1); break;
	case FM_BYDATE: fm->sort = SORT_DATE; load_list(fm, 1); break;
	case FM_HIDDEN: fm->hidden = !fm->hidden; reload(fm); break;
	case FM_REFRESH: reload(fm); break;
	case FM_NEWWIN: open_files(wm, fm->dir); return;
	case FM_UP: {
		char p[VFS_PATH_MAX];
		vfs_dirname(fm->dir, p, sizeof p);
		go(fm, p);
		break;
	}
	}
	if (fm_alive(wm, w))
		fm_render(w);
}

static void fm_menu(struct fm *fm, int which, int x, int y)
{
	int nsel = 0;
	for (int i = 0; i < fm->n; i++)
		nsel += fm->ents[i].sel && strcmp(fm->ents[i].st.name, "..");
	int one = nsel == 1;
	struct menu_item it[MENU_MAX];
	int n = 0;
#define ADD(l, f, i) menu_put(it, &n, l, f, i)
	if (which == 0) {
		ADD("Open\tEnter", fm->n ? 0 : MI_DISABLED, FM_OPEN);
		ADD("Move To...\tF7", nsel ? 0 : MI_DISABLED, FM_MOVE);
		ADD("Copy To...\tF8", nsel ? 0 : MI_DISABLED, FM_COPY);
		ADD("Delete...\tDel", nsel ? 0 : MI_DISABLED, FM_DELETE);
		ADD("Rename...\tF2", one ? 0 : MI_DISABLED, FM_RENAME);
		ADD("Properties...", one ? 0 : MI_DISABLED, FM_PROPS);
		ADD("", MI_SEP, 0);
		ADD("New Folder...\tCtrl+N", 0, FM_MKDIR);
		ADD("", MI_SEP, 0);
		ADD("Close", 0, FM_CLOSE);
	} else if (which == 1) {
		ADD("Cut\tCtrl+X", nsel ? 0 : MI_DISABLED, FM_CUT);
		ADD("Copy\tCtrl+C", nsel ? 0 : MI_DISABLED, FM_CLIPCOPY);
		ADD("Paste\tCtrl+V", clip.n ? 0 : MI_DISABLED, FM_PASTE);
		ADD("", MI_SEP, 0);
		ADD("Select All\tCtrl+A", 0, FM_SELALL);
		ADD("Select None", nsel ? 0 : MI_DISABLED, FM_SELNONE);
		ADD("Invert Selection", 0, FM_INVERT);
	} else {
		ADD("Name Only", fm->details ? 0 : MI_CHECKED, FM_NAMES);
		ADD("All File Details", fm->details ? MI_CHECKED : 0, FM_DETAILS);
		ADD("", MI_SEP, 0);
		ADD("Sort by Name", fm->sort == SORT_NAME ? MI_CHECKED : 0, FM_BYNAME);
		ADD("Sort by Type", fm->sort == SORT_TYPE ? MI_CHECKED : 0, FM_BYTYPE);
		ADD("Sort by Size", fm->sort == SORT_SIZE ? MI_CHECKED : 0, FM_BYSIZE);
		ADD("Sort by Date", fm->sort == SORT_DATE ? MI_CHECKED : 0, FM_BYDATE);
		ADD("", MI_SEP, 0);
		ADD("Show Hidden Files", fm->hidden ? MI_CHECKED : 0, FM_HIDDEN);
		ADD("Refresh\tF5", 0, FM_REFRESH);
		ADD("New Window", 0, FM_NEWWIN);
	}
#undef ADD
	fm->mb.open = which;
	popup_open(fm->w->wm, x, y, it, n, fm_pick, fm->w, fm->w);
	fm_render(fm->w);
}

/* ---- input ----------------------------------------------------------------------------------- */

static int list_index(struct fm *fm, struct rect lr, int y)
{
	int i = (y - lr.y - 2 + fm->lsb.pos) / ROW_H;
	return y >= lr.y + 2 && i >= 0 && i < fm->n ? i : -1;
}

static int tree_index(struct fm *fm, struct rect tr, int y)
{
	int i = (y - tr.y - 2 + fm->tsb.pos) / ROW_H;
	return y >= tr.y + 2 && i >= 0 && i < fm->nrows ? i : -1;
}

static void fm_mouse(struct win *w, int type, int x, int y, int buttons, int dbl)
{
	struct fm *fm = FM(w);
	struct rect tr = tree_rect(w), lr = list_rect(w);
	if (type == EV_DOWN) {
		fm->mode = FMM_IDLE;
		if (y < MENU_H) {
			int tx, tw, m = menubar_hit(&fm->mb, x, &tx, &tw);
			if (m >= 0)
				fm_menu(fm, m, w->x + CLIENT_X + tx, w->y + CLIENT_Y + MENU_H);
			return;
		}
		if (y < MENU_H + DRIVEBAR_H) {
			int k = (x - 6) / 38;
			if (x >= 6 && k >= 0 && k < fm->ndrives && (x - 6) % 38 < 34) {
				char root[4] = { fm->drives[k], ':', '\\', 0 };
				go(fm, root);
			}
			fm_render(w);
			return;
		}
		if (rect_has(tr, x, y)) {
			fm->focus_tree = 1;
			if (fm->tsb.total > fm->tsb.visible && x >= fm->tsb.r.x) {
				fm->mode = FMM_TREE_SB;
				sb_mouse(&fm->tsb, EV_DOWN, x, y, ROW_H);
			} else {
				int i = tree_index(fm, tr, y);
				if (i >= 0) {
					struct trow *r = &fm->rows[i];
					int rx = tr.x + 4 + r->depth * 14;
					int on_box = r->depth && x >= rx - 4 && x < rx + 9;
					if (on_box || (dbl && r->depth)) {
						set_open(fm, r->path, !r->expanded);
						build_tree(fm);
					} else if (strcasecmp(r->path, fm->dir)) {
						strncpy0(fm->dir, r->path, sizeof fm->dir);
						build_tree(fm);
						load_list(fm, 0);
						fm_title(fm);
					}
				}
			}
			fm_render(w);
			return;
		}
		if (rect_has(lr, x, y)) {
			fm->focus_tree = 0;
			if (fm->lsb.total > fm->lsb.visible && x >= fm->lsb.r.x) {
				fm->mode = FMM_LIST_SB;
				sb_mouse(&fm->lsb, EV_DOWN, x, y, ROW_H);
				fm_render(w);
				return;
			}
			int i = list_index(fm, lr, y);
			if (i < 0) {
				/* empty space: drag a box to select (Ctrl: add to what's selected) */
				for (int k = 0; k < fm->n; k++) {
					if (!(buttons & MOD_CTRL))
						fm->ents[k].sel = 0;
					fm->ents[k].was = fm->ents[k].sel;
				}
				fm->mode = FMM_BAND;
				fm->bx0 = fm->bx1 = x;
				fm->by0 = fm->by1 = y + fm->lsb.pos;
			} else if (buttons & MOD_CTRL) {
				fm->ents[i].sel = !fm->ents[i].sel;
				fm->cursor = fm->anchor = i;
			} else if (buttons & MOD_SHIFT) {
				int a = MIN(fm->anchor, i), b = MAX(fm->anchor, i);
				for (int k = 0; k < fm->n; k++)
					fm->ents[k].sel = k >= a && k <= b;
				fm->cursor = i;
			} else {
				if (!fm->ents[i].sel)
					select_only(fm, i);
				fm->cursor = i;
				if (dbl)
					open_entry(fm, i);
				else {
					fm->mode = FMM_PRESS;	/* a drag, maybe */
					fm->px = x;
					fm->py = y;
					fm->press_item = i;
				}
			}
			if (fm_alive(w->wm, w))
				fm_render(w);
			return;
		}
		return;
	}
	if (type == EV_MOVE && (buttons & 1)) {
		if (fm->mode == FMM_PRESS) {
			int dx = x - fm->px, dy = y - fm->py;
			if (dx * dx + dy * dy > 25) {
				fm->mode = FMM_IDLE;
				int n;
				char *paths = selection(fm, &n);
				if (paths && n) {
					struct fent *e = &fm->ents[fm->press_item];
					wm_dnd_start(w->wm, w, paths, e->st.type == VFS_DIR ? "folder" : icon_for_file(e->st.name, 0));
				}
				kfree(paths);
			}
		} else if (fm->mode == FMM_BAND) {
			x = MAX(lr.x + 2, MIN(x, lr.x + lr.w - 3));
			y = MAX(lr.y + 2, MIN(y, lr.y + lr.h - 3));
			/* past the edge: scroll */
			if (y <= lr.y + 2)
				fm->lsb.pos -= ROW_H;
			else if (y >= lr.y + lr.h - 3)
				fm->lsb.pos += ROW_H;
			sb_clamp(&fm->lsb);
			fm->bx1 = x;
			fm->by1 = y + fm->lsb.pos;
			int top = MIN(fm->by0, fm->by1) - lr.y - 2, bot = MAX(fm->by0, fm->by1) - lr.y - 2;
			for (int k = 0; k < fm->n; k++) {
				int in = k * ROW_H + ROW_H > top && k * ROW_H <= bot && strcmp(fm->ents[k].st.name, "..");
				fm->ents[k].sel = fm->ents[k].was || in;
			}
			fm_render(w);
		} else if (fm->mode == FMM_TREE_SB) {
			if (sb_mouse(&fm->tsb, EV_MOVE, x, y, 0))
				fm_render(w);
		} else if (fm->mode == FMM_LIST_SB) {
			if (sb_mouse(&fm->lsb, EV_MOVE, x, y, 0))
				fm_render(w);
		}
		return;
	}
	if (type == EV_UP) {
		if (fm->mode == FMM_PRESS) {
			/* a click (not a drag) on something already selected: just that one */
			if (!(buttons & (MOD_CTRL | MOD_SHIFT)))
				select_only(fm, fm->press_item);
		}
		sb_mouse(&fm->tsb, EV_UP, x, y, 0);
		sb_mouse(&fm->lsb, EV_UP, x, y, 0);
		fm->mode = FMM_IDLE;
		fm_render(w);
	}
}

static void fm_context(struct win *w, int x, int y)
{
	struct fm *fm = FM(w);
	struct rect tr = tree_rect(w), lr = list_rect(w);
	int sx = w->x + CLIENT_X + x, sy = w->y + CLIENT_Y + y;
	struct menu_item it[20];
	int n = 0;
#define ADD(l, f, i) menu_put(it, &n, l, f, i)
	if (rect_has(lr, x, y)) {
		fm->focus_tree = 0;
		int i = list_index(fm, lr, y);
		if (i >= 0 && strcmp(fm->ents[i].st.name, "..")) {
			if (!fm->ents[i].sel)
				select_only(fm, i);
			fm->cursor = i;
			int nsel = 0;
			for (int k = 0; k < fm->n; k++)
				nsel += fm->ents[k].sel;
			int dir = fm->ents[i].st.type == VFS_DIR;
			ADD("Open", 0, FM_OPEN);
			if (!dir)
				ADD("Edit", 0, FM_EDIT);
			ADD("", MI_SEP, 0);
			ADD("Cut\tCtrl+X", 0, FM_CUT);
			ADD("Copy\tCtrl+C", 0, FM_CLIPCOPY);
			if (dir && nsel == 1)
				ADD("Paste Into Folder", clip.n ? 0 : MI_DISABLED, FM_PASTE_INTO);
			ADD("", MI_SEP, 0);
			ADD("Copy To...\tF8", 0, FM_COPY);
			ADD("Move To...\tF7", 0, FM_MOVE);
			ADD("Rename...\tF2", nsel == 1 ? 0 : MI_DISABLED, FM_RENAME);
			ADD("Delete...\tDel", 0, FM_DELETE);
			ADD("", MI_SEP, 0);
			ADD("Properties...", nsel == 1 ? 0 : MI_DISABLED, FM_PROPS);
		} else {
			ADD("Paste\tCtrl+V", clip.n ? 0 : MI_DISABLED, FM_PASTE);
			ADD("", MI_SEP, 0);
			ADD("New Folder...\tCtrl+N", 0, FM_MKDIR);
			ADD("Select All\tCtrl+A", 0, FM_SELALL);
			ADD("Refresh\tF5", 0, FM_REFRESH);
			ADD("", MI_SEP, 0);
			ADD("Up a Folder\tBksp", strlen(fm->dir) > 3 ? 0 : MI_DISABLED, FM_UP);
			ADD("New Window", 0, FM_NEWWIN);
		}
	} else if (rect_has(tr, x, y)) {
		int i = tree_index(fm, tr, y);
		if (i >= 0 && strcasecmp(fm->rows[i].path, fm->dir)) {
			strncpy0(fm->dir, fm->rows[i].path, sizeof fm->dir);
			build_tree(fm);
			load_list(fm, 0);
			fm_title(fm);
		}
		fm->focus_tree = 1;
		ADD("New Window", 0, FM_NEWWIN);
		ADD("New Folder...\tCtrl+N", 0, FM_MKDIR);
		ADD("Refresh\tF5", 0, FM_REFRESH);
	} else {
		control_menu(w->wm, w, sx, sy);
		return;
	}
#undef ADD
	fm_render(w);
	popup_open(w->wm, sx, sy, it, n, fm_pick, w, w);
}

static void fm_key(struct win *w, int k)
{
	struct fm *fm = FM(w);
	int shift = input_mods() & 1;
	if (k == '\t' || k == KEY_SHIFT_TAB) {
		fm->focus_tree = !fm->focus_tree;
	} else if (fm->focus_tree) {
		int cur = 0;
		for (int i = 0; i < fm->nrows; i++)
			if (!strcasecmp(fm->rows[i].path, fm->dir))
				cur = i;
		if ((k == KEY_UP && cur > 0) || (k == KEY_DOWN && cur + 1 < fm->nrows)) {
			strncpy0(fm->dir, fm->rows[cur + (k == KEY_DOWN ? 1 : -1)].path, sizeof fm->dir);
			build_tree(fm);
			load_list(fm, 0);
			fm_title(fm);
		} else if (k == KEY_RIGHT || k == KEY_LEFT || k == '\n') {
			set_open(fm, fm->dir, k == KEY_RIGHT || (k == '\n' && !is_open(fm, fm->dir)));
			build_tree(fm);
		}
	} else if (k == KEY_UP || k == KEY_DOWN || k == KEY_HOME || k == KEY_END || k == KEY_PGUP || k == KEY_PGDN) {
		int page = MAX(1, fm->lsb.visible / ROW_H - 1);
		int c = fm->cursor;
		if (k == KEY_UP) c--;
		else if (k == KEY_DOWN) c++;
		else if (k == KEY_HOME) c = 0;
		else if (k == KEY_END) c = fm->n - 1;
		else if (k == KEY_PGUP) c -= page;
		else c += page;
		c = MAX(0, MIN(c, fm->n - 1));
		if (shift) {
			int a = MIN(fm->anchor, c), b = MAX(fm->anchor, c);
			for (int i = 0; i < fm->n; i++)
				fm->ents[i].sel = i >= a && i <= b;
			fm->cursor = c;
		} else {
			select_only(fm, c);
		}
		keep_cursor_visible(fm);
	} else if (k == ' ' && fm->cursor < fm->n) {
		fm->ents[fm->cursor].sel = !fm->ents[fm->cursor].sel;
	} else if (k == '\n') {
		open_entry(fm, fm->cursor);
	} else if (k == '\b') {
		fm_pick(w->wm, w, FM_UP);
		return;
	} else if (k == KEY_DEL) {
		ask_delete(fm);
	} else if (k == KEY_F1 + 1) {
		ask_rename(fm);
	} else if (k == KEY_F1 + 4) {
		reload(fm);
	} else if (k == KEY_F1 + 6) {
		ask_transfer(fm, 1);
	} else if (k == KEY_F1 + 7) {
		ask_transfer(fm, 0);
	} else if (k == 14) {	/* Ctrl-N */
		ask_mkdir(fm);
	} else if (k == 1) {	/* Ctrl-A */
		fm_pick(w->wm, w, FM_SELALL);
		return;
	} else if (k == 24 || k == 3) {	/* Ctrl-X, Ctrl-C */
		clip_take(fm, k == 24);
	} else if (k == 22) {		/* Ctrl-V */
		clip_paste(fm, fm->dir);
		return;
	}
	if (fm_alive(w->wm, w))
		fm_render(w);
}

/* things dropped here: onto a folder in the list or tree, or into this folder */
static int fm_drop(struct win *w, int x, int y, const char *paths, int copy)
{
	struct fm *fm = FM(w);
	struct rect tr = tree_rect(w), lr = list_rect(w);
	char dest[VFS_PATH_MAX];
	strncpy0(dest, fm->dir, sizeof dest);
	if (rect_has(lr, x, y)) {
		int i = list_index(fm, lr, y);
		if (i >= 0 && fm->ents[i].st.type == VFS_DIR) {
			if (!strcmp(fm->ents[i].st.name, ".."))
				vfs_dirname(fm->dir, dest, sizeof dest);
			else
				vfs_join(fm->dir, fm->ents[i].st.name, dest, sizeof dest);
		}
	} else if (rect_has(tr, x, y)) {
		int i = tree_index(fm, tr, y);
		if (i >= 0)
			strncpy0(dest, fm->rows[i].path, sizeof dest);
	} else if (y >= MENU_H && y < MENU_H + DRIVEBAR_H) {
		int k = (x - 6) / 38;
		if (k >= 0 && k < fm->ndrives)
			snprintf(dest, sizeof dest, "%c:\\", fm->drives[k]);
	}
	/* Windows 3.1's rule: same drive moves, another drive copies; Ctrl copies */
	int move = !copy && toupper(paths[0]) == toupper(dest[0]);
	transfer(w->wm, paths, dest, move);
	return 1;
}

static void fm_destroy(struct win *w)
{
	struct fm *fm = FM(w);
	kfree(fm->rows);
	kfree(fm->open);
	kfree(fm->ents);
	kfree(fm);
	w->priv = NULL;
}

static void fm_wheel(struct win *w, int x, int y, int clicks)
{
	struct fm *fm = w->priv;
	if (!fm)
		return;
	struct scrollbar *sb = rect_has(tree_rect(w), x, y) ? &fm->tsb : &fm->lsb;
	sb->pos -= clicks * 3 * ROW_H;
	sb_clamp(sb);
	fm_render(w);
}

static const struct winops fm_ops = {
	.key = fm_key,
	.wheel = fm_wheel,
	.mouse = fm_mouse,
	.context = fm_context,
	.resized = fm_render,
	.repaint = fm_render,
	.destroy = fm_destroy,
	.drop = fm_drop,
	.icon = "folder",
};

struct win *open_files(struct wm *wm, const char *dir)
{
	struct fm *fm = kzalloc(sizeof *fm);
	if (!fm)
		return NULL;
	fm->mb = (struct menubar){ fm_menus, 3, -1 };
	fm->details = 1;
	fm->tsb.vertical = fm->lsb.vertical = 1;
	char canon[VFS_PATH_MAX];
	if (vfs_canon(dir, canon, sizeof canon))
		strncpy0(canon, "C:\\", sizeof canon);
	strncpy0(fm->dir, canon, sizeof fm->dir);
	int sw = wm->disp->surf->w, sh = wm->disp->surf->h;
	struct win *w = win_new(wm, W_FILES, "File Manager", -1, -1, MIN(640, sw - 40), MIN(420, sh - 60));
	if (!w) {
		kfree(fm);
		return NULL;
	}
	w->priv = fm;
	w->ops = &fm_ops;
	w->resizable = 1;
	w->min_cw = 360;
	w->min_ch = 200;
	fm->w = w;
	w->menubar = &fm->mb;
	reload(fm);
	fm_render(w);
	return w;
}
