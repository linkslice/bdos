/*
 * remote.c - sessions between bdos machines.
 *
 *   CTTY \\HOST        a shell on HOST, in this console
 *   WIN \\HOST         HOST's desktop, full screen or in this window
 *   NET USE X: \\HOST\path   map a directory on HOST as drive X:
 *
 * Inside CTTY and WIN sessions, drive T: on the remote machine is the
 * directory you started from here, the way /mnt/term works with drawterm.
 * Files, keys and pixels all travel over the same BDNET connection.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../gui/wm.h"
#include "net.h"
#include "../lz.h"

enum {
	M_KEY = 1, M_OUT, M_CTRL, M_EV, M_PIX, M_FREQ, M_FREP, M_BYE, M_PTR,
	M_PIXZ,		/* M_PIX's payload squashed (lz.c): u32 its length, then the block */
};

enum { C_CLEAR = 1, C_GOTO, C_ATTR, C_CURSOR };

enum { F_STAT = 1, F_LIST, F_READ, F_WRITE, F_CREATE, F_TRUNC, F_REMOVE, F_RENAME,
	F_SETATTR, F_INFO };

#define HELLO_SHARE	1
#define HELLO_CMD	2	/* run this one command and hang up (CPU \\HOST cmd) */
#define HELLO_POINTER	2
#define HELLO_LZ	4	/* the viewer understands M_PIXZ */

struct hello {
	int32_t w, h;		/* columns/rows (TTY) or pixels (DESK) */
	uint8_t flags;
	char host[32];
	char share[128];	/* the directory offered as T: */
	char cmd[160];		/* HELLO_CMD: the command line */
} __attribute__((packed));

static size_t strnlen_b(const char *s, size_t max)
{
	size_t n = 0;
	while (n < max && s[n])
		n++;
	return n;
}

int net_unuse(char letter);

/* ---- the file protocol ------------------------------------------------------------- */

struct freq {
	uint32_t id;
	uint8_t op, arg;
	uint16_t pad;
	uint64_t off;
	uint32_t n;
	/* then: path\0, then path2\0 or data */
} __attribute__((packed));

struct frep {
	uint32_t id;
	int32_t result;
} __attribute__((packed));

struct wire_stat {
	uint8_t type, attr;
	uint64_t size, mtime;
	char short_name[13];
	char name[VFS_NAME_MAX];
} __attribute__((packed));

static void to_wire(const struct vstat *st, struct wire_stat *w)
{
	memset(w, 0, sizeof *w);
	w->type = (uint8_t)st->type;
	w->attr = st->attr;
	w->size = st->size;
	w->mtime = st->mtime;
	strncpy0(w->short_name, st->short_name, sizeof w->short_name);
	strncpy0(w->name, st->name, sizeof w->name);
}

static void from_wire(const struct wire_stat *w, struct vstat *st)
{
	memset(st, 0, sizeof *st);
	st->type = w->type;
	st->attr = w->attr;
	st->size = w->size;
	st->mtime = w->mtime;
	strncpy0(st->short_name, w->short_name, sizeof st->short_name);
	strncpy0(st->name, w->name, sizeof st->name);
}

/* The serving side: map "a/b" under root to a local absolute path. */
static int serve_path(const char *root, const char *rel, char *out, size_t n)
{
	strncpy0(out, root, n);
	const char *p = rel;
	while (*p) {
		while (*p == '/' || *p == '\\')
			p++;
		if (!*p)
			break;
		char part[VFS_NAME_MAX];
		size_t l = 0;
		while (*p && *p != '/' && *p != '\\' && l + 1 < sizeof part)
			part[l++] = *p++;
		part[l] = 0;
		if (!strcmp(part, "..") || !strcmp(part, "."))
			return E_ACCES;	/* stay inside what was shared */
		if (vfs_join(out, part, out, n))
			return E_NAMETOOLONG;
	}
	return 0;
}

struct list_buf {
	uint8_t *data;
	size_t len, cap;
	uint32_t count;
};

static int list_cb(void *arg, struct vstat *st)
{
	struct list_buf *lb = arg;
	if (lb->len + sizeof(struct wire_stat) > lb->cap) {
		size_t nc = MAX(lb->cap * 2, lb->len + 64 * sizeof(struct wire_stat));
		uint8_t *nb = krealloc(lb->data, nc);
		if (!nb)
			return 1;
		lb->data = nb;
		lb->cap = nc;
	}
	to_wire(st, (struct wire_stat *)(lb->data + lb->len));
	lb->len += sizeof(struct wire_stat);
	lb->count++;
	return 0;
}

