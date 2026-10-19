/*
 * vfs.c - drive letters, path resolution, DOS devices, open files, and
 * the glue that puts bdosfs volumes on drive letters.
 */
#include "kernel.h"
#include "vfs.h"
#include "drv/drv.h"
#include "../fs/bdosfs.h"

static struct vfs_drive drives[26];
static struct chardev *devices[16];
static int ndevices;

const char *vfs_strerror(int err)
{
	switch (err) {
	case E_BADDRIVE: return "Invalid drive specification";
	case E_NOTSUP: return "Not supported on this drive";
	case E_XDEV: return "Cannot move between drives (use COPY)";
	}
	return bfs_strerror(err);
}

/* ---- time ----------------------------------------------------------------- */

static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

uint64_t rtc_epoch(void)
{
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	return (uint64_t)(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
}

void epoch_to_date(uint64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	int64_t z = (int64_t)(t / 86400) + 719468;
	uint64_t secs = t % 86400;
	int64_t era = z / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*mo = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yoe + era * 400 + (*mo <= 2));
	*h = (int)(secs / 3600);
	*mi = (int)(secs / 60 % 60);
	*s = (int)(secs % 60);
}

uint64_t bfs_now(void)
{
	return rtc_epoch();
}

/* ---- drives and devices ----------------------------------------------------- */

int vfs_mount(char letter, const struct vfs_ops *ops, void *ctx, const char *desc)
{
	letter = (char)toupper(letter);
	if (letter < 'A' || letter > 'Z')
		return E_BADDRIVE;
	struct vfs_drive *d = &drives[letter - 'A'];
	d->letter = letter;
	d->ops = ops;
	d->ctx = ctx;
	strncpy0(d->desc, desc, sizeof d->desc);
	return 0;
}

int vfs_unmount(char letter)
{
	letter = (char)toupper(letter);
	if (letter < 'A' || letter > 'Z' || !drives[letter - 'A'].ops)
		return E_BADDRIVE;
	memset(&drives[letter - 'A'], 0, sizeof drives[0]);
	return 0;
}

struct vfs_drive *vfs_drive(char letter)
{
	letter = (char)toupper(letter);
	if (letter < 'A' || letter > 'Z' || !drives[letter - 'A'].ops)
		return NULL;
	return &drives[letter - 'A'];
}

void vfs_drives(void (*cb)(struct vfs_drive *d, void *arg), void *arg)
{
	for (int i = 0; i < 26; i++)
		if (drives[i].ops)
			cb(&drives[i], arg);
}

void dev_register(struct chardev *d)
{
	if (ndevices < (int)NELEM(devices))
		devices[ndevices++] = d;
}

struct chardev *dev_find(const char *name)
{
	char base[16];
	size_t n = 0;
	/* "CON", "con:", "CON.TXT" all mean the console, as in DOS */
	for (; name[n] && name[n] != ':' && name[n] != '.' && n < sizeof base - 1; n++)
		base[n] = name[n];
	base[n] = 0;
	for (int i = 0; i < ndevices; i++)
		if (!strcasecmp(devices[i]->name, base))
			return devices[i];
	return NULL;
}

/* ---- paths ---------------------------------------------------------------- */

static int is_sep(char c) { return c == '\\' || c == '/'; }

int vfs_resolve(const char *cwd, const char *in, char *out, size_t n)
{
	char drive = (char)toupper(cwd[0]);
	const char *rest = in;
	char comps[32][VFS_NAME_MAX];
	int nc = 0;

	if (isalpha(in[0]) && in[1] == ':') {
		drive = (char)toupper(in[0]);
		rest = in + 2;
	}
	if (!is_sep(*rest) && drive == toupper(cwd[0])) {
		/* relative to cwd: seed with its components */
		const char *p = cwd + 2;
		while (*p) {
			while (is_sep(*p))
				p++;
			if (!*p)
				break;
			size_t len = 0;
			while (*p && !is_sep(*p)) {
				if (len + 1 < VFS_NAME_MAX)
					comps[nc][len++] = *p;
				p++;
			}
			comps[nc][len] = 0;
			if (nc < 31)
				nc++;
		}
	}
	const char *p = rest;
	while (*p) {
		while (is_sep(*p))
			p++;
		if (!*p)
			break;
		char part[VFS_NAME_MAX];
		size_t len = 0;
		while (*p && !is_sep(*p)) {
			if (len + 1 >= VFS_NAME_MAX)
				return E_NAMETOOLONG;
			part[len++] = *p++;
		}
		part[len] = 0;
		if (!strcmp(part, "."))
			continue;
		if (!strcmp(part, "..")) {
			if (nc)
				nc--;
			continue;
		}
		if (!strcmp(part, "...")) {		/* 4DOS: two levels up */
			nc = nc >= 2 ? nc - 2 : 0;
			continue;
		}
		if (nc >= 31)
			return E_NAMETOOLONG;
		strcpy(comps[nc++], part);
	}

	size_t o = 0;
	if (n < 4)
		return E_NAMETOOLONG;
	out[o++] = drive;
	out[o++] = ':';
	out[o++] = '\\';
	for (int i = 0; i < nc; i++) {
		size_t l = strlen(comps[i]);
		if (o + l + 2 > n)
			return E_NAMETOOLONG;
		memcpy(out + o, comps[i], l);
		o += l;
		if (i + 1 < nc)
			out[o++] = '\\';
	}
	out[o] = 0;
	return 0;
}

