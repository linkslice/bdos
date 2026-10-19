/*
 * shell.c - COMMAND.COM for bdos.
 *
 * DOS commands with Unix aliases. A command remembers which name it was
 * invoked by: as DIR, '/' starts a switch and '\' separates paths; as ls,
 * '-' starts a switch and '/' separates paths.
 *
 * Supports > >> < redirection, | pipes, %VARIABLES%, and batch files with
 * %0-%9, GOTO, IF, FOR, CALL and SHIFT.
 */
#include "kernel.h"
#include "vfs.h"
#include "drv/drv.h"

#define LINE_MAX	512
#define MAX_ARGS	32
#define HIST_MAX	32
#define MAX_VARS	64

/* ---- per-shell state -------------------------------------------------------- */

struct batch {
	char *text;		/* the whole file, lines split in place */
	char **lines;
	int nlines;
	int pc;
	char *args[10];
	int argc;
	struct batch *prev;
};

struct shell {
	int echo_on;
	int errorlevel;
	int exit_req;
	char prompt[64];
	char *var_name[MAX_VARS];
	char *var_val[MAX_VARS];
	int nvars;
	struct batch *bat;
	char history[HIST_MAX][LINE_MAX];
	int hist_len;
};

struct cmd {
	int argc;
	char *argv[MAX_ARGS];
	int unix_style;
	struct shell *sh;
};

static struct shell *sh_cur(void)
{
	return task_current()->user;
}

static const char *getvar(struct shell *sh, const char *name)
{
	for (int i = 0; i < sh->nvars; i++)
		if (!strcasecmp(sh->var_name[i], name))
			return sh->var_val[i];
	return NULL;
}

static void setvar(struct shell *sh, const char *name, const char *val)
{
	for (int i = 0; i < sh->nvars; i++) {
		if (!strcasecmp(sh->var_name[i], name)) {
			kfree(sh->var_val[i]);
			if (val && *val) {
				sh->var_val[i] = kstrdup(val);
			} else {
				kfree(sh->var_name[i]);
				sh->nvars--;
				sh->var_name[i] = sh->var_name[sh->nvars];
				sh->var_val[i] = sh->var_val[sh->nvars];
			}
			return;
		}
	}
	if (!val || !*val || sh->nvars >= MAX_VARS)
		return;
	char *n = kstrdup(name);
	for (char *p = n; *p; p++)
		*p = (char)toupper(*p);
	sh->var_name[sh->nvars] = n;
	sh->var_val[sh->nvars] = kstrdup(val);
	sh->nvars++;
}

/* ---- consoles for redirection and pipes ------------------------------------- */

/*
 * A wrapper console: output can be captured into a buffer, input can come
 * from a buffer (ending in Ctrl-Z). Everything else passes through.
 */
struct wrapcon {
	struct console c;
	struct console *parent;
	int capture;
	char *out;
	size_t olen, ocap;
	const char *in;
	size_t ilen, ipos;
	int has_in;
};

static void wc_write(struct console *c, const char *s, size_t n)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (!w->capture) {
		w->parent->write(w->parent, s, n);
		return;
	}
	if (w->olen + n + 1 > w->ocap) {
		size_t nc = MAX(w->ocap * 2, w->olen + n + 256);
		char *nb = krealloc(w->out, nc);
		if (!nb)
			return;
		w->out = nb;
		w->ocap = nc;
	}
	memcpy(w->out + w->olen, s, n);
	w->olen += n;
	w->out[w->olen] = 0;
}

static int wc_getkey(struct console *c)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (w->has_in)
		return w->ipos < w->ilen ? (uint8_t)w->in[w->ipos++] : KEY_EOF;
	return w->parent->getkey(w->parent);
}

static void wc_clear(struct console *c)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (!w->capture)
		w->parent->clear(w->parent);
}

static void wc_cursor(struct console *c, int on)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (!w->capture)
		w->parent->cursor(w->parent, on);
}

static void wc_size(struct console *c, int *x, int *y)
{
	struct wrapcon *w = (struct wrapcon *)c;
	w->parent->size(w->parent, x, y);
	if (w->capture)
		*y = 0x7FFFFFFF;	/* nothing pages into a file */
}

static void wc_gotoxy(struct console *c, int x, int y)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (!w->capture && w->parent->gotoxy)
		w->parent->gotoxy(w->parent, x, y);
}

static void wc_attr(struct console *c, int fg, int bg)
{
	struct wrapcon *w = (struct wrapcon *)c;
	if (!w->capture && w->parent->attr)
		w->parent->attr(w->parent, fg, bg);
}

static void wrap_init(struct wrapcon *w, struct console *parent)
{
	memset(w, 0, sizeof *w);
	w->c.name = parent->name;
	w->c.write = wc_write;
	w->c.getkey = wc_getkey;
	w->c.clear = wc_clear;
	w->c.cursor = wc_cursor;
	w->c.size = wc_size;
	w->c.gotoxy = wc_gotoxy;
	w->c.attr = wc_attr;
	w->parent = parent;
}

int stdin_is_pipe(void)
{
	struct console *c = task_current()->con;
	return c && c->getkey == wc_getkey && ((struct wrapcon *)c)->has_in;
}

static int stdout_is_captured(void)
{
	struct console *c = task_current()->con;
	return c && c->write == wc_write && ((struct wrapcon *)c)->capture;
}

/* ---- small helpers ------------------------------------------------------------ */

int resolve_path(const char *in, char *out, size_t n)
{
	return vfs_resolve(task_current()->cwd, in, out, n);
}

static int is_switch(const char *arg, int unix_style)
{
	return unix_style ? (arg[0] == '-' && arg[1]) : arg[0] == '/';
}

static int has_flag(struct cmd *c, char f)
{
	for (int i = 1; i < c->argc; i++)
		if (is_switch(c->argv[i], c->unix_style))
			for (const char *p = c->argv[i] + 1; *p; p++) {
				if (*p == ':' )
					break;	/* /A:H - value, not more flags */
				if (toupper(*p) == toupper(f))
					return 1;
			}
	return 0;
}

/* nth non-switch argument */
static const char *arg(struct cmd *c, int n)
{
	for (int i = 1; i < c->argc; i++)
		if (!is_switch(c->argv[i], c->unix_style) && n-- == 0)
			return c->argv[i];
	return NULL;
}

static int nargs(struct cmd *c)
{
	int n = 0;
	while (arg(c, n))
		n++;
	return n;
}

static int fail(struct cmd *c, const char *what, int err)
{
	if (c->unix_style)
		kprintf("%s: %s: %s\n", c->argv[0], what ? what : "", vfs_strerror(err));
	else if (what)
		kprintf("%s - %s\n", vfs_strerror(err), what);
	else
		kprintf("%s\n", vfs_strerror(err));
	return 1;
}

static int has_wild(const char *s)
{
	return strchr(s, '*') || strchr(s, '?');
}

static void fmt_date(uint64_t t, char *out, size_t n)
{
	int y, mo, d, h, mi, s;
	epoch_to_date(t, &y, &mo, &d, &h, &mi, &s);
	snprintf(out, n, "%04d-%02d-%02d  %02d:%02d", y, mo, d, h, mi);
}

/* Pause output every screenful when writing to a real console. */
struct pager {
	int on, lines, rows;
};

static void pager_init(struct pager *p, int on)
{
	int cols;
	p->on = on && !stdout_is_captured();
	p->lines = 0;
	con_size(&cols, &p->rows);
}

static int pager_line(struct pager *p)	/* returns 1 if the user quit */
{
	if (!p->on || ++p->lines < p->rows - 1)
		return 0;
	p->lines = 0;
	con_write("-- More --");
	int k = con_getkey();
	con_write("\r          \r");
	return k == 'q' || k == 'Q' || k == 3 || k == 27;
}

/* Wildcard expansion: call fn for every entry in dir matching pattern. */
struct match_ctx {
	const char *pat;
	int hidden;
	struct vstat *list;
	int n, cap;
};

static int match_cb(void *arg, struct vstat *st)
{
	struct match_ctx *m = arg;
	if ((st->attr & (A_HIDDEN | A_SYSTEM)) && !m->hidden)
		return 0;
	if (!wildmatch(m->pat, st->name) && !(st->short_name[0] && wildmatch(m->pat, st->short_name)))
		return 0;
	if (m->n == m->cap) {
		int nc = m->cap ? m->cap * 2 : 32;
		struct vstat *nl = krealloc(m->list, (size_t)nc * sizeof *nl);
		if (!nl)
			return 1;
		m->list = nl;
		m->cap = nc;
	}
	m->list[m->n++] = *st;
	return 0;
}

/* Sort: directories first, then by name, ignoring case. */
static void sort_vstats(struct vstat *v, int n)
{
	for (int i = 1; i < n; i++) {
		struct vstat key = v[i];
		int j = i - 1;
		while (j >= 0) {
			int dk = key.type == VFS_DIR, dj = v[j].type == VFS_DIR;
			if (dj > dk || (dj == dk && strcasecmp(v[j].name, key.name) <= 0))
				break;
			v[j + 1] = v[j];
			j--;
		}
		v[j + 1] = key;
	}
}

/* Expand "DIR\*.BAS" into dir + sorted matches. Caller kfree()s *out. */
static int expand(const char *absdir, const char *pat, int hidden, struct vstat **out, int *n)
{
	struct match_ctx m = { pat, hidden, NULL, 0, 0 };
	int r = vfs_list(absdir, match_cb, &m);
	if (r) {
		kfree(m.list);
		return r;
	}
	sort_vstats(m.list, m.n);
	*out = m.list;
	*n = m.n;
	return 0;
}

/* Split a user path into (absolute dir, pattern). "*.BAS" -> (cwd, "*.BAS");
 * "GAMES" (a dir) -> ("...\GAMES", "*"). */
static int dir_and_pattern(const char *in, char *dir, size_t dn, char *pat, size_t pn)
{
	char abs[VFS_PATH_MAX];
	int r = resolve_path(in ? in : ".", abs, sizeof abs);
	if (r)
		return r;
	if (in && has_wild(vfs_basename(abs))) {
		strncpy0(pat, vfs_basename(abs), pn);
		vfs_dirname(abs, dir, dn);
		return 0;
	}
	struct vstat st;
	r = vfs_stat(abs, &st);
	if (r)
		return r;
	if (st.type == VFS_DIR) {
		strncpy0(dir, abs, dn);
		strncpy0(pat, "*", pn);
	} else {
		vfs_dirname(abs, dir, dn);
		strncpy0(pat, vfs_basename(abs), pn);
	}
	return 0;
}

/* ---- commands ---------------------------------------------------------------- */

static int cmd_help(struct cmd *c);
static int cmd_alias(struct cmd *c);
static int run_line(char *line);
static int run_batch(const char *path, int argc, char **argv);
static void print_prompt(struct shell *sh);

static int cmd_ver(struct cmd *c)
{
	(void)c;
	kprintf("\n%s version %s (%s)\n%s\n\n", BDOS_NAME, BDOS_VERSION, ARCH_NAME, BDOS_LONGNAME);
	return 0;
}