/* Answer one file request against a local directory. */
static void serve_file(struct conn *c, const char *root, struct netmsg *m)
{
	struct freq *q = (struct freq *)m->data;
	struct frep rep = { q->id, 0 };
	if (m->len < sizeof *q) {
		rep.result = E_INVAL;
		conn_send(c, M_FREP, &rep, sizeof rep);
		return;
	}
	const char *path = (const char *)(m->data + sizeof *q);
	size_t plen = strnlen_b(path, m->len - sizeof *q);
	const uint8_t *extra = (const uint8_t *)path + plen + 1;
	size_t extra_len = m->len - sizeof *q - plen - 1;
	char abs[VFS_PATH_MAX], abs2[VFS_PATH_MAX];
	int r = serve_path(root, path, abs, sizeof abs);
	if (r) {
		rep.result = r;
		conn_send(c, M_FREP, &rep, sizeof rep);
		return;
	}
	switch (q->op) {
	case F_STAT: {
		struct vstat st;
		struct wire_stat w;
		rep.result = vfs_stat(abs, &st);
		if (!rep.result && !*path)
			strcpy(st.name, "");
		to_wire(&st, &w);
		conn_send2(c, M_FREP, &rep, sizeof rep, rep.result ? NULL : &w, rep.result ? 0 : sizeof w);
		return;
	}
	case F_LIST: {
		struct list_buf lb = { NULL, 0, 0, 0 };
		rep.result = vfs_list(abs, list_cb, &lb);
		if (!rep.result)
			rep.result = (int32_t)lb.count;
		conn_send2(c, M_FREP, &rep, sizeof rep, lb.data, lb.len);
		kfree(lb.data);
		return;
	}
	case F_READ: {
		uint32_t n = MIN(q->n, 64u * 1024);
		uint8_t *buf = kmalloc(n ? n : 1);
		int err = 0;
		struct vfile *f = buf ? vfs_open(abs, O_READ, &err) : NULL;
		long got = f ? 0 : (buf ? err : E_NOMEM);
		if (f) {
			vfs_seek(f, q->off);
			got = vfs_read(f, buf, n);
			vfs_close(f);
		}
		rep.result = (int32_t)got;
		conn_send2(c, M_FREP, &rep, sizeof rep, buf, got > 0 ? (size_t)got : 0);
		kfree(buf);
		return;
	}
	case F_WRITE: {
		int err = 0;
		struct vfile *f = vfs_open(abs, O_WRITE, &err);
		long w = err;
		if (f) {
			vfs_seek(f, q->off);
			w = vfs_write(f, extra, extra_len);
			vfs_close(f);
		}
		rep.result = (int32_t)w;
		break;
	}
	case F_CREATE:
		rep.result = q->arg == VFS_DIR ? vfs_mkdir(abs) : 0;
		if (q->arg != VFS_DIR) {
			int err = 0;
			struct vfile *f = vfs_open(abs, O_WRITE | O_CREATE, &err);
			if (f)
				vfs_close(f);
			rep.result = f ? 0 : err;
			struct vstat st;
			if (f && !vfs_stat(abs, &st) && st.size)	/* create means new */
				rep.result = E_EXIST;
		}
		break;
	case F_TRUNC: {
		int err = 0;
		struct vfile *f = q->off ? NULL : vfs_open(abs, O_WRITE | O_TRUNC, &err);
		rep.result = q->off ? E_NOTSUP : (f ? 0 : err);
		if (f)
			vfs_close(f);
		break;
	}
	case F_REMOVE: {
		struct vstat st;
		rep.result = vfs_stat(abs, &st);
		if (!rep.result)
			rep.result = st.type == VFS_DIR ? vfs_rmdir(abs) : vfs_delete(abs);
		break;
	}
	case F_RENAME:
		rep.result = serve_path(root, (const char *)extra, abs2, sizeof abs2);
		if (!rep.result)
			rep.result = vfs_rename(abs, abs2);
		break;
	case F_SETATTR:
		rep.result = vfs_setattr(abs, q->arg);
		break;
	case F_INFO: {
		uint8_t info[48];
		char label[32] = "";
		uint64_t free = 0, total = 0;
		rep.result = vfs_info(root[0], label, sizeof label, &free, &total);
		memcpy(info, label, 32);
		memcpy(info + 32, &free, 8);
		memcpy(info + 40, &total, 8);
		conn_send2(c, M_FREP, &rep, sizeof rep, info, sizeof info);
		return;
	}
	default:
		rep.result = E_NOTSUP;
	}
	conn_send(c, M_FREP, &rep, sizeof rep);
}

/* ---- the asking side: a VFS drive backed by a connection ------------------------------ */

struct rfs {
	struct conn *c;
	uint32_t next_id;
	struct netmsg *stash[8];	/* replies that arrived for other requests */
	struct vfs_drive drive;
	char label[32];
};