const char *vfs_basename(const char *abs)
{
	const char *b = abs;
	for (const char *p = abs; *p; p++)
		if (is_sep(*p) || *p == ':')
			b = p + 1;
	return b;
}

void vfs_dirname(const char *abs, char *out, size_t n)
{
	strncpy0(out, abs, n);
	char *b = (char *)vfs_basename(out);
	if (b > out + 3)
		b[-1] = 0;	/* drop "\name" */
	else
		out[3] = 0;	/* "C:\" */
}

int vfs_join(const char *dir, const char *name, char *out, size_t n)
{
	size_t dl = strlen(dir);
	int need_sep = dl && !is_sep(dir[dl - 1]);
	if (dl + need_sep + strlen(name) + 1 > n)
		return E_NAMETOOLONG;
	strcpy(out, dir);
	if (need_sep)
		strcat0(out, "\\", n);
	strcat0(out, name, n);
	return 0;
}

/* "C:\A\B" -> drive C, inner path "A/B" */
static int split(const char *abs, struct vfs_drive **d, char *inner, size_t n)
{
	if (!isalpha(abs[0]) || abs[1] != ':')
		return E_INVAL;
	*d = vfs_drive(abs[0]);
	if (!*d)
		return E_BADDRIVE;
	const char *p = abs + 2;
	while (is_sep(*p))
		p++;
	size_t o = 0;
	for (; *p && o + 1 < n; p++)
		inner[o++] = is_sep(*p) ? '/' : *p;
	while (o && inner[o - 1] == '/')
		o--;
	inner[o] = 0;
	return 0;
}

#define SPLIT(abs) \
	struct vfs_drive *d; \
	char inner[VFS_PATH_MAX]; \
	int r_ = split(abs, &d, inner, sizeof inner); \
	if (r_) \
		return r_;

int vfs_stat(const char *abs, struct vstat *st)
{
	struct chardev *dev = dev_find(vfs_basename(abs));
	if (dev && *vfs_basename(abs)) {
		memset(st, 0, sizeof *st);
		st->type = VFS_DEV;
		strncpy0(st->name, dev->name, sizeof st->name);
		return 0;
	}
	SPLIT(abs);
	return d->ops->stat(d->ctx, inner, st);
}

int vfs_canon(const char *abs, char *out, size_t n)
{
	/* walk component by component, replacing each with its stored spelling */
	char cur[VFS_PATH_MAX];
	strncpy0(cur, abs, 4);
	cur[0] = (char)toupper(cur[0]);
	const char *p = abs + 2;
	while (*p) {
		while (is_sep(*p))
			p++;
		if (!*p)
			break;
		char part[VFS_NAME_MAX];
		size_t l = 0;
		while (*p && !is_sep(*p) && l + 1 < sizeof part)
			part[l++] = *p++;
		part[l] = 0;
		char next[VFS_PATH_MAX];
		if (vfs_join(cur, part, next, sizeof next))
			return E_NAMETOOLONG;
		struct vstat st;
		int r = vfs_stat(next, &st);
		if (r)
			return r;
		vfs_join(cur, st.name, cur, sizeof cur);
	}
	strncpy0(out, cur, n);
	return 0;
}

int vfs_list(const char *absdir, int (*cb)(void *arg, struct vstat *st), void *arg)
{
	SPLIT(absdir);
	return d->ops->list(d->ctx, inner, cb, arg);
}

int vfs_mkdir(const char *abs)
{
	SPLIT(abs);
	return d->ops->create(d->ctx, inner, VFS_DIR);
}