static int cmd_cls(struct cmd *c)
{
	(void)c;
	con_clear();
	return 0;
}

static int cmd_echo(struct cmd *c)
{
	struct shell *sh = c->sh;
	if (c->argc == 1) {
		kprintf("ECHO is %s\n", sh->echo_on ? "on" : "off");
		return 0;
	}
	if (c->argc == 2 && !strcasecmp(c->argv[1], "on")) {
		sh->echo_on = 1;
		return 0;
	}
	if (c->argc == 2 && !strcasecmp(c->argv[1], "off")) {
		sh->echo_on = 0;
		return 0;
	}
	int first = 1;
	for (int i = 1; i < c->argc; i++) {
		if (i == 1 && !strcmp(c->argv[i], "."))
			continue;
		kprintf("%s%s", first ? "" : " ", c->argv[i]);
		first = 0;
	}
	kprintf("\n");
	return 0;
}

static int cmd_rem(struct cmd *c)
{
	(void)c;
	return 0;
}

/* --- DIR / ls --- */

struct dirtotals {
	int files, dirs;
	uint64_t bytes;
};

static int dir_one(struct cmd *c, const char *dir, const char *pat, struct dirtotals *tot,
    struct pager *pg, int recurse)
{
	int hidden = has_flag(c, 'A');
	int wide = c->unix_style ? !has_flag(c, 'L') : has_flag(c, 'W');
	int bare = !c->unix_style && has_flag(c, 'B');
	struct vstat *v;
	int n, r = expand(dir, pat, hidden, &v, &n);
	if (r)
		return fail(c, dir, r);

	char canon[VFS_PATH_MAX];
	if (vfs_canon(dir, canon, sizeof canon))
		strncpy0(canon, dir, sizeof canon);
	if (!bare && !c->unix_style)
		kprintf("\n Directory of %s\n\n", canon);
	else if (c->unix_style && recurse)
		kprintf("\n%s:\n", canon);

	int cols, rows, col = 0;
	con_size(&cols, &rows);
	int colw = 20, per = MAX(1, cols / colw);
	int files = 0, dirs = 0;
	uint64_t bytes = 0;
	int quit = 0;
	for (int i = 0; i < n && !quit; i++) {
		struct vstat *s = &v[i];
		if (s->type == VFS_DIR)
			dirs++;
		else {
			files++;
			bytes += s->size;
		}
		if (bare) {
			kprintf("%s\n", s->name);
			quit = pager_line(pg);
		} else if (wide) {
			char name[VFS_NAME_MAX + 4];
			if (s->type == VFS_DIR)
				snprintf(name, sizeof name, c->unix_style ? "%s/" : "[%s]", s->name);
			else
				snprintf(name, sizeof name, "%s", s->name);
			int len = (int)strlen(name), span = (len + colw) / colw;
			if (col && col + span > per) {
				kprintf("\n");
				quit = pager_line(pg);
				col = 0;
			}
			kprintf("%s", name);
			for (int k = len; k < span * colw && col + span < per; k++)
				kprintf(" ");
			col += span;
		} else {
			char date[24], size[32];
			fmt_date(s->mtime, date, sizeof date);
			if (s->type == VFS_DIR)
				strcpy(size, "<DIR>");
			else
				fmt_commas(size, sizeof size, s->size);
			if (c->unix_style) {
				kprintf("%c%c%c%c %13s  %s  %s%s\n", s->type == VFS_DIR ? 'd' : '-',
				    (s->attr & A_RO) ? 'r' : 'w', (s->attr & A_HIDDEN) ? 'h' : '-',
				    (s->attr & A_SYSTEM) ? 's' : '-', s->type == VFS_DIR ? "-" : size,
				    date, s->name, s->type == VFS_DIR ? "/" : "");
			} else if (s->type == VFS_DIR) {
				kprintf("%-24s %-13s  %s\n", s->name, size, date);
			} else {
				kprintf("%-24s %13s  %s\n", s->name, size, date);
			}
			if (strlen(s->name) > 24 && !c->unix_style && s->short_name[0] &&
			    has_flag(c, 'X'))
				kprintf("    (%s)\n", s->short_name);
			quit = pager_line(pg);
		}
	}
	if (wide && col)
		kprintf("\n");
	if (!bare && !c->unix_style) {
		char b[32];
		fmt_commas(b, sizeof b, bytes);
		kprintf("%16d file(s) %16s bytes\n", files, b);
	}
	tot->files += files;
	tot->dirs += dirs;
	tot->bytes += bytes;

	if (recurse && !quit) {
		for (int i = 0; i < n; i++) {
			if (v[i].type != VFS_DIR)
				continue;
			char sub[VFS_PATH_MAX];
			vfs_join(dir, v[i].name, sub, sizeof sub);
			dir_one(c, sub, pat, tot, pg, 1);
		}
	}
	kfree(v);
	return quit;
}

static int cmd_dir(struct cmd *c)
{
	const char *a = arg(c, 0);
	char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
	int r = dir_and_pattern(a, dir, sizeof dir, pat, sizeof pat);
	if (r) {
		if (c->unix_style)
			return fail(c, a, r);
		kprintf("File not found\n");
		return 1;
	}
	struct pager pg;
	pager_init(&pg, !c->unix_style && has_flag(c, 'P'));
	char label[32];
	uint64_t free = 0, total = 0;
	int bare = !c->unix_style && has_flag(c, 'B');
	if (!c->unix_style && !bare) {
		if (vfs_info(dir[0], label, sizeof label, &free, &total) || !label[0])
			kprintf("\n Volume in drive %c has no label\n", dir[0]);
		else
			kprintf("\n Volume in drive %c is %s\n", dir[0], label);
	}
	struct dirtotals tot = { 0, 0, 0 };
	int recurse = has_flag(c, c->unix_style ? 'R' : 'S');
	dir_one(c, dir, pat, &tot, &pg, recurse);
	if (!bare && !c->unix_style) {
		char b[32];
		vfs_info(dir[0], label, sizeof label, &free, &total);
		if (recurse) {
			fmt_commas(b, sizeof b, tot.bytes);
			kprintf("\n     Total files listed:\n%16d file(s) %16s bytes\n", tot.files, b);
		}
		fmt_commas(b, sizeof b, free);
		kprintf("%16d dir(s)  %16s bytes free\n\n", tot.dirs, b);
	}
	return 0;
}

static int cmd_cd(struct cmd *c)
{
	const char *a = arg(c, 0);
	struct task *t = task_current();
	if (!a) {
		if (c->unix_style) {
			kprintf("%s\n", t->cwd + 2);	/* drop "C:" */
		} else {
			kprintf("%s\n", t->cwd);
		}
		return 0;
	}
	char abs[VFS_PATH_MAX];
	int r = resolve_path(a, abs, sizeof abs);
	struct vstat st;
	if (!r)
		r = vfs_stat(abs, &st);
	if (!r && st.type != VFS_DIR)
		r = E_NOTDIR;
	if (r) {
		if (c->unix_style)
			return fail(c, a, r);
		kprintf(r == E_BADDRIVE ? "Invalid drive specification\n" : "Invalid directory\n");
		return 1;
	}
	if (vfs_canon(abs, t->cwd, sizeof t->cwd))
		strncpy0(t->cwd, abs, sizeof t->cwd);
	return 0;
}

static int cmd_md(struct cmd *c)
{
	if (!arg(c, 0)) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	int rc = 0;
	for (int i = 0; arg(c, i); i++) {
		char abs[VFS_PATH_MAX];
		int r = resolve_path(arg(c, i), abs, sizeof abs);
		if (!r && has_flag(c, 'P') && c->unix_style) {
			/* mkdir -p: make every missing parent */
			for (char *p = abs + 3; *p; p++) {
				if (*p != '\\')
					continue;
				*p = 0;
				vfs_mkdir(abs);
				*p = '\\';
			}
			r = vfs_mkdir(abs);
			if (r == E_EXIST)
				r = 0;
		} else if (!r) {
			r = vfs_mkdir(abs);
		}
		if (r)
			rc = fail(c, arg(c, i), r == E_EXIST && !c->unix_style ?
			    E_EXIST : r);
	}
	return rc;
}

static int cmd_rd(struct cmd *c)
{
	if (!arg(c, 0)) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	char abs[VFS_PATH_MAX];
	int r = resolve_path(arg(c, 0), abs, sizeof abs);
	if (!r && !strcasecmp(abs, task_current()->cwd))
		r = E_ACCES;	/* can't remove where you stand */
	if (!r)
		r = vfs_rmdir(abs);
	return r ? fail(c, arg(c, 0), r) : 0;
}

/* --- TYPE / cat --- */

static int type_file(struct cmd *c, const char *abs, const char *shown, struct pager *pg)
{
	int err;
	struct vfile *f = vfs_open(abs, O_READ, &err);
	if (!f)
		return fail(c, shown, err);
	char buf[512];
	long n;
	int last = '\n', quit = 0;
	while (!quit && (n = vfs_read(f, buf, sizeof buf)) > 0) {
		for (long i = 0; i < n; i++) {
			if (buf[i] == KEY_EOF)	/* DOS text files may end in ^Z */
				break;
			con_putc(buf[i]);
			last = buf[i];
			if (buf[i] == '\n' && (quit = pager_line(pg)))
				break;
		}
	}
	vfs_close(f);
	if (last != '\n')
		con_putc('\n');
	return n < 0 ? fail(c, shown, (int)n) : 0;
}

static int cmd_type(struct cmd *c)
{
	if (!arg(c, 0)) {
		if (c->unix_style && stdin_is_pipe()) {	/* cat with no args: copy stdin */
			int k;
			while ((k = con_getkey()) != KEY_EOF)
				con_putc((char)k);
			return 0;
		}
		kprintf("Required parameter missing\n");
		return 1;
	}
	struct pager pg;
	pager_init(&pg, 0);
	int rc = 0;
	for (int i = 0; arg(c, i); i++) {
		const char *a = arg(c, i);
		char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
		int r = dir_and_pattern(a, dir, sizeof dir, pat, sizeof pat);
		if (r) {
			rc = fail(c, a, r);
			continue;
		}
		if (!has_wild(pat)) {
			char abs[VFS_PATH_MAX];
			resolve_path(a, abs, sizeof abs);
			rc |= type_file(c, abs, a, &pg);
			continue;
		}
		struct vstat *v;
		int n;
		if ((r = expand(dir, pat, 0, &v, &n))) {
			rc = fail(c, a, r);
			continue;
		}
		for (int k = 0; k < n; k++) {
			if (v[k].type != VFS_FILE)
				continue;
			char abs[VFS_PATH_MAX];
			vfs_join(dir, v[k].name, abs, sizeof abs);
			if (!c->unix_style)
				kprintf("\n%s\n\n", v[k].name);
			rc |= type_file(c, abs, v[k].name, &pg);
		}
		if (!n)
			rc = fail(c, a, E_NOENT);
		kfree(v);
	}
	return rc;
}