static struct netmsg *rfs_call(struct rfs *r, int op, int arg, uint64_t off, uint32_t n,
    const char *path, const void *extra, size_t elen, int32_t *result)
{
	uint32_t id = ++r->next_id;
	struct freq q = { id, (uint8_t)op, (uint8_t)arg, 0, off, n };
	size_t pl = strlen(path) + 1;
	uint8_t *body = kmalloc(pl + elen);
	if (!body) {
		*result = E_NOMEM;
		return NULL;
	}
	memcpy(body, path, pl);
	if (elen)
		memcpy(body + pl, extra, elen);
	int s = conn_send2(r->c, M_FREQ, &q, sizeof q, body, pl + elen);
	kfree(body);
	if (s) {
		*result = E_IO;
		return NULL;
	}
	for (;;) {
		for (int i = 0; i < (int)NELEM(r->stash); i++) {
			struct netmsg *m = r->stash[i];
			if (m && ((struct frep *)m->data)->id == id) {
				r->stash[i] = NULL;
				*result = ((struct frep *)m->data)->result;
				return m;
			}
		}
		struct netmsg *m = conn_wait(r->c, MSG(M_FREP), 15000);
		if (!m) {
			*result = E_IO;	/* the other machine went away */
			return NULL;
		}
		if (m->len >= sizeof(struct frep) && ((struct frep *)m->data)->id == id) {
			*result = ((struct frep *)m->data)->result;
			return m;
		}
		int stored = 0;
		for (int i = 0; i < (int)NELEM(r->stash) && !stored; i++)
			if (!r->stash[i]) {
				r->stash[i] = m;
				stored = 1;
			}
		if (!stored)
			kfree(m);
	}
}

static int simple_call(struct rfs *r, int op, int arg, uint64_t off, const char *path,
    const void *extra, size_t elen)
{
	int32_t res;
	struct netmsg *m = rfs_call(r, op, arg, off, 0, path, extra, elen, &res);
	kfree(m);
	return res;
}

static int r_stat(void *ctx, const char *path, struct vstat *st)
{
	int32_t res;
	struct netmsg *m = rfs_call(ctx, F_STAT, 0, 0, 0, path, NULL, 0, &res);
	if (m && !res && m->len >= sizeof(struct frep) + sizeof(struct wire_stat))
		from_wire((struct wire_stat *)(m->data + sizeof(struct frep)), st);
	kfree(m);
	return res;
}

static int r_list(void *ctx, const char *path, int (*cb)(void *, struct vstat *), void *arg)
{
	int32_t res;
	struct netmsg *m = rfs_call(ctx, F_LIST, 0, 0, 0, path, NULL, 0, &res);
	if (!m)
		return res;
	if (res < 0) {
		kfree(m);
		return res;
	}
	struct vstat *st = kmalloc(sizeof *st);
	struct wire_stat *w = (struct wire_stat *)(m->data + sizeof(struct frep));
	size_t have = (m->len - sizeof(struct frep)) / sizeof *w;
	for (size_t i = 0; st && i < have && i < (size_t)res; i++) {
		from_wire(&w[i], st);
		if (cb(arg, st))
			break;
	}
	kfree(st);
	kfree(m);
	return 0;
}

static long r_read(void *ctx, const char *path, uint64_t off, void *buf, size_t n)
{
	size_t done = 0;
	while (done < n) {
		uint32_t chunk = (uint32_t)MIN(n - done, (size_t)64 * 1024);
		int32_t res;
		struct netmsg *m = rfs_call(ctx, F_READ, 0, off + done, chunk, path, NULL, 0, &res);
		if (!m || res < 0) {
			kfree(m);
			return done ? (long)done : res;
		}
		size_t got = MIN((size_t)res, m->len - sizeof(struct frep));
		memcpy((uint8_t *)buf + done, m->data + sizeof(struct frep), got);
		kfree(m);
		done += got;
		if (got < chunk)
			break;
	}
	return (long)done;
}

static long r_write(void *ctx, const char *path, uint64_t off, const void *buf, size_t n)
{
	size_t done = 0;
	while (done < n || !n) {
		size_t chunk = MIN(n - done, (size_t)32 * 1024);
		int res = simple_call(ctx, F_WRITE, 0, off + done, path, (const uint8_t *)buf + done, chunk);
		if (res < 0)
			return done ? (long)done : res;
		done += chunk;
		if (!n)
			break;
	}
	return (long)done;
}

static int r_create(void *ctx, const char *path, int type)
{
	return simple_call(ctx, F_CREATE, type, 0, path, NULL, 0);
}

static int r_truncate(void *ctx, const char *path, uint64_t size)
{
	return simple_call(ctx, F_TRUNC, 0, size, path, NULL, 0);
}

static int r_remove(void *ctx, const char *path)
{
	return simple_call(ctx, F_REMOVE, 0, 0, path, NULL, 0);
}

static int r_rename(void *ctx, const char *from, const char *to)
{
	return simple_call(ctx, F_RENAME, 0, 0, from, to, strlen(to) + 1);
}

static int r_setattr(void *ctx, const char *path, uint8_t attr)
{
	return simple_call(ctx, F_SETATTR, attr, 0, path, NULL, 0);
}

static int r_info(void *ctx, char *label, size_t n, uint64_t *free, uint64_t *total)
{
	struct rfs *r = ctx;
	int32_t res;
	struct netmsg *m = rfs_call(r, F_INFO, 0, 0, 0, "", NULL, 0, &res);
	*free = *total = 0;
	strncpy0(label, r->label, n);
	if (m && !res && m->len >= sizeof(struct frep) + 48) {
		const uint8_t *p = m->data + sizeof(struct frep);
		memcpy(free, p + 32, 8);
		memcpy(total, p + 40, 8);
	}
	kfree(m);
	return 0;
}