int vfs_rmdir(const char *abs)
{
	struct vstat st;
	int r = vfs_stat(abs, &st);
	if (r)
		return r;
	if (st.type != VFS_DIR)
		return E_NOTDIR;
	SPLIT(abs);
	if (!*inner)
		return E_ACCES;
	return d->ops->remove(d->ctx, inner);
}

int vfs_delete(const char *abs)
{
	struct vstat st;
	int r = vfs_stat(abs, &st);
	if (r)
		return r;
	if (st.type == VFS_DIR)
		return E_ISDIR;
	if (st.type == VFS_DEV)
		return E_ACCES;
	SPLIT(abs);
	return d->ops->remove(d->ctx, inner);
}

int vfs_rename(const char *from, const char *to)
{
	if (toupper(from[0]) != toupper(to[0]))
		return E_XDEV;
	struct vfs_drive *d;
	char a[VFS_PATH_MAX], b[VFS_PATH_MAX];
	int r = split(from, &d, a, sizeof a);
	if (!r)
		r = split(to, &d, b, sizeof b);
	if (r)
		return r;
	return d->ops->rename(d->ctx, a, b);
}

int vfs_setattr(const char *abs, uint8_t attr)
{
	SPLIT(abs);
	return d->ops->setattr ? d->ops->setattr(d->ctx, inner, attr) : E_NOTSUP;
}

int vfs_xattr_get(const char *abs, const char *key, char *val, size_t n)
{
	SPLIT(abs);
	return d->ops->xattr_get ? d->ops->xattr_get(d->ctx, inner, key, val, n) : E_NOTSUP;
}

int vfs_xattr_set(const char *abs, const char *key, const char *val)
{
	SPLIT(abs);
	return d->ops->xattr_set ? d->ops->xattr_set(d->ctx, inner, key, val) : E_NOTSUP;
}

int vfs_info(char letter, char *label, size_t n, uint64_t *free, uint64_t *total)
{
	struct vfs_drive *d = vfs_drive(letter);
	if (!d)
		return E_BADDRIVE;
	return d->ops->info(d->ctx, label, n, free, total);
}

int vfs_check(char letter, int fix, void (*out)(const char *))
{
	struct vfs_drive *d = vfs_drive(letter);
	if (!d)
		return E_BADDRIVE;
	return d->ops->check ? d->ops->check(d->ctx, fix, out) : E_NOTSUP;
}

int vfs_set_label(char letter, const char *label)
{
	struct vfs_drive *d = vfs_drive(letter);
	if (!d)
		return E_BADDRIVE;
	return d->ops->set_label ? d->ops->set_label(d->ctx, label) : E_NOTSUP;
}

/* ---- open files ------------------------------------------------------------ */

struct vfile {
	struct vfs_drive *d;
	char path[VFS_PATH_MAX];
	uint64_t pos;
	int flags;
	struct chardev *dev;
	int dev_done;
};

struct vfile *vfs_open(const char *abs, int flags, int *err)
{
	int r = 0;
	struct vfile *f = kzalloc(sizeof *f);
	if (!f) {
		*err = E_NOMEM;
		return NULL;
	}
	f->flags = flags;
	struct chardev *dev = dev_find(vfs_basename(abs));
	if (dev && *vfs_basename(abs)) {
		f->dev = dev;
		return f;
	}
	r = split(abs, &f->d, f->path, sizeof f->path);
	if (r)
		goto fail;
	struct vstat st;
	r = f->d->ops->stat(f->d->ctx, f->path, &st);
	if (r == E_NOENT && (flags & O_CREATE)) {
		r = f->d->ops->create(f->d->ctx, f->path, VFS_FILE);
		st.size = 0;
		st.type = VFS_FILE;
		st.attr = 0;
	}
	if (r)
		goto fail;
	if (st.type == VFS_DIR) {
		r = E_ISDIR;
		goto fail;
	}
	if ((flags & (O_WRITE | O_TRUNC)) && (st.attr & A_RO)) {
		r = E_ACCES;
		goto fail;
	}
	if ((flags & O_TRUNC) && st.size) {
		r = f->d->ops->truncate(f->d->ctx, f->path, 0);
		if (r)
			goto fail;
	}
	if (flags & O_APPEND)
		f->pos = (flags & O_TRUNC) ? 0 : st.size;
	return f;
fail:
	kfree(f);
	*err = r;
	return NULL;
}