/* --- COPY / cp, XCOPY, MOVE / mv, DEL / rm, REN --- */

static int copy_file(const char *src, const char *dst)
{
	if (!strcasecmp(src, dst))
		return E_EXIST;	/* "File cannot be copied onto itself" */
	int err;
	struct vfile *in = vfs_open(src, O_READ, &err);
	if (!in)
		return err;
	struct vfile *out = vfs_open(dst, O_WRITE | O_CREATE | O_TRUNC, &err);
	if (!out) {
		vfs_close(in);
		return err;
	}
	char *buf = kmalloc(64 * 1024);
	long n = buf ? 0 : E_NOMEM;
	if (buf) {
		if (vfs_isdev(in)) {
			/* COPY CON FILE: read typed lines until Ctrl-Z */
			char line[LINE_MAX];
			int len;
			while ((len = read_line(line, sizeof line)) >= 0) {
				line[len++] = '\n';
				if ((n = vfs_write(out, line, (size_t)len)) < 0)
					break;
			}
			if (n > 0)
				n = 0;
		} else {
			while ((n = vfs_read(in, buf, 64 * 1024)) > 0)
				if ((n = vfs_write(out, buf, (size_t)n)) < 0)
					break;
		}
	}
	kfree(buf);
	vfs_close(in);
	vfs_close(out);
	if (n >= 0) {
		/* keep the source's attributes (minus read-only on the copy) */
		struct vstat st;
		if (!vfs_stat(src, &st) && st.type == VFS_FILE)
			vfs_setattr(dst, (uint8_t)((st.attr | A_ARCHIVE) & ~A_RO));
	}
	return n < 0 ? (int)n : 0;
}

static int copy_tree(const char *src, const char *dst, int *count)
{
	int r = vfs_mkdir(dst);
	if (r && r != E_EXIST)
		return r;
	struct vstat *v;
	int n;
	if ((r = expand(src, "*", 1, &v, &n)))
		return r;
	for (int i = 0; i < n && !r; i++) {
		char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
		vfs_join(src, v[i].name, s, sizeof s);
		vfs_join(dst, v[i].name, d, sizeof d);
		if (v[i].type == VFS_DIR) {
			r = copy_tree(s, d, count);
		} else {
			kprintf("%s\n", s);
			r = copy_file(s, d);
			if (!r)
				(*count)++;
		}
	}
	kfree(v);
	return r;
}

/* Where does `src` go, given the destination argument? */
static void target_for(const char *dst_abs, int dst_is_dir, const char *name, char *out,
    size_t n)
{
	if (dst_is_dir)
		vfs_join(dst_abs, name, out, n);
	else
		strncpy0(out, dst_abs, n);
}

static int do_copy(struct cmd *c, int move)
{
	int na = nargs(c);
	if (na < 1) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	const char *dst_arg = na >= 2 ? arg(c, na - 1) : ".";
	int nsrc = na >= 2 ? na - 1 : 1;
	char dst[VFS_PATH_MAX];
	int r = resolve_path(dst_arg, dst, sizeof dst);
	if (r)
		return fail(c, dst_arg, r);
	struct vstat dst_st;
	int dst_is_dir = !vfs_stat(dst, &dst_st) && dst_st.type == VFS_DIR;
	size_t dl = strlen(dst_arg);
	if (!dst_is_dir && dl && (dst_arg[dl - 1] == '\\' || dst_arg[dl - 1] == '/'))
		return fail(c, dst_arg, E_NOENT);
	int recursive = c->unix_style ? has_flag(c, 'R') : 0;
	int count = 0, rc = 0;

	for (int i = 0; i < nsrc; i++) {
		const char *sa = arg(c, i);
		char sabs[VFS_PATH_MAX];
		if ((r = resolve_path(sa, sabs, sizeof sabs))) {
			rc = fail(c, sa, r);
			continue;
		}
		struct vstat *v = NULL;
		int n = 0;
		char sdir[VFS_PATH_MAX];
		if (has_wild(vfs_basename(sabs))) {
			vfs_dirname(sabs, sdir, sizeof sdir);
			if ((r = expand(sdir, vfs_basename(sabs), 0, &v, &n))) {
				rc = fail(c, sa, r);
				continue;
			}
			if (!n) {
				rc = fail(c, sa, E_NOENT);
				kfree(v);
				continue;
			}
		} else {
			v = kmalloc(sizeof *v);
			if ((r = vfs_stat(sabs, v))) {
				kfree(v);
				rc = fail(c, sa, r);
				continue;
			}
			n = 1;
			vfs_dirname(sabs, sdir, sizeof sdir);
			if (v->type == VFS_DEV)
				strncpy0(v->name, vfs_basename(sabs), sizeof v->name);
		}
		if ((n > 1 || nsrc > 1) && !dst_is_dir) {
			kfree(v);
			kprintf(c->unix_style ? "%s: target is not a directory\n" :
			    "Cannot copy several files onto one (%s)\n", dst_arg);
			return 1;
		}
		for (int k = 0; k < n; k++) {
			char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
			if (v[k].type == VFS_DEV)
				strncpy0(s, sabs, sizeof s);
			else
				vfs_join(sdir, v[k].name, s, sizeof s);
			target_for(dst, dst_is_dir, v[k].name, d, sizeof d);
			if (v[k].type == VFS_DIR) {
				if (move) {
					r = vfs_rename(s, d);
					if (!r)
						count++;
				} else if (recursive) {
					r = copy_tree(s, d, &count);
				} else {
					if (n == 1)
						kprintf("%s is a directory (use XCOPY /S%s)\n", v[k].name,
						    c->unix_style ? " or cp -r" : "");
					continue;
				}
			} else if (move) {
				r = vfs_rename(s, d);
				if (r == E_XDEV && !(r = copy_file(s, d)))
					r = vfs_delete(s);
				if (!r)
					count++;
			} else {
				if (n > 1 && !c->unix_style)
					kprintf("%s\n", v[k].name);
				r = copy_file(s, d);
				if (!r)
					count++;
			}
			if (r) {
				if (r == E_EXIST && !strcasecmp(s, d))
					kprintf("File cannot be copied onto itself\n");
				else
					fail(c, v[k].name, r);
				rc = 1;
			}
		}
		kfree(v);
	}
	if (!c->unix_style)
		kprintf("%9d file(s) %s\n", count, move ? "moved" : "copied");
	return rc;
}

static int cmd_copy(struct cmd *c) { return do_copy(c, 0); }
static int cmd_move(struct cmd *c) { return do_copy(c, 1); }

static int cmd_xcopy(struct cmd *c)
{
	const char *sa = arg(c, 0), *da = arg(c, 1);
	if (!sa || !da) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
	int r = resolve_path(sa, s, sizeof s);
	if (!r)
		r = resolve_path(da, d, sizeof d);
	if (r)
		return fail(c, sa, r);
	struct vstat st;
	if ((r = vfs_stat(s, &st)))
		return fail(c, sa, r);
	int count = 0;
	if (st.type == VFS_DIR) {
		r = copy_tree(s, d, &count);
	} else {
		struct vstat dst;
		char t[VFS_PATH_MAX];
		target_for(d, !vfs_stat(d, &dst) && dst.type == VFS_DIR, st.name, t, sizeof t);
		kprintf("%s\n", s);
		r = copy_file(s, t);
		if (!r)
			count++;
	}
	kprintf("%d File(s) copied\n", count);
	return r ? fail(c, sa, r) : 0;
}

static int delete_tree(const char *abs)
{
	struct vstat *v;
	int n, r = expand(abs, "*", 1, &v, &n);
	if (r)
		return r;
	for (int i = 0; i < n && !r; i++) {
		char p[VFS_PATH_MAX];
		vfs_join(abs, v[i].name, p, sizeof p);
		if (v[i].attr & A_RO)
			vfs_setattr(p, (uint8_t)(v[i].attr & ~A_RO));
		r = v[i].type == VFS_DIR ? delete_tree(p) : vfs_delete(p);
	}
	kfree(v);
	return r ? r : vfs_rmdir(abs);
}

static int yes_no(const char *q)
{
	kprintf("%s (Y/N)?", q);
	for (;;) {
		int k = con_getkey();
		if (k == 'y' || k == 'Y' || k == 'n' || k == 'N' || k == KEY_EOF || k == 3) {
			kprintf("%c\n", k == KEY_EOF || k == 3 ? 'N' : toupper(k));
			return k == 'y' || k == 'Y';
		}
	}
}

static int cmd_deltree(struct cmd *c)
{
	const char *a = arg(c, 0);
	if (!a) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	char abs[VFS_PATH_MAX];
	int r = resolve_path(a, abs, sizeof abs);
	if (r)
		return fail(c, a, r);
	if (!strncasecmp(task_current()->cwd, abs, strlen(abs)))
		return fail(c, a, E_ACCES);
	if (!has_flag(c, 'Y') && !c->unix_style) {
		char q[VFS_PATH_MAX + 32];
		snprintf(q, sizeof q, "Delete directory \"%s\" and all its subdirectories", a);
		if (!yes_no(q))
			return 0;
	}
	struct vstat st;
	if ((r = vfs_stat(abs, &st)))
		return fail(c, a, r);
	r = st.type == VFS_DIR ? delete_tree(abs) : vfs_delete(abs);
	if (!r && !c->unix_style)
		kprintf("Deleting %s...\n", a);
	return r ? fail(c, a, r) : 0;
}

static int cmd_del(struct cmd *c)
{
	if (!arg(c, 0)) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	if (c->unix_style && has_flag(c, 'R'))
		return cmd_deltree(c);
	int rc = 0;
	for (int i = 0; arg(c, i); i++) {
		const char *a = arg(c, i);
		char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
		int r = dir_and_pattern(a, dir, sizeof dir, pat, sizeof pat);
		if (r) {
			if (!(c->unix_style && has_flag(c, 'F')))
				rc = fail(c, a, r);
			continue;
		}
		char abs[VFS_PATH_MAX];
		resolve_path(a, abs, sizeof abs);
		struct vstat st;
		if (!has_wild(pat) && !vfs_stat(abs, &st) && st.type == VFS_DIR) {
			if (c->unix_style) {
				rc = fail(c, a, E_ISDIR);
				continue;
			}
			/* DEL DIRNAME means DEL DIRNAME\*.* */
			strncpy0(dir, abs, sizeof dir);
			strcpy(pat, "*.*");
		}
		if (!strcmp(pat, "*") || !strcmp(pat, "*.*")) {
			if (!c->unix_style && !has_flag(c, 'Q') &&
			    !yes_no("All files in directory will be deleted!\nAre you sure"))
				continue;
		}
		struct vstat *v;
		int n;
		if ((r = expand(dir, pat, 0, &v, &n))) {
			rc = fail(c, a, r);
			continue;
		}
		int deleted = 0;
		for (int k = 0; k < n; k++) {
			if (v[k].type != VFS_FILE)
				continue;
			char p[VFS_PATH_MAX];
			vfs_join(dir, v[k].name, p, sizeof p);
			if (has_flag(c, 'P') && !c->unix_style) {
				char q[VFS_PATH_MAX];
				snprintf(q, sizeof q, "%s, Delete", v[k].name);
				if (!yes_no(q))
					continue;
			}
			if ((r = vfs_delete(p)))
				rc = fail(c, v[k].name, r);
			else
				deleted++;
		}
		if (!deleted && !rc && !(c->unix_style && has_flag(c, 'F')))
			rc = fail(c, a, E_NOENT);
		kfree(v);
	}
	return rc;
}