static const struct vfs_ops remote_ops = {
	.stat = r_stat,
	.list = r_list,
	.read = r_read,
	.write = r_write,
	.create = r_create,
	.truncate = r_truncate,
	.remove = r_remove,
	.rename = r_rename,
	.setattr = r_setattr,
	.info = r_info,
};

static struct rfs *rfs_new(struct conn *c, char letter, const char *desc, const char *label)
{
	struct rfs *r = kzalloc(sizeof *r);
	if (!r)
		return NULL;
	r->c = c;
	r->drive.letter = letter;
	r->drive.ops = &remote_ops;
	r->drive.ctx = r;
	strncpy0(r->drive.desc, desc, sizeof r->drive.desc);
	strncpy0(r->label, label, sizeof r->label);
	return r;
}

static void rfs_free(struct rfs *r)
{
	for (int i = 0; i < (int)NELEM(r->stash); i++)
		kfree(r->stash[i]);
	kfree(r);
}

/* ---- server: a remote console for CTTY ----------------------------------------------- */

struct rcon {
	struct console c;
	struct conn *conn;
	int cols, rows;
};

static void rc_write(struct console *c, const char *s, size_t n)
{
	struct rcon *r = c->priv;
	conn_send(r->conn, M_OUT, s, n);
}

static int rc_getkey(struct console *c)
{
	struct rcon *r = c->priv;
	if (!conn_alive(r->conn))
		return KEY_EOF;
	struct netmsg *m = conn_take(r->conn, MSG(M_KEY) | MSG(M_BYE));
	if (!m)
		return -1;
	int k = KEY_EOF;
	if (m->type == M_KEY && m->len >= 4)
		memcpy(&k, m->data, 4);
	kfree(m);
	return k;
}

static void rc_ctrl(struct rcon *r, int op, int a, int b)
{
	int32_t msg[3] = { op, a, b };
	conn_send(r->conn, M_CTRL, msg, sizeof msg);
}

static void rc_clear(struct console *c) { rc_ctrl(c->priv, C_CLEAR, 0, 0); }
static void rc_cursor(struct console *c, int on) { rc_ctrl(c->priv, C_CURSOR, on, 0); }
static void rc_gotoxy(struct console *c, int x, int y) { rc_ctrl(c->priv, C_GOTO, x, y); }
static void rc_attr(struct console *c, int fg, int bg) { rc_ctrl(c->priv, C_ATTR, fg, bg); }

static void rc_size(struct console *c, int *w, int *h)
{
	struct rcon *r = c->priv;
	*w = r->cols;
	*h = r->rows;
}

int console_is_remote(struct console *c)
{
	return c && c->write == rc_write;
}

/* WIN typed inside a CTTY session: there's no screen to draw on there. */
int remote_session_desktop(struct console *c)
{
	(void)c;
	kprintf("This is a text session. Use WIN \\\\%s from your own machine for the desktop.\n",
	    net_hostname());
	return 1;
}

static void tty_service(struct conn *c, const uint8_t *hello_raw, size_t hlen)
{
	struct hello h;
	memset(&h, 0, sizeof h);
	memcpy(&h, hello_raw, MIN(hlen, sizeof h));
	struct rcon *r = kzalloc(sizeof *r);
	if (!r) {
		conn_close(c);
		return;
	}
	r->conn = c;
	r->cols = h.w > 10 ? h.w : 80;
	r->rows = h.h > 5 ? h.h : 25;
	r->c.name = "REMOTE";
	r->c.write = rc_write;
	r->c.getkey = rc_getkey;
	r->c.clear = rc_clear;
	r->c.cursor = rc_cursor;
	r->c.size = rc_size;
	r->c.gotoxy = rc_gotoxy;
	r->c.attr = rc_attr;
	r->c.priv = r;

	struct task *t = task_current();
	t->con = &r->c;
	struct rfs *tdr = NULL;
	if (h.flags & HELLO_SHARE) {
		char desc[48];
		snprintf(desc, sizeof desc, "%s on \\\\%s", h.share, h.host);
		tdr = rfs_new(c, 'T', desc, h.host);
		if (tdr)
			t->tdrive = &tdr->drive;
	}
	if (h.flags & HELLO_CMD) {
		/* a CPU server's job: run it from the caller's own directory, T: */
		h.cmd[sizeof h.cmd - 1] = 0;
		if (tdr)
			strcpy(t->cwd, "T:\\");
		shell_task(kstrdup(h.cmd));
	} else {
		kprintf("Connected to \\\\%s (%s %s, %d core%s). T: is %s on \\\\%s. EXIT to disconnect.\n\n",
		    net_hostname(), BDOS_NAME, BDOS_VERSION, ncpus, ncpus == 1 ? "" : "s",
		    h.share[0] ? h.share : "nothing", h.host);
		shell_task(NULL);
	}
	conn_send(c, M_BYE, NULL, 0);
	t->con = NULL;
	t->tdrive = NULL;
	conn_close(c);
	if (tdr)
		rfs_free(tdr);
	kfree(r);
}

/* ---- server: a remote display for WIN ----------------------------------------------- */