long vfs_read(struct vfile *f, void *buf, size_t n)
{
	if (f->dev) {
		if (f->dev_done)
			return 0;
		long r = f->dev->read ? f->dev->read(f->dev, buf, n) : 0;
		if (f->dev->once)
			f->dev_done = 1;
		return r;
	}
	long r = f->d->ops->read(f->d->ctx, f->path, f->pos, buf, n);
	if (r > 0)
		f->pos += (uint64_t)r;
	return r;
}

long vfs_write(struct vfile *f, const void *buf, size_t n)
{
	if (f->dev)
		return f->dev->write ? f->dev->write(f->dev, buf, n) : (long)n;
	if (!(f->flags & (O_WRITE | O_APPEND)))
		return E_ACCES;
	long r = f->d->ops->write(f->d->ctx, f->path, f->pos, buf, n);
	if (r > 0)
		f->pos += (uint64_t)r;
	return r;
}

int vfs_seek(struct vfile *f, uint64_t pos)
{
	f->pos = pos;
	return 0;
}

uint64_t vfs_tell(struct vfile *f) { return f->pos; }
int vfs_isdev(struct vfile *f) { return f->dev != NULL; }

uint64_t vfs_size(struct vfile *f)
{
	if (f->dev)
		return 0;
	struct vstat st;
	if (f->d->ops->stat(f->d->ctx, f->path, &st))
		return 0;
	return st.size;
}

void vfs_close(struct vfile *f)
{
	kfree(f);
}

char *vfs_slurp(const char *abs, size_t *len, int *err)
{
	struct vfile *f = vfs_open(abs, O_READ, err);
	if (!f)
		return NULL;
	uint64_t size = vfs_size(f);
	char *buf = kmalloc(size + 1);
	if (!buf) {
		vfs_close(f);
		*err = E_NOMEM;
		return NULL;
	}
	long got = vfs_read(f, buf, size);
	vfs_close(f);
	if (got < 0) {
		kfree(buf);
		*err = (int)got;
		return NULL;
	}
	buf[got] = 0;
	if (len)
		*len = (size_t)got;
	return buf;
}

int vfs_spew(const char *abs, const void *data, size_t len)
{
	int err = 0;
	struct vfile *f = vfs_open(abs, O_WRITE | O_CREATE | O_TRUNC, &err);
	if (!f)
		return err;
	long w = vfs_write(f, data, len);
	vfs_close(f);
	return w < 0 ? (int)w : 0;
}

/* ---- bdosfs volumes ---------------------------------------------------------- */

struct bvol {
	struct bfs *fs;
	struct bfs_dev dev;
	struct blkdev *blk;
	uint64_t start;		/* first sector of the partition */
};

static int bd_read(void *ctx, uint64_t blk, uint32_t n, void *buf)
{
	struct bvol *v = ctx;
	return v->blk->read(v->blk, v->start + blk * 8, n * 8, buf);
}

static int bd_write(void *ctx, uint64_t blk, uint32_t n, const void *buf)
{
	struct bvol *v = ctx;
	return v->blk->write(v->blk, v->start + blk * 8, n * 8, buf);
}

static int bd_flush(void *ctx)
{
	struct bvol *v = ctx;
	return v->blk->flush ? v->blk->flush(v->blk) : 0;
}

/* inner path -> inode (and parent + last name, for create/remove) */
static int bv_walk(struct bfs *fs, const char *path, uint32_t *ino)
{
	uint32_t cur = BFS_ROOT;
	const char *p = path;
	while (*p) {
		char part[VFS_NAME_MAX];
		size_t l = 0;
		while (*p && *p != '/' && l + 1 < sizeof part)
			part[l++] = *p++;
		part[l] = 0;
		while (*p == '/')
			p++;
		int r = bfs_lookup(fs, cur, part, &cur);
		if (r)
			return r;
	}
	*ino = cur;
	return 0;
}

static int bv_parent(struct bfs *fs, const char *path, uint32_t *dir, const char **name)
{
	const char *slash = strrchr(path, '/');
	char parent[VFS_PATH_MAX];
	if (slash) {
		size_t l = (size_t)(slash - path);
		memcpy(parent, path, l);
		parent[l] = 0;
		*name = slash + 1;
	} else {
		parent[0] = 0;
		*name = path;
	}
	if (!**name)
		return E_INVAL;
	int r = bv_walk(fs, parent, dir);
	if (r)
		return r;
	struct bfs_stat st;
	if ((r = bfs_stat(fs, *dir, &st)))
		return r;
	return st.type == BFS_DIR ? 0 : E_NOTDIR;
}