static int cmd_ren(struct cmd *c)
{
	const char *a = arg(c, 0), *b = arg(c, 1);
	if (!a || !b) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	if (strchr(b, '\\') || strchr(b, '/') || strchr(b, ':')) {
		kprintf("Invalid parameter - REN only changes names; use MOVE to move\n");
		return 1;
	}
	char from[VFS_PATH_MAX], dir[VFS_PATH_MAX], to[VFS_PATH_MAX];
	int r = resolve_path(a, from, sizeof from);
	if (r)
		return fail(c, a, r);
	vfs_dirname(from, dir, sizeof dir);
	vfs_join(dir, b, to, sizeof to);
	r = vfs_rename(from, to);
	if (r == E_EXIST)
		kprintf("Duplicate file name or file not found\n");
	else if (r)
		fail(c, a, r);
	return r ? 1 : 0;
}

static int cmd_touch(struct cmd *c)
{
	int rc = 0;
	for (int i = 0; arg(c, i); i++) {
		char abs[VFS_PATH_MAX];
		int r = resolve_path(arg(c, i), abs, sizeof abs), err = 0;
		if (r) {
			rc = fail(c, arg(c, i), r);
			continue;
		}
		struct vfile *f = vfs_open(abs, O_WRITE | O_CREATE | O_APPEND, &err);
		if (!f) {
			rc = fail(c, arg(c, i), err);
			continue;
		}
		vfs_write(f, "", 0);	/* bumps the modification time */
		vfs_close(f);
	}
	return rc;
}

static int cmd_attrib(struct cmd *c)
{
	uint8_t set = 0, clr = 0;
	const char *target = NULL;
	for (int i = 1; i < c->argc; i++) {
		const char *a = c->argv[i];
		if ((a[0] == '+' || a[0] == '-') && a[1] && !a[2]) {
			uint8_t bit = 0;
			switch (toupper(a[1])) {
			case 'R': bit = A_RO; break;
			case 'H': bit = A_HIDDEN; break;
			case 'S': bit = A_SYSTEM; break;
			case 'A': bit = A_ARCHIVE; break;
			}
			if (!bit) {
				kprintf("Invalid switch - %s\n", a);
				return 1;
			}
			if (a[0] == '+')
				set |= bit;
			else
				clr |= bit;
		} else if (a[0] != '/') {
			target = a;
		}
	}
	char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
	int r = dir_and_pattern(target, dir, sizeof dir, pat, sizeof pat);
	if (r)
		return fail(c, target, r);
	if (!target)
		strcpy(pat, "*");
	struct vstat *v;
	int n;
	if ((r = expand(dir, pat, 1, &v, &n)))
		return fail(c, target, r);
	for (int k = 0; k < n; k++) {
		char p[VFS_PATH_MAX];
		vfs_join(dir, v[k].name, p, sizeof p);
		if (set || clr) {
			if ((r = vfs_setattr(p, (uint8_t)((v[k].attr | set) & ~clr))))
				fail(c, v[k].name, r);
		} else {
			uint8_t at = v[k].attr;
			kprintf("%c  %c%c%c  %s\n", (at & A_ARCHIVE) ? 'A' : ' ',
			    (at & A_SYSTEM) ? 'S' : ' ', (at & A_HIDDEN) ? 'H' : ' ',
			    (at & A_RO) ? 'R' : ' ', p);
		}
	}
	if (!n)
		fail(c, target, E_NOENT);
	kfree(v);
	return 0;
}

static void tree_walk(const char *dir, char *prefix, size_t plen, int files, int *count)
{
	struct vstat *v;
	int n;
	if (expand(dir, "*", 0, &v, &n))
		return;
	int last_dir = -1, last = n - 1;
	for (int i = 0; i < n; i++)
		if (v[i].type == VFS_DIR)
			last_dir = i;
	if (!files)
		last = last_dir;
	for (int i = 0; i < n; i++) {
		if (v[i].type != VFS_DIR && !files)
			continue;
		int is_last = (i == last);
		kprintf("%s%s%s\n", prefix, is_last ? "\\---" : "+---", v[i].name);
		if (v[i].type == VFS_DIR) {
			(*count)++;
			char sub[VFS_PATH_MAX];
			vfs_join(dir, v[i].name, sub, sizeof sub);
			size_t l = strlen(prefix);
			strcat0(prefix, is_last ? "    " : "|   ", plen);
			tree_walk(sub, prefix, plen, files, count);
			prefix[l] = 0;
		}
	}
	kfree(v);
}

static int cmd_tree(struct cmd *c)
{
	char abs[VFS_PATH_MAX];
	int r = resolve_path(arg(c, 0) ? arg(c, 0) : ".", abs, sizeof abs);
	if (r)
		return fail(c, arg(c, 0), r);
	char canon[VFS_PATH_MAX];
	if (vfs_canon(abs, canon, sizeof canon))
		return fail(c, arg(c, 0), E_NOENT);
	kprintf("%s\n", canon);
	char prefix[256] = "";
	int count = 0;
	tree_walk(canon, prefix, sizeof prefix, has_flag(c, 'F'), &count);
	if (!count && !has_flag(c, 'F'))
		kprintf("No subfolders exist\n");
	return 0;
}

/* --- volumes, memory, system --- */

static int cmd_vol(struct cmd *c)
{
	char drive = arg(c, 0) ? (char)toupper(arg(c, 0)[0]) : task_current()->cwd[0];
	char label[32];
	uint64_t free, total;
	int r = vfs_info(drive, label, sizeof label, &free, &total);
	if (r)
		return fail(c, NULL, r);
	if (label[0])
		kprintf("\n Volume in drive %c is %s\n\n", drive, label);
	else
		kprintf("\n Volume in drive %c has no label\n\n", drive);
	return 0;
}

static int cmd_label(struct cmd *c)
{
	char drive = task_current()->cwd[0];
	const char *l = arg(c, 0);
	if (l && strlen(l) == 2 && l[1] == ':') {
		drive = (char)toupper(l[0]);
		l = arg(c, 1);
	}
	if (!l) {
		cmd_vol(c);
		char buf[32];
		kprintf("Volume label (31 characters, ENTER for none)? ");
		int n = read_line(buf, sizeof buf);
		if (n < 0)
			return 0;
		l = buf;
		int r = vfs_set_label(drive, l);
		return r ? fail(c, NULL, r) : 0;
	}
	int r = vfs_set_label(drive, l);
	return r ? fail(c, NULL, r) : 0;
}

static void chk_out(const char *msg)
{
	kprintf("  %s\n", msg);
}

static int cmd_chkdsk(struct cmd *c)
{
	char drive = task_current()->cwd[0];
	if (arg(c, 0) && arg(c, 0)[1] == ':')
		drive = (char)toupper(arg(c, 0)[0]);
	int fix = has_flag(c, 'F');
	char label[32];
	uint64_t free, total;
	int r = vfs_info(drive, label, sizeof label, &free, &total);
	if (r)
		return fail(c, NULL, r);
	kprintf("\nChecking drive %c: (%s)...\n\n", drive, label[0] ? label : "no label");
	r = vfs_check(drive, fix, chk_out);
	if (r < 0)
		return fail(c, NULL, r);
	vfs_info(drive, label, sizeof label, &free, &total);
	char a[32], b[32];
	fmt_commas(a, sizeof a, total);
	fmt_commas(b, sizeof b, free);
	kprintf("\n%18s bytes total disk space\n%18s bytes available on disk\n\n", a, b);
	if (r && !fix)
		kprintf("Errors found. Run CHKDSK /F to fix them.\n\n");
	else if (!r)
		kprintf("No problems found.\n\n");
	return r ? 1 : 0;
}

static void drive_cb(struct vfs_drive *d, void *arg)
{
	(void)arg;
	char label[32];
	uint64_t free = 0, total = 0;
	d->ops->info(d->ctx, label, sizeof label, &free, &total);
	char f[32];
	fmt_commas(f, sizeof f, free / 1024);
	kprintf("  %c:  %-12s %12s K free   %s\n", d->letter, label[0] ? label : "(no label)",
	    f, d->desc);
}

static int cmd_drives(struct cmd *c)
{
	(void)c;
	kprintf("\n");
	vfs_drives(drive_cb, NULL);
	kprintf("\n");
	return 0;
}

static int cmd_mem(struct cmd *c)
{
	(void)c;
	char a[32], b[32], h[32];
	fmt_commas(a, sizeof a, pmm_total() / 1024);
	fmt_commas(b, sizeof b, pmm_free_bytes() / 1024);
	fmt_commas(h, sizeof h, heap_in_use() / 1024);
	kprintf("\nMemory Type          Total\n");
	kprintf("----------------  ---------------\n");
	kprintf("Total RAM         %12s K\n", a);
	kprintf("Free              %12s K\n", b);
	kprintf("Kernel heap       %12s K\n", h);
	kprintf("----------------  ---------------\n");
	fmt_commas(a, sizeof a, bootinfo->kernel_size / 1024);
	kprintf("Kernel at %p, %s K. Uptime %lu s.\n", (void *)bootinfo->kernel_base, a,
	    uptime_ms() / 1000);
	kprintf("No 640K limit here: programs get as much RAM as is free.\n\n");
	return 0;
}

static int cmd_tasks(struct cmd *c)
{
	(void)c;
	task_list();
	return 0;
}

static int cmd_hwinfo(struct cmd *c)
{
	(void)c;
	pci_list();
	for (int i = 0; i < blk_count(); i++) {
		struct blkdev *b = blk_get(i);
		char sz[32];
		fmt_commas(sz, sizeof sz, b->sectors / 2048);
		kprintf("  %-6s %10s MB  %s\n", b->name, sz, b->model);
	}
	kprintf("\n  Display %ux%u, mouse %s\n\n", bootinfo->fb.width, bootinfo->fb.height,
	    ps2_mouse_present() ? "present" : "not found");
	return 0;
}