struct rdisp {
	struct display d;
	struct surface surf;
	struct conn *c;
	uint32_t *rowbuf;
	/* what changed and hasn't gone yet: sent when the link has room, so a
	 * slow link gets fewer, fresher pictures instead of a growing queue */
	struct rect pend[16];
	int npend;
	int lz;			/* the viewer takes M_PIXZ */
	uint32_t *ztab;
	uint8_t *zbuf;
	/* messages made while drawing (no big kernel lock), sent at commit */
	struct outmsg { uint8_t type; uint8_t *buf; size_t len; } *outq;
	int nout, capout;
};

/* send it now, or keep it for commit if we're drawing without the kernel lock */
static void rd_out(struct rdisp *rd, int type, const void *a, size_t alen, const void *b, size_t blen)
{
	if (bkl_held()) {
		conn_send2(rd->c, type, a, alen, b, blen);
		return;
	}
	if (rd->nout == rd->capout) {
		int nc = rd->capout ? rd->capout * 2 : 16;
		struct outmsg *nq = krealloc(rd->outq, sizeof *nq * (size_t)nc);
		if (!nq)
			return;
		rd->outq = nq;
		rd->capout = nc;
	}
	uint8_t *buf = kmalloc(alen + blen);
	if (!buf)
		return;
	memcpy(buf, a, alen);
	if (blen)
		memcpy(buf + alen, b, blen);
	rd->outq[rd->nout++] = (struct outmsg){ (uint8_t)type, buf, alen + blen };
}

static void rd_commit(struct display *d)
{
	struct rdisp *rd = (struct rdisp *)d;
	for (int i = 0; i < rd->nout; i++) {
		conn_send2(rd->c, rd->outq[i].type, rd->outq[i].buf, rd->outq[i].len, NULL, 0);
		kfree(rd->outq[i].buf);
	}
	rd->nout = 0;
}

/* how much may be queued or in flight before new pictures wait */
#define PIX_BACKLOG (64 * 1024)

#define PIX_MAX_RUNS 8192

static void rd_send_rect(struct rdisp *rd, struct rect r);

/* send what's changed, if the link has room for it */
static void rd_send_pending(struct rdisp *rd)
{
	if (!rd->npend || conn_backlog(rd->c) > PIX_BACKLOG)
		return;
	int n = rd->npend;
	rd->npend = 0;
	for (int i = 0; i < n; i++)
		rd_send_rect(rd, rd->pend[i]);
}

static void rd_flush(struct display *d, struct rect r)
{
	struct rdisp *rd = (struct rdisp *)d;
	if (!conn_alive(rd->c)) {
		d->alive = 0;
		return;
	}
	r = rect_isect(r, (struct rect){ 0, 0, rd->surf.w, rd->surf.h });
	if (rect_empty(r))
		return;
	/* fold it into what's waiting: into one it overlaps, or a new one */
	int merged = 0;
	for (int i = 0; i < rd->npend && !merged; i++) {
		struct rect u = rect_union(rd->pend[i], r);
		long ua = (long)u.w * u.h, sum = (long)rd->pend[i].w * rd->pend[i].h + (long)r.w * r.h;
		if (!rect_empty(rect_isect(rd->pend[i], r)) || ua <= sum + sum / 4) {
			rd->pend[i] = u;
			merged = 1;
		}
	}
	if (!merged) {
		if (rd->npend == (int)NELEM(rd->pend)) {
			for (int i = 1; i < rd->npend; i++)
				rd->pend[0] = rect_union(rd->pend[0], rd->pend[i]);
			rd->npend = 1;
			rd->pend[0] = rect_union(rd->pend[0], r);
		} else {
			rd->pend[rd->npend++] = r;
		}
	}
	rd_send_pending(rd);
}

static void rd_send_rect(struct rdisp *rd, struct rect r)
{
	r = rect_isect(r, (struct rect){ 0, 0, rd->surf.w, rd->surf.h });	/* it may have shrunk since */
	if (rect_empty(r))
		return;
	/* run-length encode in bands so each message stays reasonable; the
	 * header goes just in front of the runs, so the two squash together */
	uint8_t *msg = kmalloc(16 + 6 * PIX_MAX_RUNS);
	if (!msg)
		return;
	struct { uint16_t n; uint32_t rgb; } __attribute__((packed)) *runs = (void *)(msg + 16);
	int y = r.y;
	while (y < r.y + r.h) {
		int nruns = 0, y0 = y;
		uint32_t cur = 0;
		int have = 0;
		for (; y < r.y + r.h && nruns < PIX_MAX_RUNS - r.w; y++) {
			uint32_t *row = rd->surf.px + (size_t)y * (size_t)rd->surf.pitch + r.x;
			for (int x = 0; x < r.w; x++) {
				uint32_t px = row[x];
				if (have && px == cur && runs[nruns - 1].n < 0xFFFF) {
					runs[nruns - 1].n++;
				} else {
					cur = px;
					have = 1;
					runs[nruns].n = 1;
					runs[nruns].rgb = pixel_unpack(px);
					nruns++;
				}
			}
		}
		int32_t hdr[4] = { r.x, y0, r.w, y - y0 };
		memcpy(msg, hdr, 16);
		size_t raw = 16 + sizeof(*runs) * (size_t)nruns, z = 0;
		if (rd->lz && rd->zbuf)
			z = lz_compress(msg, raw, rd->zbuf, lz_bound(16 + 6 * PIX_MAX_RUNS), rd->ztab);
		if (z && z + 4 < raw) {
			uint32_t rl = (uint32_t)raw;
			rd_out(rd, M_PIXZ, &rl, 4, rd->zbuf, z);
		} else {
			rd_out(rd, M_PIX, msg, raw, NULL, 0);
		}
	}
	kfree(msg);
}