static void to_vstat(const char *name, struct bfs_stat *bs, struct vstat *st)
{
	memset(st, 0, sizeof *st);
	st->type = bs->type == BFS_DIR ? VFS_DIR : VFS_FILE;
	st->attr = bs->attr;
	st->size = bs->size;
	st->mtime = bs->mtime;
	strncpy0(st->short_name, bs->short_name, sizeof st->short_name);
	strncpy0(st->name, name, sizeof st->name);
}

static int bv_stat(void *ctx, const char *path, struct vstat *st)
{
	struct bvol *v = ctx;
	uint32_t dir, ino;
	struct bfs_stat bs;
	if (!*path) {
		int r = bfs_stat(v->fs, BFS_ROOT, &bs);
		if (r)
			return r;
		to_vstat("", &bs, st);
		return 0;
	}
	const char *name;
	int r = bv_parent(v->fs, path, &dir, &name);
	if (r)
		return r;
	char canon[BFS_NAME_MAX + 1];
	if ((r = bfs_lookup_name(v->fs, dir, name, &ino, canon)) || (r = bfs_stat(v->fs, ino, &bs)))
		return r;
	to_vstat(canon, &bs, st);
	/* short name lives in the directory entry; fetch it the slow way */
	uint64_t cookie = 0;
	struct bfs_listent *e = kmalloc(sizeof *e);
	if (e) {
		while (bfs_readdir(v->fs, dir, &cookie, e) == 1)
			if (e->st.ino == ino) {
				strncpy0(st->short_name, e->st.short_name, sizeof st->short_name);
				break;
			}
		kfree(e);
	}
	return 0;
}

static int bv_list(void *ctx, const char *path, int (*cb)(void *, struct vstat *), void *arg)
{
	struct bvol *v = ctx;
	uint32_t dir;
	int r = bv_walk(v->fs, path, &dir);
	if (r)
		return r;
	struct bfs_listent *e = kmalloc(sizeof *e);
	struct vstat *st = kmalloc(sizeof *st);
	if (!e || !st) {
		kfree(e);
		kfree(st);
		return E_NOMEM;
	}
	uint64_t cookie = 0;
	while ((r = bfs_readdir(v->fs, dir, &cookie, e)) == 1) {
		to_vstat(e->name, &e->st, st);
		if (cb(arg, st))
			break;
	}
	kfree(e);
	kfree(st);
	return r < 0 ? r : 0;
}

static long bv_read(void *ctx, const char *path, uint64_t off, void *buf, size_t n)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	return r ? r : bfs_read(v->fs, ino, off, buf, n);
}

static long bv_write(void *ctx, const char *path, uint64_t off, const void *buf, size_t n)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	if (r)
		return r;
	return bfs_write(v->fs, ino, off, buf, n);
}

static int bv_create(void *ctx, const char *path, int type)
{
	struct bvol *v = ctx;
	uint32_t dir;
	const char *name;
	int r = bv_parent(v->fs, path, &dir, &name);
	if (r)
		return r;
	return bfs_create(v->fs, dir, name, type == VFS_DIR ? BFS_DIR : BFS_FILE, NULL);
}

static int bv_truncate(void *ctx, const char *path, uint64_t size)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	return r ? r : bfs_truncate(v->fs, ino, size);
}

static int bv_remove(void *ctx, const char *path)
{
	struct bvol *v = ctx;
	uint32_t dir;
	const char *name;
	int r = bv_parent(v->fs, path, &dir, &name);
	return r ? r : bfs_unlink(v->fs, dir, name);
}

static int bv_rename(void *ctx, const char *from, const char *to)
{
	struct bvol *v = ctx;
	uint32_t sd, dd;
	const char *sn, *dn;
	int r = bv_parent(v->fs, from, &sd, &sn);
	if (!r)
		r = bv_parent(v->fs, to, &dd, &dn);
	return r ? r : bfs_rename(v->fs, sd, sn, dd, dn);
}

static int bv_setattr(void *ctx, const char *path, uint8_t attr)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	return r ? r : bfs_setattr(v->fs, ino, attr);
}

static int bv_xget(void *ctx, const char *path, const char *key, char *val, size_t n)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	return r ? r : bfs_xattr_get(v->fs, ino, key, val, n);
}

static int bv_xset(void *ctx, const char *path, const char *key, const char *val)
{
	struct bvol *v = ctx;
	uint32_t ino;
	int r = bv_walk(v->fs, path, &ino);
	return r ? r : bfs_xattr_set(v->fs, ino, key, val);
}