static int cmd_date(struct cmd *c)
{
	(void)c;
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	static const char *const dow[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
	kprintf("Current date is %s %04d-%02d-%02d\n", dow[(rtc_epoch() / 86400) % 7], y, mo, d);
	return 0;
}

static int cmd_time(struct cmd *c)
{
	(void)c;
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	kprintf("Current time is %02d:%02d:%02d\n", h, mi, s);
	return 0;
}

static int cmd_reboot(struct cmd *c)
{
	(void)c;
	kprintf("Restarting...\n");
	arch_reboot();
}

static int cmd_pause(struct cmd *c)
{
	(void)c;
	kprintf("Press any key to continue . . .");
	con_getkey();
	kprintf("\n");
	return 0;
}

static int cmd_set(struct cmd *c)
{
	struct shell *sh = c->sh;
	if (c->argc == 1) {
		for (int i = 0; i < sh->nvars; i++)
			kprintf("%s=%s\n", sh->var_name[i], sh->var_val[i]);
		return 0;
	}
	/* re-join: SET NAME=some words */
	char buf[LINE_MAX] = "";
	for (int i = 1; i < c->argc; i++) {
		strcat0(buf, c->argv[i], sizeof buf);
		if (i + 1 < c->argc)
			strcat0(buf, " ", sizeof buf);
	}
	char *eq = strchr(buf, '=');
	if (!eq) {
		const char *v = getvar(sh, buf);
		if (v)
			kprintf("%s=%s\n", buf, v);
		else
			kprintf("Environment variable %s not defined\n", buf);
		return v ? 0 : 1;
	}
	*eq = 0;
	setvar(sh, buf, eq + 1);
	return 0;
}

static int cmd_path(struct cmd *c)
{
	if (c->argc == 1) {
		const char *p = getvar(c->sh, "PATH");
		kprintf("PATH=%s\n", p ? p : "");
		return 0;
	}
	setvar(c->sh, "PATH", strcmp(c->argv[1], ";") ? c->argv[1] : "");
	return 0;
}

static int cmd_prompt(struct cmd *c)
{
	char buf[64] = "";
	for (int i = 1; i < c->argc; i++) {
		strcat0(buf, c->argv[i], sizeof buf);
		if (i + 1 < c->argc)
			strcat0(buf, " ", sizeof buf);
	}
	strncpy0(c->sh->prompt, buf[0] ? buf : "$P$G", sizeof c->sh->prompt);
	return 0;
}

static int cmd_exit(struct cmd *c)
{
	if (c->sh->bat && c->argc > 1 && !strcasecmp(c->argv[1], "/B")) {
		c->sh->bat->pc = c->sh->bat->nlines;
		return c->sh->errorlevel;
	}
	c->sh->exit_req = 1;
	if (c->sh->bat)
		c->sh->bat->pc = c->sh->bat->nlines;
	return arg(c, 0) ? (int)strtol(arg(c, 0), NULL, 10) : 0;
}

/* --- filters: MORE, SORT, FIND --- */

/* Read all of stdin (or the files given) into one buffer. */
static char *gather_input(struct cmd *c, int first_file_arg, size_t *len)
{
	char *buf = NULL;
	size_t n = 0, cap = 0;
	if (arg(c, first_file_arg)) {
		for (int i = first_file_arg; arg(c, i); i++) {
			char abs[VFS_PATH_MAX];
			size_t l;
			int err = resolve_path(arg(c, i), abs, sizeof abs);
			char *data = err ? NULL : vfs_slurp(abs, &l, &err);
			if (!data) {
				fail(c, arg(c, i), err);
				continue;
			}
			char *nb = krealloc(buf, n + l + 2);
			if (!nb) {
				kfree(data);
				break;
			}
			buf = nb;
			memcpy(buf + n, data, l);
			n += l;
			if (l && data[l - 1] != '\n')
				buf[n++] = '\n';
			kfree(data);
		}
	} else {
		char line[LINE_MAX];
		int l;
		while ((l = read_line(line, sizeof line)) >= 0) {
			if (n + (size_t)l + 2 > cap) {
				cap = MAX(cap * 2, n + (size_t)l + 256);
				char *nb = krealloc(buf, cap);
				if (!nb)
					break;
				buf = nb;
			}
			memcpy(buf + n, line, (size_t)l);
			n += (size_t)l;
			buf[n++] = '\n';
		}
	}
	if (!buf)
		buf = kzalloc(1);
	else
		buf[n] = 0;
	*len = n;
	return buf;
}

static int cmd_more(struct cmd *c)
{
	size_t n;
	char *text = gather_input(c, 0, &n);
	struct pager pg;
	pager_init(&pg, 1);
	for (size_t i = 0; i < n; i++) {
		con_putc(text[i]);
		if (text[i] == '\n' && pager_line(&pg))
			break;
	}
	kfree(text);
	return 0;
}

static int cmp_lines(const char *a, const char *b, int rev)
{
	int r = strcasecmp(a, b);
	return rev ? -r : r;
}

static int cmd_sort(struct cmd *c)
{
	size_t n;
	char *text = gather_input(c, 0, &n);
	int count = 0;
	for (size_t i = 0; i < n; i++)
		if (text[i] == '\n')
			count++;
	char **lines = kmalloc(sizeof(char *) * (size_t)(count + 1));
	int k = 0;
	char *p = text;
	for (size_t i = 0; i < n && lines; i++)
		if (text[i] == '\n') {
			text[i] = 0;
			lines[k++] = p;
			p = text + i + 1;
		}
	int rev = has_flag(c, 'R');
	for (int i = 1; i < k; i++) {
		char *key = lines[i];
		int j = i - 1;
		while (j >= 0 && cmp_lines(lines[j], key, rev) > 0) {
			lines[j + 1] = lines[j];
			j--;
		}
		lines[j + 1] = key;
	}
	for (int i = 0; i < k; i++)
		kprintf("%s\n", lines[i]);
	kfree(lines);
	kfree(text);
	return 0;
}

static int cmd_find(struct cmd *c)
{
	const char *needle = arg(c, 0);
	if (!needle) {
		kprintf("Required parameter missing\n");
		return 1;
	}
	int invert = has_flag(c, 'V'), count_only = has_flag(c, 'C');
	int nocase = c->unix_style ? has_flag(c, 'i') : has_flag(c, 'I');
	size_t n;
	char *text = gather_input(c, 1, &n);
	size_t nl = strlen(needle);
	int hits = 0;
	char *line = text;
	for (size_t i = 0; i <= n; i++) {
		if (i < n && text[i] != '\n')
			continue;
		if (i == n && line == text + n)
			break;
		text[i] = 0;
		int found = 0;
		for (char *p = line; *p && !found; p++)
			found = nocase ? !strncasecmp(p, needle, nl) : !strncmp(p, needle, nl);
		if (!nl)
			found = 1;
		if (found != invert) {
			hits++;
			if (!count_only)
				kprintf("%s\n", line);
		}
		line = text + i + 1;
	}
	if (count_only)
		kprintf("%d\n", hits);
	kfree(text);
	return hits ? 0 : 1;
}

/* --- EDIT, BASIC, START, CTTY, WIN --- */

static int cmd_edit(struct cmd *c)
{
	char abs[VFS_PATH_MAX];
	if (!arg(c, 0)) {
		kprintf("Usage: EDIT filename\n");
		return 1;
	}
	int r = resolve_path(arg(c, 0), abs, sizeof abs);
	if (r)
		return fail(c, arg(c, 0), r);
	return edit_file(abs);
}

static int cmd_basic(struct cmd *c)
{
	if (!arg(c, 0))
		return basic_immediate();
	char abs[VFS_PATH_MAX];
	int r = resolve_path(arg(c, 0), abs, sizeof abs);
	if (r)
		return fail(c, arg(c, 0), r);
	return basic_run_file(abs, c->argc - 1, c->argv + 1);
}

static void start_task(void *arg)
{
	char *line = arg;
	shell_task(line);
}

static int cmd_start(struct cmd *c)
{
	if (c->argc < 2) {
		kprintf("Usage: START command   (runs it as a separate task)\n");
		return 1;
	}
	char line[LINE_MAX] = "";
	for (int i = 1; i < c->argc; i++) {
		strcat0(line, c->argv[i], sizeof line);
		if (i + 1 < c->argc)
			strcat0(line, " ", sizeof line);
	}
	struct task *t = task_spawn(c->argv[1], start_task, kstrdup(line), NULL);
	if (!t)
		return fail(c, NULL, E_NOMEM);
	kprintf("Started task %d\n", t->id);
	return 0;
}

static int cmd_ctty(struct cmd *c)
{
	const char *a = arg(c, 0);
	struct task *t = task_current();
	if (!a) {
		kprintf("Console is %s\n", t->con->name);
		return 0;
	}
	struct console *target = NULL;
	if (!strcasecmp(a, "COM1") || !strcasecmp(a, "AUX")) {
		target = serial_console();
		screen_mirror(0);
	} else if (!strcasecmp(a, "CON")) {
		target = screen_console();
		screen_mirror(1);
	} else {
		extern struct console *remote_console_open(const char *host, int *err);
		if (a[0] == '\\' && a[1] == '\\') {
			int err = 0;
			target = remote_console_open(a + 2, &err);
			if (!target)
				return fail(c, a, err);
		}
	}
	if (!target) {
		kprintf("Invalid device - %s\n", a);
		return 1;
	}
	/* The shell's own console must be the real one, not a redirect wrapper. */
	t->con = target;
	return 0;
}

static int cmd_win(struct cmd *c)
{
	extern int desktop_run(const char *remote);
	return desktop_run(arg(c, 0));
}

/* --- command tables --- */

typedef int (*cmd_fn)(struct cmd *);

struct command {
	const char *name;
	cmd_fn fn;
	const char *help;
};

static int cmd_call(struct cmd *c);
static int cmd_goto(struct cmd *c);
static int cmd_if(struct cmd *c);
static int cmd_for(struct cmd *c);
static int cmd_shift(struct cmd *c);

static const struct command commands[] = {
	{ "ALIAS",   cmd_alias,   "List the Unix-style names" },
	{ "ATTRIB",  cmd_attrib,  "Show or change attributes (+R -R +H -H +S -S +A -A)" },
	{ "BASIC",   cmd_basic,   "BBC-style BASIC: BASIC [file.BAS]" },
	{ "CALL",    cmd_call,    "Run a batch file from another" },
	{ "CD",      cmd_cd,      "Show or change the current directory" },
	{ "CHDIR",   cmd_cd,      NULL },
	{ "CHKDSK",  cmd_chkdsk,  "Check a drive (/F fixes problems)" },
	{ "CLS",     cmd_cls,     "Clear the screen" },
	{ "COPY",    cmd_copy,    "Copy files (COPY CON FILE to type one in)" },
	{ "CTTY",    cmd_ctty,    "Move this shell to COM1, CON or \\\\HOST" },
	{ "DATE",    cmd_date,    "Show the date" },
	{ "DEL",     cmd_del,     "Delete files (/P asks, /Q quiet)" },
	{ "DELTREE", cmd_deltree, "Delete a directory and everything in it" },
	{ "DIR",     cmd_dir,     "List files (/W wide /A all /B bare /S subdirs /P pause)" },
	{ "DRIVES",  cmd_drives,  "List drives" },
	{ "ECHO",    cmd_echo,    "Display text, or turn command echo ON/OFF" },
	{ "EDIT",    cmd_edit,    "Full-screen text editor" },
	{ "ERASE",   cmd_del,     NULL },
	{ "EXIT",    cmd_exit,    "Leave this shell (EXIT /B leaves a batch file)" },
	{ "FIND",    cmd_find,    "Find text: FIND \"text\" [files] (/V /C /I)" },
	{ "FOR",     cmd_for,     "FOR %V IN (set) DO command" },
	{ "GOTO",    cmd_goto,    "Jump to a :label in a batch file" },
	{ "HELP",    cmd_help,    "This list" },
	{ "HWINFO",  cmd_hwinfo,  "Show PCI devices and disks" },
	{ "IF",      cmd_if,      "IF [NOT] EXIST f | ERRORLEVEL n | a==b  command" },
	{ "LABEL",   cmd_label,   "Set a drive's volume label" },
	{ "MD",      cmd_md,      "Make a directory" },
	{ "MEM",     cmd_mem,     "Show memory usage" },
	{ "MKDIR",   cmd_md,      NULL },
	{ "MORE",    cmd_more,    "Show input a screen at a time" },
	{ "MOVE",    cmd_move,    "Move or rename files and directories" },
	{ "PATH",    cmd_path,    "Show or set where programs are found" },
	{ "PAUSE",   cmd_pause,   "Wait for a key" },
	{ "PROMPT",  cmd_prompt,  "Set the prompt ($P path $G > $D date $T time)" },
	{ "RD",      cmd_rd,      "Remove an empty directory" },
	{ "REBOOT",  cmd_reboot,  "Restart the computer" },
	{ "REM",     cmd_rem,     "Comment" },
	{ "REN",     cmd_ren,     "Rename a file" },
	{ "RENAME",  cmd_ren,     NULL },
	{ "RMDIR",   cmd_rd,      NULL },
	{ "SET",     cmd_set,     "Show or set environment variables" },
	{ "SHIFT",   cmd_shift,   "Shift batch parameters %1..%9" },
	{ "SORT",    cmd_sort,    "Sort lines (/R reverse)" },
	{ "START",   cmd_start,   "Run a command as a separate task" },
	{ "TASKS",   cmd_tasks,   "List running tasks" },
	{ "TIME",    cmd_time,    "Show the time" },
	{ "TOUCH",   cmd_touch,   "Create a file or update its time" },
	{ "TREE",    cmd_tree,    "Show the directory tree (/F with files)" },
	{ "TYPE",    cmd_type,    "Display text files" },
	{ "VER",     cmd_ver,     "Show the bdos version" },
	{ "VOL",     cmd_vol,     "Show a drive's label" },
	{ "WIN",     cmd_win,     "Start the desktop (WIN \\\\HOST for a remote one)" },
	{ "XCOPY",   cmd_xcopy,   "Copy directories and everything in them" },
};

/* Unix names. A command's own name (dir, sort...) keeps DOS parsing. */
struct alias {
	const char *name;
	const char *target;
};

static const struct alias aliases[] = {
	{ "ls",     "DIR" },
	{ "cat",    "TYPE" },
	{ "cp",     "COPY" },
	{ "mv",     "MOVE" },
	{ "rm",     "DEL" },
	{ "mkdir",  "MD" },
	{ "rmdir",  "RD" },
	{ "clear",  "CLS" },
	{ "pwd",    "CD" },
	{ "free",   "MEM" },
	{ "uname",  "VER" },
	{ "man",    "HELP" },
	{ "grep",   "FIND" },
	{ "less",   "MORE" },
	{ "ps",     "TASKS" },
	{ "nano",   "EDIT" },
	{ "vi",     "EDIT" },
	{ "df",     "DRIVES" },
	{ "lspci",  "HWINFO" },
	{ "env",    "SET" },
};

static int cmd_help(struct cmd *c)
{
	struct pager pg;
	pager_init(&pg, 1);
	kprintf("\n");
	pager_line(&pg);
	for (size_t i = 0; i < NELEM(commands); i++)
		if (commands[i].help) {
			kprintf("  %-8s %s\n", commands[i].name, commands[i].help);
			if (pager_line(&pg))
				return 0;
		}
	(void)c;
	kprintf("\nRun programs (.BAT, .BAS, .COM, .C) by typing their name.\n"
	    "Redirect with > >> < and chain with |. Unix names work too: "
	    "ls, cat, cp, rm...\nType ALIAS to see them all.\n\n");
	return 0;
}

static int cmd_alias(struct cmd *c)
{
	(void)c;
	for (size_t i = 0; i < NELEM(aliases); i++)
		if (strcasecmp(aliases[i].name, aliases[i].target))
			kprintf("  %-8s = %s\n", aliases[i].name, aliases[i].target);
	return 0;
}

/* ---- running programs -------------------------------------------------------------- */

static const struct command *find_command(const char *name, int *unix_style)
{
	for (size_t i = 0; i < NELEM(aliases); i++) {
		/* aliases are case-sensitive, like Unix */
		if (!strcmp(aliases[i].name, name)) {
			*unix_style = 1;
			for (size_t k = 0; k < NELEM(commands); k++)
				if (!strcmp(commands[k].name, aliases[i].target))
					return &commands[k];
		}
	}
	*unix_style = 0;
	for (size_t i = 0; i < NELEM(commands); i++)
		if (!strcasecmp(commands[i].name, name))
			return &commands[i];
	return NULL;
}

static const char *const prog_exts[] = { ".COM", ".BAT", ".BAS", ".C" };

/* Look for NAME, NAME.COM, NAME.BAT, NAME.BAS, NAME.C here and on PATH. */
static int find_program(struct shell *sh, const char *name, char *out, size_t n)
{
	char dirs[LINE_MAX];
	const char *path = getvar(sh, "PATH");
	snprintf(dirs, sizeof dirs, ".;%s", path ? path : "");
	int explicit_dir = strchr(name, '\\') || strchr(name, '/') || strchr(name, ':');
	const char *dot = strrchr(vfs_basename(name), '.');

	char *save = dirs;
	while (save) {
		char *d = save;
		char *semi = strchr(save, ';');
		if (semi)
			*semi = 0;
		save = semi ? semi + 1 : NULL;
		if (!*d)
			continue;
		char base[VFS_PATH_MAX], cand[VFS_PATH_MAX];
		if (explicit_dir) {
			if (resolve_path(name, base, sizeof base))
				return 0;
			save = NULL;
		} else {
			char dir[VFS_PATH_MAX];
			if (resolve_path(d, dir, sizeof dir) || vfs_join(dir, name, base, sizeof base))
				continue;
		}
		struct vstat st;
		if (dot) {
			for (size_t i = 0; i < NELEM(prog_exts); i++)
				if (!strcasecmp(dot, prog_exts[i]) && !vfs_stat(base, &st) &&
				    st.type == VFS_FILE) {
					strncpy0(out, base, n);
					return 1;
				}
			continue;
		}
		for (size_t i = 0; i < NELEM(prog_exts); i++) {
			snprintf(cand, sizeof cand, "%s%s", base, prog_exts[i]);
			if (!vfs_stat(cand, &st) && st.type == VFS_FILE) {
				strncpy0(out, cand, n);
				return 1;
			}
		}
	}
	return 0;
}

static int run_program(struct shell *sh, const char *path, int argc, char **argv)
{
	const char *ext = strrchr(path, '.');
	if (ext && !strcasecmp(ext, ".BAT"))
		return run_batch(path, argc, argv);
	if (ext && !strcasecmp(ext, ".BAS"))
		return basic_run_file(path, argc, argv);
	if (ext && !strcasecmp(ext, ".COM")) {
		extern int com_run(const char *path, int argc, char **argv);
		return com_run(path, argc, argv);
	}
	if (ext && !strcasecmp(ext, ".C")) {
		extern int cint_run_file(const char *path, int argc, char **argv);
		return cint_run_file(path, argc, argv);
	}
	(void)sh;
	kprintf("Don't know how to run %s\n", path);
	return 1;
}

static const struct command *find_command(const char *name, int *unix_style);

/*
 * Split a command line into words. The command word also ends at '/', '\'
 * or ".." so that DOS habits like "dir/w", "cd\" and "cd.." work, and
 * "ECHO." prints a blank line. Double quotes group words.
 */
static int tokenize(char *line, char **argv, char *cmdbuf, size_t cmdlen)
{
	int argc = 0;
	char *p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return 0;

	argv[argc++] = p;
	if (*p == '"') {			/* quoted program name */
		argv[0] = ++p;
		while (*p && *p != '"')
			p++;
		if (*p)
			*p++ = 0;
	} else {
		char *end = p;
		while (*end && *end != ' ' && *end != '\t')
			end++;
		/* "dir/w", "cd\", "cd..", "echo.": split only after a built-in name */
		char *split = NULL;
		for (char *k = p + 1; k < end && !split; k++) {
			int sep = (*k == '/' || *k == '\\' ||
			    (*k == '.' && (k + 1 == end || k[1] == '.')));
			if (!sep)
				continue;
			size_t n = MIN((size_t)(k - p), cmdlen - 1);
			memcpy(cmdbuf, p, n);
			cmdbuf[n] = 0;
			int u;
			if (find_command(cmdbuf, &u))
				split = k;
		}
		if (split) {
			argv[0] = cmdbuf;	/* already holds the prefix */
			p = split;
		} else {
			p = end;
			if (*p)
				*p++ = 0;
		}
	}
	while (*p && argc < MAX_ARGS - 1) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"')
				p++;
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	argv[argc] = NULL;
	return argc;
}

/* Run one simple command (no pipes or redirection left). */
static int exec_simple(char *line)
{
	struct shell *sh = sh_cur();
	struct cmd c;
	char cmdbuf[64];
	memset(&c, 0, sizeof c);
	c.sh = sh;
	c.argc = tokenize(line, c.argv, cmdbuf, sizeof cmdbuf);
	if (!c.argc)
		return sh->errorlevel;
	const char *name = c.argv[0];

	if (strlen(name) == 2 && name[1] == ':' && isalpha(name[0])) {
		char drive = (char)toupper(name[0]);
		if (!vfs_drive(drive)) {
			kprintf("Invalid drive specification\n");
			return 1;
		}
		struct task *t = task_current();
		if (t->cwd[0] != drive)
			snprintf(t->cwd, sizeof t->cwd, "%c:\\", drive);
		return 0;
	}
	if (name[0] == ':')		/* a batch label */
		return sh->errorlevel;

	const struct command *cmd = find_command(name, &c.unix_style);
	if (cmd)
		return cmd->fn(&c);

	char prog[VFS_PATH_MAX];
	if (find_program(sh, name, prog, sizeof prog))
		return run_program(sh, prog, c.argc, c.argv);
	kprintf("Bad command or file name\n");
	return 1;
}

/* Expand %VAR% (and %0-%9 inside batch files). */
static void expand_vars(struct shell *sh, const char *in, char *out, size_t n)
{
	size_t o = 0;
	for (const char *p = in; *p && o + 1 < n; p++) {
		if (*p != '%') {
			out[o++] = *p;
			continue;
		}
		if (p[1] == '%') {		/* %% -> % */
			out[o++] = '%';
			p++;
			continue;
		}
		if (sh->bat && isdigit(p[1])) {
			int i = p[1] - '0';
			const char *v = i < sh->bat->argc ? sh->bat->args[i] : "";
			while (*v && o + 1 < n)
				out[o++] = *v++;
			p++;
			continue;
		}
		const char *end = strchr(p + 1, '%');
		if (!end || end == p + 1) {
			out[o++] = '%';
			continue;
		}
		char name[64];
		size_t l = MIN((size_t)(end - p - 1), sizeof name - 1);
		memcpy(name, p + 1, l);
		name[l] = 0;
		if (!strcmp(name, "ERRORLEVEL")) {
			char num[16];
			snprintf(num, sizeof num, "%d", sh->errorlevel);
			for (const char *v = num; *v && o + 1 < n; v++)
				out[o++] = *v;
		} else if (!strcasecmp(name, "CD")) {
			for (const char *v = task_current()->cwd; *v && o + 1 < n; v++)
				out[o++] = *v;
		} else {
			const char *v = getvar(sh, name);
			if (v)
				while (*v && o + 1 < n)
					out[o++] = *v++;
		}
		p = end;
	}
	out[o] = 0;
}

/*
 * Pull redirections out of a segment: "> f", ">> f", "< f" (outside quotes).
 * The segment is edited in place to leave just the command.
 */
static void take_redirects(char *seg, char *out_file, size_t on, int *append, char *in_file,
    size_t inn)
{
	out_file[0] = in_file[0] = 0;
	*append = 0;
	int q = 0;
	for (char *p = seg; *p; p++) {
		if (*p == '"')
			q = !q;
		if (q || (*p != '>' && *p != '<'))
			continue;
		int is_in = *p == '<';
		char *start = p;
		if (!is_in && p[1] == '>') {
			*append = 1;
			p++;
		}
		p++;
		while (*p == ' ')
			p++;
		char *dst = is_in ? in_file : out_file;
		size_t dn = is_in ? inn : on, k = 0;
		int fq = 0;
		if (*p == '"') {
			fq = 1;
			p++;
		}
		while (*p && (fq ? *p != '"' : (*p != ' ' && *p != '>' && *p != '<'))) {
			if (k + 1 < dn)
				dst[k++] = *p;
			p++;
		}
		if (fq && *p == '"')
			p++;
		dst[k] = 0;
		memmove(start, p, strlen(p) + 1);
		p = start - 1;
	}
}

static int run_line(char *raw)
{
	struct shell *sh = sh_cur();
	char line[LINE_MAX];
	while (*raw == ' ' || *raw == '\t')
		raw++;
	int quiet = 0;
	if (*raw == '@') {
		quiet = 1;
		raw++;
	}
	if (sh->bat && sh->echo_on && !quiet && *raw && *raw != ':') {
		print_prompt(sh);
		kprintf("%s\n", raw);
	}
	expand_vars(sh, raw, line, sizeof line);

	/* split on | (outside quotes) */
	char *segs[8];
	int nseg = 0, q = 0;
	segs[nseg++] = line;
	for (char *p = line; *p; p++) {
		if (*p == '"')
			q = !q;
		if (*p == '|' && !q && nseg < 8) {
			*p = 0;
			segs[nseg++] = p + 1;
		}
	}

	struct task *t = task_current();
	struct console *real = t->con;
	char *pipe_in = NULL;
	size_t pipe_len = 0;
	int rc = 0;

	for (int i = 0; i < nseg; i++) {
		char out_file[VFS_PATH_MAX], in_file[VFS_PATH_MAX];
		int append;
		take_redirects(segs[i], out_file, sizeof out_file, &append, in_file, sizeof in_file);
		int last = (i == nseg - 1);

		struct wrapcon w;
		wrap_init(&w, real);
		char *in_data = NULL;
		if (in_file[0]) {
			char abs[VFS_PATH_MAX];
			int err = resolve_path(in_file, abs, sizeof abs);
			if (!err)
				in_data = vfs_slurp(abs, &w.ilen, &err);
			if (!in_data) {
				kprintf("%s - %s\n", vfs_strerror(err), in_file);
				rc = 1;
				break;
			}
			w.in = in_data;
			w.has_in = 1;
		} else if (pipe_in) {
			w.in = pipe_in;
			w.ilen = pipe_len;
			w.has_in = 1;
		}
		w.capture = out_file[0] || !last;

		t->con = &w.c;
		rc = exec_simple(segs[i]);
		t->con = (t->con == &w.c) ? real : t->con;	/* CTTY may have changed it */
		if (t->con != real)
			real = t->con;

		kfree(in_data);
		kfree(pipe_in);
		pipe_in = NULL;
		if (out_file[0]) {
			char abs[VFS_PATH_MAX];
			int err = resolve_path(out_file, abs, sizeof abs);
			struct vfile *f = err ? NULL : vfs_open(abs,
			    O_WRITE | O_CREATE | (append ? O_APPEND : O_TRUNC), &err);
			if (!f) {
				kprintf("%s - %s\n", vfs_strerror(err), out_file);
				rc = 1;
			} else {
				if (w.olen)
					vfs_write(f, w.out, w.olen);
				vfs_close(f);
			}
			kfree(w.out);
			if (!last) {	/* "a > f | b": b reads nothing */
				pipe_in = kzalloc(1);
				pipe_len = 0;
			}
		} else if (!last) {
			pipe_in = w.out ? w.out : kzalloc(1);
			pipe_len = w.olen;
		}
	}
	kfree(pipe_in);
	sh->errorlevel = rc;
	return rc;
}

int shell_exec(const char *line)
{
	char buf[LINE_MAX];
	strncpy0(buf, line, sizeof buf);
	return run_line(buf);
}

/* ---- batch files ---------------------------------------------------------------- */

static int run_batch(const char *path, int argc, char **argv)
{
	struct shell *sh = sh_cur();
	int err;
	size_t len;
	char *text = vfs_slurp(path, &len, &err);
	if (!text) {
		kprintf("%s - %s\n", vfs_strerror(err), path);
		return 1;
	}
	struct batch *b = kzalloc(sizeof *b);
	int count = 1;
	for (size_t i = 0; i < len; i++)
		if (text[i] == '\n')
			count++;
	b->lines = kmalloc(sizeof(char *) * (size_t)count);
	b->text = text;
	char *p = text;
	for (size_t i = 0; i <= len; i++) {
		if (i == len || text[i] == '\n') {
			text[i] = 0;
			if (i && text[i - 1] == '\r')
				text[i - 1] = 0;
			b->lines[b->nlines++] = p;
			p = text + i + 1;
		}
	}
	b->argc = MIN(argc, 10);
	for (int i = 0; i < b->argc; i++)
		b->args[i] = kstrdup(i == 0 ? path : argv[i]);
	b->prev = sh->bat;
	sh->bat = b;

	int rc = 0;
	while (b->pc < b->nlines) {
		if (con_pollkey() == 3) {	/* ^C between lines */
			kprintf("^C\nBatch job terminated\n");
			break;
		}
		char line[LINE_MAX];
		strncpy0(line, b->lines[b->pc++], sizeof line);
		rc = run_line(line);
		if (sh->exit_req)
			break;
	}
	sh->bat = b->prev;
	for (int i = 0; i < b->argc; i++)
		kfree(b->args[i]);
	kfree(b->lines);
	kfree(b->text);
	kfree(b);
	return rc;
}

static int cmd_call(struct cmd *c)
{
	if (c->argc < 2)
		return 0;
	char prog[VFS_PATH_MAX];
	if (!find_program(c->sh, c->argv[1], prog, sizeof prog)) {
		kprintf("Bad command or file name\n");
		return 1;
	}
	return run_program(c->sh, prog, c->argc - 1, c->argv + 1);
}

static int cmd_goto(struct cmd *c)
{
	struct batch *b = c->sh->bat;
	if (!b || c->argc < 2)
		return 0;
	const char *label = c->argv[1][0] == ':' ? c->argv[1] + 1 : c->argv[1];
	if (!strcasecmp(label, "EOF")) {
		b->pc = b->nlines;
		return 0;
	}
	for (int i = 0; i < b->nlines; i++) {
		const char *l = b->lines[i];
		while (*l == ' ')
			l++;
		if (*l == ':' && !strncasecmp(l + 1, label, strlen(label)) &&
		    (!l[1 + strlen(label)] || l[1 + strlen(label)] == ' ')) {
			b->pc = i + 1;
			return 0;
		}
	}
	kprintf("Label not found - %s\n", label);
	b->pc = b->nlines;
	return 1;
}

static int cmd_shift(struct cmd *c)
{
	struct batch *b = c->sh->bat;
	if (!b || b->argc < 2)
		return 0;
	kfree(b->args[1]);
	for (int i = 1; i + 1 < b->argc; i++)
		b->args[i] = b->args[i + 1];
	b->argc--;
	return 0;
}

/* Rebuild "rest of the command line" from argv[from..]. */
static void join_args(struct cmd *c, int from, char *out, size_t n)
{
	out[0] = 0;
	for (int i = from; i < c->argc; i++) {
		int quote = strchr(c->argv[i], ' ') != NULL;
		if (quote)
			strcat0(out, "\"", n);
		strcat0(out, c->argv[i], n);
		if (quote)
			strcat0(out, "\"", n);
		if (i + 1 < c->argc)
			strcat0(out, " ", n);
	}
}

static int cmd_if(struct cmd *c)
{
	int i = 1, neg = 0, cond;
	if (i < c->argc && !strcasecmp(c->argv[i], "NOT")) {
		neg = 1;
		i++;
	}
	if (i + 1 >= c->argc) {
		kprintf("Syntax error\n");
		return 1;
	}
	if (!strcasecmp(c->argv[i], "EXIST")) {
		char abs[VFS_PATH_MAX];
		struct vstat st;
		cond = !resolve_path(c->argv[i + 1], abs, sizeof abs) && !vfs_stat(abs, &st);
		if (!cond && has_wild(c->argv[i + 1])) {
			char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
			struct vstat *v;
			int n;
			if (!dir_and_pattern(c->argv[i + 1], dir, sizeof dir, pat, sizeof pat) &&
			    !expand(dir, pat, 0, &v, &n)) {
				cond = n > 0;
				kfree(v);
			}
		}
		i += 2;
	} else if (!strcasecmp(c->argv[i], "ERRORLEVEL")) {
		cond = c->sh->errorlevel >= strtol(c->argv[i + 1], NULL, 10);
		i += 2;
	} else {
		/* a==b, written as one word or split around the == */
		char lhs[LINE_MAX], rhs[LINE_MAX];
		char *eq = strstr(c->argv[i], "==");
		if (eq) {
			*eq = 0;
			strncpy0(lhs, c->argv[i], sizeof lhs);
			strncpy0(rhs, eq + 2, sizeof rhs);
			i++;
		} else if (i + 1 < c->argc && !strncmp(c->argv[i + 1], "==", 2)) {
			strncpy0(lhs, c->argv[i], sizeof lhs);
			strncpy0(rhs, c->argv[i + 1] + 2, sizeof rhs);
			i += 2;
			if (!rhs[0] && i < c->argc && strcmp(c->argv[i - 1], "==\"\""))
				strncpy0(rhs, c->argv[i++], sizeof rhs);
		} else {
			kprintf("Syntax error\n");
			return 1;
		}
		/* the tokenizer keeps quotes glued to ==; drop them */
		for (char *sd = lhs; sd; sd = sd == lhs ? rhs : NULL) {
			size_t l = strlen(sd);
			if (l >= 2 && sd[0] == '"' && sd[l - 1] == '"') {
				memmove(sd, sd + 1, l - 2);
				sd[l - 2] = 0;
			} else if (l == 1 && sd[0] == '"') {
				sd[0] = 0;
			}
		}
		cond = !strcmp(lhs, rhs);
	}
	if (cond == neg)
		return c->sh->errorlevel;
	char rest[LINE_MAX];
	join_args(c, i, rest, sizeof rest);
	return run_line(rest);
}

static int cmd_for(struct cmd *c)
{
	/* FOR %V IN (a b c) DO command %V */
	if (c->argc < 6 || c->argv[1][0] != '%' || strcasecmp(c->argv[2], "IN")) {
		kprintf("Syntax error\n");
		return 1;
	}
	const char *var = c->argv[1];
	int i = 3;
	char set[LINE_MAX] = "";
	for (; i < c->argc && strcasecmp(c->argv[i], "DO"); i++) {
		strcat0(set, c->argv[i], sizeof set);
		strcat0(set, " ", sizeof set);
	}
	if (i >= c->argc - 1) {
		kprintf("Syntax error\n");
		return 1;
	}
	char body[LINE_MAX];
	join_args(c, i + 1, body, sizeof body);
	char *s = set;
	while (*s == ' ' || *s == '(')
		s++;
	char *end = strrchr(s, ')');
	if (end)
		*end = 0;

	int rc = 0;
	char *item = s;
	while (*item) {
		while (*item == ' ')
			item++;
		if (!*item)
			break;
		char *e = item;
		while (*e && *e != ' ')
			e++;
		char save = *e;
		*e = 0;

		/* wildcards in the set expand to file names */
		char **names = NULL;
		int nnames = 0;
		struct vstat *v = NULL;
		char dir[VFS_PATH_MAX], pat[VFS_NAME_MAX];
		if (has_wild(item) && !dir_and_pattern(item, dir, sizeof dir, pat, sizeof pat) &&
		    !expand(dir, pat, 0, &v, &nnames)) {
			names = kmalloc(sizeof(char *) * (size_t)(nnames + 1));
			for (int k = 0; k < nnames; k++)
				names[k] = v[k].name;
		} else {
			names = kmalloc(sizeof(char *));
			names[0] = item;
			nnames = 1;
		}
		for (int k = 0; k < nnames && names; k++) {
			char cmdline[LINE_MAX];
			size_t o = 0, vl = strlen(var);
			for (const char *b = body; *b && o + 1 < sizeof cmdline; ) {
				if (!strncmp(b, var, vl)) {
					for (const char *x = names[k]; *x && o + 1 < sizeof cmdline; x++)
						cmdline[o++] = *x;
					b += vl;
				} else {
					cmdline[o++] = *b++;
				}
			}
			cmdline[o] = 0;
			rc = run_line(cmdline);
		}
		kfree(names);
		kfree(v);
		*e = save;
		item = e;
	}
	return rc;
}

/* ---- the prompt and the line editor ------------------------------------------------ */

static void print_prompt(struct shell *sh)
{
	char out[256];
	size_t o = 0;
	struct task *t = task_current();
	for (const char *p = sh->prompt; *p && o + 64 < sizeof out; p++) {
		if (*p != '$' || !p[1]) {
			out[o++] = *p;
			continue;
		}
		p++;
		int y, mo, d, h, mi, s;
		switch (toupper(*p)) {
		case 'P':
			for (const char *c = t->cwd; *c && o + 1 < sizeof out; c++)
				out[o++] = *c;
			break;
		case 'N':
			out[o++] = t->cwd[0];
			break;
		case 'G': out[o++] = '>'; break;
		case 'L': out[o++] = '<'; break;
		case 'B': out[o++] = '|'; break;
		case 'Q': out[o++] = '='; break;
		case '$': out[o++] = '$'; break;
		case '_': out[o++] = '\n'; break;
		case 'D':
			arch_rtc(&y, &mo, &d, &h, &mi, &s);
			o += (size_t)snprintf(out + o, sizeof out - o, "%04d-%02d-%02d", y, mo, d);
			break;
		case 'T':
			arch_rtc(&y, &mo, &d, &h, &mi, &s);
			o += (size_t)snprintf(out + o, sizeof out - o, "%02d:%02d:%02d", h, mi, s);
			break;
		case 'V':
			o += (size_t)snprintf(out + o, sizeof out - o, "%s %s", BDOS_NAME, BDOS_VERSION);
			break;
		}
	}
	out[o] = 0;
	con_write(out);
}

static void erase_line(size_t n)
{
	while (n--)
		con_write("\b \b");
}

int read_line(char *buf, size_t cap)
{
	struct shell *sh = sh_cur();
	size_t len = 0;
	int hpos = sh ? sh->hist_len : 0;
	int piped = stdin_is_pipe();
	buf[0] = 0;

	if (!piped)
		con_cursor(1);
	for (;;) {
		int k = con_getkey();
		if (k == KEY_EOF) {
			if (len && piped)
				break;		/* last line without a newline */
			if (!piped)
				con_write("^Z\n");
			con_cursor(0);
			return -1;
		}
		if (k == '\n') {
			if (!piped)
				con_putc('\n');
			break;
		}
		if (piped) {
			if (k >= 0 && k < 0x100 && k != '\r' && len + 1 < cap)
				buf[len++] = (char)k;
			continue;
		}
		if (k == '\b') {
			if (len) {
				len--;
				con_write("\b \b");
			}
		} else if (k == 3) {
			con_write("^C\n");
			len = 0;
			break;
		} else if (k == 27) {
			erase_line(len);
			len = 0;
		} else if ((k == KEY_UP || k == KEY_DOWN) && sh) {
			int np = hpos + (k == KEY_UP ? -1 : 1);
			if (np < 0 || np > sh->hist_len)
				continue;
			hpos = np;
			erase_line(len);
			if (hpos == sh->hist_len)
				buf[0] = 0;
			else
				strncpy0(buf, sh->history[hpos], cap);
			len = strlen(buf);
			con_write(buf);
		} else if (k == '\t') {
			/* filename completion on the last word */
			buf[len] = 0;
			char *word = strrchr(buf, ' ');
			word = word ? word + 1 : buf;
			char pat[VFS_PATH_MAX], dir[VFS_PATH_MAX], wpat[VFS_NAME_MAX];
			snprintf(pat, sizeof pat, "%s*", word);
			struct vstat *v;
			int n;
			if (!dir_and_pattern(pat, dir, sizeof dir, wpat, sizeof wpat) &&
			    !expand(dir, wpat, 0, &v, &n)) {
				if (n == 1) {
					const char *base = vfs_basename(word);
					const char *add = v[0].name + strlen(base);
					for (; *add && len + 2 < cap; add++) {
						buf[len++] = *add;
						con_putc(*add);
					}
					if (v[0].type == VFS_DIR && len + 2 < cap) {
						buf[len++] = '\\';
						con_putc('\\');
					}
				} else if (n > 1) {
					con_putc('\n');
					for (int i = 0; i < n; i++)
						kprintf("%s%s  ", v[i].name, v[i].type == VFS_DIR ? "\\" : "");
					con_putc('\n');
					print_prompt(sh);
					con_write(buf);
				}
				kfree(v);
			}
		} else if (k >= 0x20 && k < 0x7F && len + 1 < cap) {
			buf[len++] = (char)k;
			buf[len] = 0;
			con_putc((char)k);
		}
		buf[len] = 0;
	}
	buf[len] = 0;
	if (!piped)
		con_cursor(0);
	return (int)len;
}

static void remember(struct shell *sh, const char *line)
{
	if (!*line || (sh->hist_len && !strcmp(sh->history[sh->hist_len - 1], line)))
		return;
	if (sh->hist_len == HIST_MAX) {
		memmove(sh->history[0], sh->history[1], sizeof sh->history[0] * (HIST_MAX - 1));
		sh->hist_len--;
	}
	strncpy0(sh->history[sh->hist_len++], line, LINE_MAX);
}

/* ---- the shell task --------------------------------------------------------------- */

static struct shell *shell_new(void)
{
	struct shell *sh = kzalloc(sizeof *sh);
	if (!sh)
		return NULL;
	sh->echo_on = 1;
	strcpy(sh->prompt, "$P$G");
	/* inherit the parent shell's variables, if any */
	struct shell *parent = task_current()->user;
	if (parent && parent != sh) {
		for (int i = 0; i < parent->nvars; i++)
			setvar(sh, parent->var_name[i], parent->var_val[i]);
		strncpy0(sh->prompt, parent->prompt, sizeof sh->prompt);
	}
	return sh;
}

void shell_task(void *arg)
{
	struct task *t = task_current();
	struct shell *sh = shell_new();
	if (!sh)
		return;
	t->user = sh;
	if (!getvar(sh, "PATH"))
		setvar(sh, "PATH", "C:\\BDOS");
	if (!getvar(sh, "COMSPEC"))
		setvar(sh, "COMSPEC", "C:\\COMMAND");

	if (arg) {
		/* START: run one command line, then end */
		char *line = arg;
		run_line(line);
		kfree(line);
		return;
	}

	static int first = 1;
	if (first) {
		first = 0;
		char ae[VFS_PATH_MAX];
		struct vstat st;
		if (vfs_drive('C') && !vfs_resolve("C:\\", "AUTOEXEC.BAT", ae, sizeof ae) &&
		    !vfs_stat(ae, &st)) {
			char *argv[1] = { ae };
			run_batch(ae, 1, argv);
		}
		sh->echo_on = 1;
		sh->exit_req = 0;
	}
	if (!vfs_drive(t->cwd[0])) {
		/* no C: drive - sit at the first one that exists, if any */
		for (char d = 'A'; d <= 'Z'; d++)
			if (vfs_drive(d)) {
				snprintf(t->cwd, sizeof t->cwd, "%c:\\", d);
				break;
			}
	}

	char line[LINE_MAX];
	while (!sh->exit_req) {
		print_prompt(sh);
		if (read_line(line, sizeof line) < 0)
			break;
		remember(sh, line);
		run_line(line);
	}
	for (int i = 0; i < sh->nvars; i++) {
		kfree(sh->var_name[i]);
		kfree(sh->var_val[i]);
	}
	t->user = NULL;
	kfree(sh);
}