static int rd_event(struct display *d, struct event *e)
{
	struct rdisp *rd = (struct rdisp *)d;
	rd_send_pending(rd);	/* the desktop asks often: a good time to catch up */
	if (!conn_alive(rd->c)) {
		d->alive = 0;
		return 0;
	}
	struct netmsg *m = conn_take(rd->c, MSG(M_EV) | MSG(M_BYE));
	if (!m)
		return 0;
	int ok = 0;
	if (m->type == M_BYE) {
		d->alive = 0;
	} else if (m->len >= 5 * sizeof(int32_t)) {
		int32_t v[5];
		memcpy(v, m->data, sizeof v);
		*e = (struct event){ v[0], v[1], v[2], v[3], v[4] };
		ok = 1;
		if (e->type == EV_RESIZE) {
			/* bterm's window changed size: so does the desktop */
			int w = MAX(320, MIN(e->x, 1600)), h = MAX(200, MIN(e->y, 1200));
			struct surface ns;
			if ((w != rd->surf.w || h != rd->surf.h) && !surf_init(&ns, w, h)) {
				surf_free(&rd->surf);
				rd->surf = ns;
			} else {
				ok = 0;
			}
		}
	}
	kfree(m);
	return ok;
}

/* the pointer's shape, for a bterm that draws its own */
static void rd_pointer(struct display *d, int shape)
{
	struct rdisp *rd = (struct rdisp *)d;
	int32_t v = shape;
	if (conn_alive(rd->c))
		conn_send(rd->c, M_PTR, &v, sizeof v);
}

static void desk_service(struct conn *c, const uint8_t *hello_raw, size_t hlen)
{
	struct hello h;
	memset(&h, 0, sizeof h);
	memcpy(&h, hello_raw, MIN(hlen, sizeof h));
	int w = MAX(320, MIN(h.w, 1600)), ht = MAX(200, MIN(h.h, 1200));
	struct rdisp *rd = kzalloc(sizeof *rd);
	if (!rd || surf_init(&rd->surf, w, ht)) {
		kfree(rd);
		conn_close(c);
		return;
	}
	rd->c = c;
	rd->d.surf = &rd->surf;
	rd->d.flush = rd_flush;
	rd->d.next_event = rd_event;
	rd->d.alive = 1;
	rd->d.draws_pointer = (h.flags & HELLO_POINTER) != 0;
	if (h.flags & HELLO_LZ) {
		rd->ztab = kmalloc(sizeof(uint32_t) * LZ_TABLE_WORDS);
		rd->zbuf = kmalloc(lz_bound(16 + 6 * PIX_MAX_RUNS));
		rd->lz = rd->ztab && rd->zbuf;
	}
	rd->d.pointer = rd_pointer;
	rd->d.commit = rd_commit;
	rd->d.name = "network";

	struct task *t = task_current();
	struct rfs *tdr = NULL;
	if (h.flags & HELLO_SHARE) {
		char desc[48];
		snprintf(desc, sizeof desc, "%s on \\\\%s", h.share, h.host);
		tdr = rfs_new(c, 'T', desc, h.host);
		if (tdr)
			t->tdrive = &tdr->drive;
	}
	char title[64];
	snprintf(title, sizeof title, "Program Manager on \\\\%s", net_hostname());
	desktop_run_on(&rd->d, title);
	conn_send(c, M_BYE, NULL, 0);
	t->tdrive = NULL;
	conn_close(c);
	if (tdr)
		rfs_free(tdr);
	surf_free(&rd->surf);
	kfree(rd->ztab);
	kfree(rd->zbuf);
	rd_commit(&rd->d);
	kfree(rd->outq);
	kfree(rd);
}

/* ---- server: NET USE file service ------------------------------------------------------ */

static void file_service(struct conn *c, const uint8_t *hello, size_t hlen)
{
	char root[VFS_PATH_MAX];
	strncpy0(root, (const char *)hello, MIN(hlen + 1, sizeof root));
	struct vstat st;
	if (vfs_stat(root, &st) || st.type != VFS_DIR) {
		conn_close(c);
		return;
	}
	for (;;) {
		struct netmsg *m = conn_wait(c, MSG(M_FREQ) | MSG(M_BYE), -1);
		if (!m || m->type == M_BYE) {
			kfree(m);
			break;
		}
		serve_file(c, root, m);
		kfree(m);
	}
	conn_close(c);
}

/* ---- clients ------------------------------------------------------------------------- */