static int bv_info(void *ctx, char *label, size_t n, uint64_t *free, uint64_t *total)
{
	struct bvol *v = ctx;
	strncpy0(label, bfs_label(v->fs), n);
	*free = bfs_free_bytes(v->fs);
	*total = bfs_total_bytes(v->fs);
	return 0;
}

static int bv_check(void *ctx, int fix, void (*out)(const char *))
{
	struct bvol *v = ctx;
	return bfs_check(v->fs, fix, out);
}

static int bv_label(void *ctx, const char *label)
{
	struct bvol *v = ctx;
	return bfs_set_label(v->fs, label);
}

static const struct vfs_ops bdosfs_ops = {
	.stat = bv_stat,
	.list = bv_list,
	.read = bv_read,
	.write = bv_write,
	.create = bv_create,
	.truncate = bv_truncate,
	.remove = bv_remove,
	.rename = bv_rename,
	.setattr = bv_setattr,
	.xattr_get = bv_xget,
	.xattr_set = bv_xset,
	.info = bv_info,
	.check = bv_check,
	.set_label = bv_label,
};

static int mount_bdosfs(char letter, struct blkdev *b, uint64_t start, uint64_t sectors,
    const char *what)
{
	struct bvol *v = kzalloc(sizeof *v);
	if (!v)
		return E_NOMEM;
	v->blk = b;
	v->start = start;
	v->dev.read = bd_read;
	v->dev.write = bd_write;
	v->dev.flush = bd_flush;
	v->dev.ctx = v;
	v->dev.nblocks = sectors / 8;
	int r = bfs_mount(&v->dev, &v->fs);
	if (r) {
		kprintf("%s: %s\n", what, bfs_strerror(r));
		kfree(v);
		return r;
	}
	char desc[48];
	snprintf(desc, sizeof desc, "bdosfs on %s", what);
	vfs_mount(letter, &bdosfs_ops, v, desc);
	if (bfs_replayed(v->fs))
		kprintf("%c: journal replayed after an unclean shutdown\n", letter);
	return 0;
}

/* ---- standard devices ------------------------------------------------------------ */

static long nul_read(struct chardev *d, void *buf, size_t n)
{
	(void)d; (void)buf; (void)n;
	return 0;
}

static long nul_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d; (void)buf;
	return (long)n;
}

static long con_dev_read(struct chardev *d, void *buf, size_t n)
{
	(void)d;
	if (!n)
		return 0;
	int k = con_getkey();
	((char *)buf)[0] = (char)(k == '\n' ? '\n' : k);
	return 1;
}

static long con_dev_write(struct chardev *d, const void *buf, size_t n)
{
	(void)d;
	const char *p = buf;
	for (size_t i = 0; i < n; i++)
		con_putc(p[i]);
	return (long)n;
}

static struct chardev nul_dev = { "NUL", nul_read, nul_write, NULL, 0 };
static struct chardev con_dev = { "CON", con_dev_read, con_dev_write, NULL, 0 };

/* ---- boot-time discovery ------------------------------------------------------------ */

void storage_init(void)
{
	dev_register(&nul_dev);
	dev_register(&con_dev);

	pci_init();
	ahci_init();

	char next = 'C';
	uint8_t *mbr = kmalloc(512);
	for (int i = 0; i < blk_count() && mbr; i++) {
		struct blkdev *b = blk_get(i);
		if (b->read(b, 0, 1, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
			continue;
		for (int p = 0; p < 4; p++) {
			uint8_t *e = mbr + 446 + p * 16;
			uint32_t start, count;
			memcpy(&start, e + 8, 4);
			memcpy(&count, e + 12, 4);
			if (e[4] != 0xBD || !count)
				continue;
			char what[32];
			snprintf(what, sizeof what, "%s partition %d", b->name, p + 1);
			if (!mount_bdosfs(next, b, start, count, what))
				next++;
		}
	}
	kfree(mbr);

	if (next == 'C' && bootinfo->rootfs_base) {
		struct blkdev *rd = ramdisk_create("ram0", (void *)bootinfo->rootfs_base,
		    bootinfo->rootfs_size);
		if (rd && !mount_bdosfs('C', rd, 0, rd->sectors, "RAM disk (from ROOTFS.IMG)"))
			next++;
	}
	if (next == 'C')
		kprintf("No bdosfs disk found. Drive C: is not available.\n");
}