static void make_hello(struct hello *h, int w, int ht, int flags)
{
	memset(h, 0, sizeof *h);
	h->w = w;
	h->h = ht;
	h->flags = (uint8_t)(flags | HELLO_SHARE);
	strncpy0(h->host, net_hostname(), sizeof h->host);
	strncpy0(h->share, task_current()->cwd, sizeof h->share);
}

static const char *strip_slashes(const char *host)
{
	while (*host == '\\' || *host == '/')
		host++;
	return host;
}

/* CTTY \\HOST: be a terminal for a shell over there. With a command, CPU
 * \\HOST runs just that over there and comes back. */
int remote_run(const char *host, const char *cmd)
{
	host = strip_slashes(host);
	int cols, rows, err = 0;
	con_size(&cols, &rows);
	if (rows > 200)
		rows = 25;
	struct hello h;
	make_hello(&h, cols, rows, cmd ? HELLO_CMD : 0);
	if (cmd)
		strncpy0(h.cmd, cmd, sizeof h.cmd);
	char share[VFS_PATH_MAX];
	strncpy0(share, task_current()->cwd, sizeof share);
	if (!cmd)
		kprintf("Connecting to \\\\%s...\n", host);
	struct conn *c = conn_connect(host, SVC_TTY, &h, sizeof h, &err);
	if (!c) {
		kprintf("Can't reach \\\\%s: %s\n", host, err == E_NOENT ? "no such machine" :
		    err == E_NOTSUP ? "no network adapter" : "no answer");
		return 1;
	}
	int done = 0;
	while (!done) {
		int busy = 0;
		struct netmsg *m;
		while ((m = conn_take(c, MSG(M_OUT) | MSG(M_CTRL) | MSG(M_FREQ) | MSG(M_BYE)))) {
			busy = 1;
			if (m->type == M_OUT) {
				char save = m->data[m->len];
				m->data[m->len] = 0;
				for (uint32_t i = 0; i < m->len; i++)
					con_putc((char)m->data[i]);
				m->data[m->len] = (uint8_t)save;
			} else if (m->type == M_CTRL && m->len >= 12) {
				int32_t v[3];
				memcpy(v, m->data, sizeof v);
				if (v[0] == C_CLEAR) con_clear();
				else if (v[0] == C_GOTO) con_gotoxy(v[1], v[2]);
				else if (v[0] == C_ATTR) con_attr(v[1], v[2]);
				else if (v[0] == C_CURSOR) con_cursor(v[1]);
			} else if (m->type == M_FREQ) {
				serve_file(c, share, m);
			} else if (m->type == M_BYE) {
				done = 1;
			}
			kfree(m);
		}
		int k;
		while ((k = con_pollkey()) >= 0) {
			busy = 1;
			if (k == 0x1D) {	/* Ctrl-]: hang up, like telnet */
				done = 1;
				break;
			}
			int32_t key = k;
			conn_send(c, M_KEY, &key, sizeof key);
			if (k == KEY_EOF)
				done = 1;
		}
		if (!conn_alive(c))
			done = 1;
		if (!busy)
			task_idle();
	}
	conn_send(c, M_BYE, NULL, 0);
	conn_close(c);
	con_attr(7, 0);
	con_cursor(0);
	if (!cmd)
		kprintf("\nConnection to \\\\%s closed.\n", host);
	return 0;
}

int remote_tty(const char *host)
{
	return remote_run(host, NULL);
}

/* WIN \\HOST: show that machine's desktop here. */
int remote_desktop(const char *host)
{
	host = strip_slashes(host);
	extern struct display *display_open(struct console *c, int *err);
	extern void display_close(struct display *d);
	extern struct console *console_unwrap(struct console *c);
	int err = 0;
	struct display *d = display_open(console_unwrap(task_current()->con), &err);
	if (!d) {
		kprintf(err == E_NOTSUP ? "WIN \\\\HOST needs a screen or a desktop window.\n" :
		    "Can't open a display here.\n");
		return 1;
	}
	if (d->attach)
		d->attach(d, NULL);
	struct hello h;
	make_hello(&h, d->surf->w, d->surf->h, (d->draws_pointer ? HELLO_POINTER : 0) | HELLO_LZ);
	char share[VFS_PATH_MAX];
	strncpy0(share, task_current()->cwd, sizeof share);

	fill(d->surf, 0, 0, d->surf->w, d->surf->h, col(0x202830));
	char msg[96];
	snprintf(msg, sizeof msg, "Connecting to \\\\%s...", host);
	text(d->surf, 20, 20, msg, col(0xFFFFFF));
	d->flush(d, (struct rect){ 0, 0, d->surf->w, d->surf->h });

	struct conn *c = conn_connect(host, SVC_DESK, &h, sizeof h, &err);
	if (!c) {
		display_close(d);
		kprintf("Can't reach \\\\%s: %s\n", host, err == E_NOENT ? "no such machine" :
		    err == E_NOTSUP ? "no network adapter" : "no answer");
		return 1;
	}
	int done = 0;
	while (!done && d->alive) {
		int busy = 0;
		struct netmsg *m;
		while ((m = conn_take(c, MSG(M_PIX) | MSG(M_PIXZ) | MSG(M_FREQ) | MSG(M_BYE)))) {
			busy = 1;
			const uint8_t *pd = m->data;
			size_t plen = m->len;
			uint8_t *unz = NULL;
			if (m->type == M_PIXZ && m->len > 4) {
				uint32_t rl;
				memcpy(&rl, m->data, 4);
				unz = rl <= 16 + 6 * PIX_MAX_RUNS ? kmalloc(rl) : NULL;
				long got = unz ? lz_decompress(m->data + 4, m->len - 4, unz, rl) : -1;
				if (got == (long)rl) {
					pd = unz;
					plen = rl;
					m->type = M_PIX;
				}
			}
			if (m->type == M_PIX && plen >= 16) {
				display_lock(d);
				int32_t r[4];
				memcpy(r, pd, sizeof r);
				const uint8_t *p = pd + 16;
				size_t nruns = (plen - 16) / 6;
				int x = 0, y = 0;
				struct surface *s = d->surf;
				for (size_t i = 0; i < nruns; i++) {
					uint16_t n;
					uint32_t rgb;
					memcpy(&n, p + i * 6, 2);
					memcpy(&rgb, p + i * 6 + 2, 4);
					uint32_t px = col(rgb);
					while (n--) {
						int sx = r[0] + x, sy = r[1] + y;
						if (sx >= 0 && sy >= 0 && sx < s->w && sy < s->h)
							s->px[(size_t)sy * (size_t)s->pitch + sx] = px;
						if (++x >= r[2]) {
							x = 0;
							y++;
						}
					}
				}
				d->flush(d, rect_isect((struct rect){ r[0], r[1], r[2], r[3] },
				    (struct rect){ 0, 0, d->surf->w, d->surf->h }));
				display_unlock(d);
			} else if (m->type == M_FREQ) {
				serve_file(c, share, m);
			} else if (m->type == M_BYE) {
				done = 1;
			}
			kfree(unz);
			kfree(m);
		}
		struct event e;
		while (d->next_event(d, &e)) {
			busy = 1;
			if (e.type == EV_KEY && e.key == KEY_F1 + 11) {	/* F12: hang up */
				done = 1;
				break;
			}
			int32_t v[5] = { e.type, e.x, e.y, e.buttons, e.key };
			conn_send(c, M_EV, v, sizeof v);
		}
		if (!conn_alive(c))
			done = 1;
		if (!busy)
			task_idle();
	}
	conn_send(c, M_BYE, NULL, 0);
	conn_close(c);
	display_close(d);
	return 0;
}

/* ---- NET USE --------------------------------------------------------------------------- */

static struct rfs *mapped[26];

int net_use(char letter, const char *unc)
{
	letter = (char)toupper(letter);
	if (letter < 'D' || letter > 'Z' || letter == 'T')
		return E_INVAL;
	const char *p = strip_slashes(unc);
	char host[32], path[VFS_PATH_MAX];
	size_t i = 0;
	while (p[i] && p[i] != '\\' && p[i] != '/' && i < sizeof host - 1) {
		host[i] = p[i];
		i++;
	}
	host[i] = 0;
	p += i;
	while (*p == '\\' || *p == '/')
		p++;
	/* \\BOX2\C\GAMES or \\BOX2\C:\GAMES */
	if (isalpha(p[0]) && (p[1] == ':' || p[1] == '\\' || p[1] == '/' || !p[1]))
		snprintf(path, sizeof path, "%c:\\%s", toupper(p[0]),
		    p[1] == ':' ? p + 2 + (p[2] == '\\' || p[2] == '/') : p + 1 + (p[1] != 0));
	else
		snprintf(path, sizeof path, "C:\\%s", p);
	for (char *q = path; *q; q++)
		if (*q == '/')
			*q = '\\';
	int err = 0;
	struct conn *c = conn_connect(host, SVC_FILE, path, strlen(path) + 1, &err);
	if (!c)
		return err;
	char desc[48];
	snprintf(desc, sizeof desc, "\\\\%s\\%s", host, path);
	struct rfs *r = rfs_new(c, letter, desc, host);
	if (!r) {
		conn_close(c);
		return E_NOMEM;
	}
	/* make sure the far side accepted the path */
	struct vstat st;
	if (r_stat(r, "", &st)) {
		conn_close(c);
		rfs_free(r);
		return E_NOENT;
	}
	if (mapped[letter - 'A'])
		net_unuse(letter);
	mapped[letter - 'A'] = r;
	vfs_mount(letter, &remote_ops, r, desc);
	return 0;
}

int net_unuse(char letter)
{
	letter = (char)toupper(letter);
	if (letter < 'A' || letter > 'Z' || !mapped[letter - 'A'])
		return E_BADDRIVE;
	struct rfs *r = mapped[letter - 'A'];
	mapped[letter - 'A'] = NULL;
	vfs_unmount(letter);
	conn_send(r->c, M_BYE, NULL, 0);
	conn_close(r->c);
	rfs_free(r);
	return 0;
}

void remote_init(void)
{
	net_service(SVC_TTY, tty_service);
	net_service(SVC_DESK, desk_service);
	net_service(SVC_FILE, file_service);
}
