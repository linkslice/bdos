/*
 * cdrom.c - CD drives: read-only ISO 9660 (with Joliet's long names) on a
 * changer of disc images. See cdrom.h.
 *
 * The disc is read 32 KB at a time and the last megabyte kept, so a game
 * reading a file a sector at a time, or a directory being searched, doesn't
 * cost a trip to the disk (or across the network to bterm) for each.
 */
#include "kernel.h"
#include "vfs.h"
#include "cdrom.h"
#include "settings.h"

#define CHUNK_SECTORS	16			/* 32 KB */
#define CHUNK_BYTES	(CHUNK_SECTORS * CD_SECTOR)
#define CACHE_CHUNKS	32			/* 1 MB */
#define MAX_CHANGERS	16

struct chunk {
	uint64_t first;				/* sector number of its first sector */
	uint64_t used;				/* when last used (0: empty) */
	uint32_t len;				/* bytes of it that are on the disc */
	uint8_t *data;
};

struct changer {
	struct vfs_drive drive;
	char letter;
	int session;				/* in its session's drives, not the machine's */
	cd_open_fn open;
	void *ctx;
	char source[48];
	int ndiscs, cur;
	char names[CD_MAX_DISCS][64];
	uint32_t gen;
	/* the disc in it */
	struct cdsrc src;
	int have_src;
	int joliet;
	uint32_t root_lba, root_size;
	uint64_t vol_bytes;
	char label[33];
	struct chunk cache[CACHE_CHUNKS];
	uint64_t clock;
};

static struct changer *all[MAX_CHANGERS];

/* ---- the disc's sectors, through the cache ---- */

static void cache_drop(struct changer *c)
{
	for (int i = 0; i < CACHE_CHUNKS; i++)
		c->cache[i].used = 0;
}

static const uint8_t *chunk_get(struct changer *c, uint64_t sector)
{
	uint64_t first = sector / CHUNK_SECTORS * CHUNK_SECTORS;
	struct chunk *victim = &c->cache[0];
	for (int i = 0; i < CACHE_CHUNKS; i++) {
		struct chunk *k = &c->cache[i];
		if (k->used && k->first == first) {
			k->used = ++c->clock;
			return k->data;
		}
		if (k->used < victim->used)
			victim = k;
	}
	if (!victim->data && !(victim->data = kmalloc(CHUNK_BYTES)))
		return NULL;
	long got = c->src.read(&c->src, first * CD_SECTOR, victim->data, CHUNK_BYTES);
	if (got <= 0)
		return NULL;
	if (got < CHUNK_BYTES)
		memset(victim->data + got, 0, (size_t)(CHUNK_BYTES - got));
	victim->first = first;
	victim->len = (uint32_t)got;
	victim->used = ++c->clock;
	return victim->data;
}

/* bytes from the disc, at a byte offset */
static int disc_read(struct changer *c, uint64_t off, void *buf, size_t n)
{
	uint8_t *out = buf;
	while (n) {
		uint64_t sector = off / CD_SECTOR;
		const uint8_t *d = chunk_get(c, sector);
		if (!d)
			return E_IO;
		uint64_t in = off - sector / CHUNK_SECTORS * CHUNK_SECTORS * CD_SECTOR;
		size_t take = (size_t)MIN((uint64_t)n, CHUNK_BYTES - in);
		memcpy(out, d + in, take);
		out += take;
		off += take;
		n -= take;
	}
	return 0;
}

int changer_read_sectors(struct changer *c, uint32_t lba, uint32_t n, void *buf)
{
	if (!c->have_src)
		return E_NOTREADY;
	return disc_read(c, (uint64_t)lba * CD_SECTOR, buf, (size_t)n * CD_SECTOR);
}

/* ---- ISO 9660 ---- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* days from 1970-01-01 to a date (proleptic Gregorian) */
static int64_t days_from_civil(int y, int m, int d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* a directory record's name, as we show it: Joliet's UCS-2, or ISO's without ";1" */
static void rec_name(struct changer *c, const uint8_t *r, char *out, size_t n)
{
	int len = r[32];
	const uint8_t *p = r + 33;
	size_t k = 0;
	if (c->joliet) {
		for (int i = 0; i + 1 < len && k + 1 < n; i += 2) {
			uint16_t u = (uint16_t)(p[i] << 8 | p[i + 1]);
			out[k++] = u < 0x80 && u >= 0x20 ? (char)u : '_';
		}
	} else {
		for (int i = 0; i < len && k + 1 < n; i++)
			out[k++] = (char)p[i];
	}
	out[k] = 0;
	char *semi = strchr(out, ';');
	if (semi)
		*semi = 0;
	size_t l = strlen(out);
	if (l > 1 && out[l - 1] == '.')
		out[l - 1] = 0;		/* "README." -> "README" */
}

/* the 8.3 name DOS programs see */
static void short_of(const char *name, char *out)
{
	const char *dot = strrchr(name, '.');
	size_t blen = dot ? (size_t)(dot - name) : strlen(name), elen = dot ? strlen(dot + 1) : 0;
	int fits = blen && blen <= 8 && elen <= 3;
	for (const char *p = name; *p && fits; p++)
		if (*p == ' ' || (*p == '.' && p != dot) || (unsigned char)*p > 0x7E)
			fits = 0;
	size_t k = 0;
	if (fits) {
		for (const char *p = name; *p; p++)
			out[k++] = (char)toupper((unsigned char)*p);
		out[k] = 0;
		return;
	}
	for (size_t i = 0; i < blen && k < 6; i++)
		if (isalnum((unsigned char)name[i]) || name[i] == '_' || name[i] == '-')
			out[k++] = (char)toupper((unsigned char)name[i]);
	out[k++] = '~';
	out[k++] = '1';
	if (elen) {
		out[k++] = '.';
		for (size_t i = 0; i < elen && i < 3; i++)
			out[k++] = (char)toupper((unsigned char)dot[1 + i]);
	}
	out[k] = 0;
}

/* this machine's clock.tz, in seconds: "-6", "+5:30" */
static int64_t local_offset(void)
{
	const char *tz = setting("clock.tz");
	int neg = *tz == '-';
	if (*tz == '-' || *tz == '+')
		tz++;
	int64_t h = strtol(tz, NULL, 10), m = 0;
	const char *colon = strchr(tz, ':');
	if (colon)
		m = strtol(colon + 1, NULL, 10);
	int64_t secs = h * 3600 + m * 60;
	return neg ? -secs : secs;
}

static void rec_stat(struct changer *c, const uint8_t *r, struct vstat *st)
{
	memset(st, 0, sizeof *st);
	rec_name(c, r, st->name, sizeof st->name);
	short_of(st->name, st->short_name);
	st->type = (r[25] & 2) ? VFS_DIR : VFS_FILE;
	st->attr = A_RO | ((r[25] & 2) ? A_DIR : 0) | ((r[25] & 1) ? A_HIDDEN : 0);
	st->size = st->type == VFS_DIR ? 0 : le32(r + 10);
	/* recorded in its maker's local time, with that zone's offset (15-minute
	 * steps); shown in this machine's (clock.tz), as every file here is */
	const uint8_t *t = r + 18;
	int64_t days = days_from_civil(1900 + t[0], t[1] ? t[1] : 1, t[2] ? t[2] : 1);
	int64_t utc = days * 86400 + t[3] * 3600 + t[4] * 60 + t[5] - (int64_t)(int8_t)t[6] * 900;
	st->mtime = (uint64_t)(utc + local_offset());
}

/* every record in a directory: fn returns nonzero to stop */
static int dir_scan(struct changer *c, uint32_t lba, uint32_t size,
    int (*fn)(struct changer *c, const uint8_t *rec, void *arg), void *arg)
{
	uint8_t sec[CD_SECTOR];
	for (uint32_t off = 0; off < size; off += CD_SECTOR) {
		if (disc_read(c, (uint64_t)lba * CD_SECTOR + off, sec, CD_SECTOR))
			return E_IO;
		for (uint32_t i = 0; i < CD_SECTOR; ) {
			uint8_t len = sec[i];
			if (!len || i + len > CD_SECTOR || len < 34)
				break;	/* the rest of this sector is padding */
			const uint8_t *r = sec + i;
			int dot = r[32] == 1 && (r[33] == 0 || r[33] == 1);
			if (!dot) {
				int stop = fn(c, r, arg);
				if (stop)
					return stop;
			}
			i += len;
		}
	}
	return 0;
}

struct find {
	const char *want;
	uint32_t lba, size;
	int dir;
	uint8_t rec[256];
};

static int find_fn(struct changer *c, const uint8_t *r, void *arg)
{
	struct find *f = arg;
	char name[VFS_NAME_MAX], sn[13];
	rec_name(c, r, name, sizeof name);
	short_of(name, sn);
	if (strcasecmp(name, f->want) && strcasecmp(sn, f->want))
		return 0;
	f->lba = le32(r + 2);
	f->size = le32(r + 10);
	f->dir = (r[25] & 2) != 0;
	memcpy(f->rec, r, r[0]);
	return 1;
}

/* a path ("GAME/DATA/FILE.DAT", '/' or '\') to its record; 0 for the root */
static int lookup(struct changer *c, const char *path, struct find *f)
{
	if (!c->have_src)
		return E_NOTREADY;
	memset(f, 0, sizeof *f);
	f->lba = c->root_lba;
	f->size = c->root_size;
	f->dir = 1;
	char comp[VFS_NAME_MAX];
	const char *p = path;
	while (*p) {
		while (*p == '/' || *p == '\\')
			p++;
		if (!*p)
			break;
		size_t k = 0;
		while (*p && *p != '/' && *p != '\\' && k + 1 < sizeof comp)
			comp[k++] = *p++;
		comp[k] = 0;
		if (!f->dir)
			return E_NOTDIR;
		f->want = comp;
		int r = dir_scan(c, f->lba, f->size, find_fn, f);
		if (r < 0)
			return r;
		if (r == 0)
			return E_NOENT;
	}
	return 0;
}

/* the disc just put in: find its root, Joliet's if it has one */
static int iso_open(struct changer *c)
{
	uint8_t vd[CD_SECTOR];
	int found = 0;
	c->joliet = 0;
	for (uint32_t s = 16; s < 64; s++) {
		if (disc_read(c, (uint64_t)s * CD_SECTOR, vd, CD_SECTOR))
			return E_IO;
		if (memcmp(vd + 1, "CD001", 5))
			return E_CORRUPT;
		if (vd[0] == 255)
			break;
		if (vd[0] == 1 && !found) {
			c->root_lba = le32(vd + 156 + 2);
			c->root_size = le32(vd + 156 + 10);
			c->vol_bytes = (uint64_t)le32(vd + 80) * CD_SECTOR;
			memcpy(c->label, vd + 40, 32);
			c->label[32] = 0;
			for (int i = 31; i >= 0 && c->label[i] == ' '; i--)
				c->label[i] = 0;
			found = 1;
		} else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' &&
		    (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
			c->root_lba = le32(vd + 156 + 2);	/* Joliet: the long names */
			c->root_size = le32(vd + 156 + 10);
			c->joliet = 1;
		}
	}
	return found ? 0 : E_CORRUPT;
}

/* ---- the drive ---- */

static int cd_stat(void *ctx, const char *path, struct vstat *st)
{
	struct changer *c = ctx;
	struct find f;
	int r = lookup(c, path, &f);
	if (r)
		return r;
	if (!f.rec[0]) {	/* the root */
		memset(st, 0, sizeof *st);
		st->type = VFS_DIR;
		st->attr = A_DIR | A_RO;
		return 0;
	}
	rec_stat(c, f.rec, st);
	return 0;
}

struct listing {
	int (*cb)(void *arg, struct vstat *st);
	void *arg;
	struct vstat st;
};

static int list_fn(struct changer *c, const uint8_t *r, void *arg)
{
	struct listing *l = arg;
	rec_stat(c, r, &l->st);
	return l->cb(l->arg, &l->st) ? 1 : 0;
}

static int cd_list(void *ctx, const char *path, int (*cb)(void *arg, struct vstat *st), void *arg)
{
	struct changer *c = ctx;
	struct find f;
	int r = lookup(c, path, &f);
	if (r)
		return r;
	if (!f.dir)
		return E_NOTDIR;
	struct listing *l = kmalloc(sizeof *l);
	if (!l)
		return E_NOMEM;
	l->cb = cb;
	l->arg = arg;
	r = dir_scan(c, f.lba, f.size, list_fn, l);
	kfree(l);
	return r < 0 ? r : 0;
}

static long cd_read(void *ctx, const char *path, uint64_t off, void *buf, size_t n)
{
	struct changer *c = ctx;
	struct find f;
	int r = lookup(c, path, &f);
	if (r)
		return r;
	if (f.dir)
		return E_ISDIR;
	if (off >= f.size)
		return 0;
	n = (size_t)MIN((uint64_t)n, f.size - off);
	r = disc_read(c, (uint64_t)f.lba * CD_SECTOR + off, buf, n);
	return r ? r : (long)n;
}

static long cd_write(void *ctx, const char *path, uint64_t off, const void *buf, size_t n)
{
	(void)ctx; (void)path; (void)off; (void)buf; (void)n;
	return E_ACCES;		/* it's a CD */
}
static int cd_create(void *ctx, const char *path, int type) { (void)ctx; (void)path; (void)type; return E_ACCES; }
static int cd_truncate(void *ctx, const char *path, uint64_t size) { (void)ctx; (void)path; (void)size; return E_ACCES; }
static int cd_remove(void *ctx, const char *path) { (void)ctx; (void)path; return E_ACCES; }
static int cd_rename(void *ctx, const char *a, const char *b) { (void)ctx; (void)a; (void)b; return E_ACCES; }
static int cd_setattr(void *ctx, const char *path, uint8_t attr) { (void)ctx; (void)path; (void)attr; return E_ACCES; }

static int cd_info(void *ctx, char *label, size_t n, uint64_t *free, uint64_t *total)
{
	struct changer *c = ctx;
	if (!c->have_src)
		return E_NOTREADY;
	strncpy0(label, c->label, n);
	*free = 0;
	*total = c->vol_bytes;
	return 0;
}

static const struct vfs_ops cd_ops = {
	.stat = cd_stat,
	.list = cd_list,
	.read = cd_read,
	.write = cd_write,
	.create = cd_create,
	.truncate = cd_truncate,
	.remove = cd_remove,
	.rename = cd_rename,
	.setattr = cd_setattr,
	.info = cd_info,
};

/* ---- changers ---- */

struct changer *changer_new(char letter, const char *desc, cd_open_fn open, void *ctx, int session)
{
	letter = (char)toupper(letter);
	if (letter < 'A' || letter > 'Z' || vfs_drive(letter))
		return NULL;
	int slot = -1;
	for (int i = 0; i < MAX_CHANGERS; i++)
		if (!all[i]) {
			slot = i;
			break;
		}
	if (slot < 0)
		return NULL;
	struct changer *c = kzalloc(sizeof *c);
	if (!c)
		return NULL;
	c->letter = letter;
	c->session = session;
	c->open = open;
	c->ctx = ctx;
	c->cur = -1;
	strncpy0(c->source, desc, sizeof c->source);
	c->drive.letter = letter;
	c->drive.ops = &cd_ops;
	c->drive.ctx = c;
	snprintf(c->drive.desc, sizeof c->drive.desc, "CD: %s", desc);
	int r = session ? vfs_session_mount(letter, &c->drive) : vfs_mount(letter, &cd_ops, c, c->drive.desc);
	if (r) {
		kfree(c);
		return NULL;
	}
	all[slot] = c;
	return c;
}

int changer_add(struct changer *c, const char *name)
{
	if (c->ndiscs >= CD_MAX_DISCS)
		return E_NOSPC;
	strncpy0(c->names[c->ndiscs++], name, sizeof c->names[0]);
	return 0;
}

static void eject(struct changer *c)
{
	if (c->have_src && c->src.close)
		c->src.close(&c->src);
	c->have_src = 0;
	c->cur = -1;
	c->label[0] = 0;
	cache_drop(c);
}

int changer_insert(struct changer *c, int disc)
{
	if (disc >= c->ndiscs || disc < -1)
		return E_INVAL;
	eject(c);
	c->gen++;		/* MSCDEX: the media changed */
	if (disc < 0)
		return 0;
	memset(&c->src, 0, sizeof c->src);
	int r = c->open(c->ctx, disc, &c->src);
	if (r)
		return r;
	c->have_src = 1;
	c->cur = disc;
	r = iso_open(c);
	if (r) {
		eject(c);
		return r;
	}
	return 0;
}

void changer_free(struct changer *c)
{
	if (c->session)
		vfs_session_mount(c->letter, NULL);
	else
		vfs_unmount(c->letter);
	eject(c);
	for (int i = 0; i < CACHE_CHUNKS; i++)
		kfree(c->cache[i].data);
	for (int i = 0; i < MAX_CHANGERS; i++)
		if (all[i] == c)
			all[i] = NULL;
	kfree(c);
}

struct changer *changer_at(char letter)
{
	struct vfs_drive *d = vfs_drive(letter);
	if (!d || d->ops != &cd_ops)
		return NULL;
	return d->ctx;
}

int changer_letter(struct changer *c) { return c->letter; }
int changer_discs(struct changer *c) { return c->ndiscs; }
int changer_current(struct changer *c) { return c->cur; }
const char *changer_disc_name(struct changer *c, int i) { return i >= 0 && i < c->ndiscs ? c->names[i] : ""; }
const char *changer_source(struct changer *c) { return c->source; }
uint32_t changer_generation(struct changer *c) { return c->gen; }
const char *changer_label(struct changer *c) { return c->label; }
uint64_t changer_size(struct changer *c) { return c->have_src ? c->vol_bytes : 0; }

int cd_letters(char *out, size_t n)
{
	size_t k = 0;
	for (char l = 'A'; l <= 'Z' && k + 1 < n; l++)
		if (changer_at(l))
			out[k++] = l;
	out[k] = 0;
	return (int)k;
}

/* ---- MOUNT: images on a drive here (C:, T:, anywhere) ---- */

struct files {
	char paths[CD_MAX_DISCS][VFS_PATH_MAX];
};

static long file_read(struct cdsrc *s, uint64_t off, void *buf, size_t n)
{
	struct vfile *f = s->ctx;
	if (vfs_seek(f, off))
		return E_IO;
	return vfs_read(f, buf, n);
}

static void file_close(struct cdsrc *s)
{
	vfs_close(s->ctx);
	s->ctx = NULL;
}

static int file_open(void *ctx, int i, struct cdsrc *out)
{
	struct files *fl = ctx;
	int e = 0;
	struct vfile *f = vfs_open(fl->paths[i], O_READ, &e);
	if (!f)
		return e ? e : E_NOENT;
	out->read = file_read;
	out->close = file_close;
	out->ctx = f;
	return 0;
}

int cd_mount_files(char letter, const char *files, char *err, size_t n)
{
	struct files *fl = kzalloc(sizeof *fl);
	if (!fl)
		return E_NOMEM;
	int count = 0;
	const char *p = files;
	while (*p && count < CD_MAX_DISCS) {
		char one[VFS_PATH_MAX];
		size_t k = 0;
		while (*p && *p != ',' && k + 1 < sizeof one)
			one[k++] = *p++;
		one[k] = 0;
		if (*p == ',')
			p++;
		if (!one[0])
			continue;
		struct vstat st;
		if (resolve_path(one, fl->paths[count], sizeof fl->paths[count]) || vfs_stat(fl->paths[count], &st) ||
		    st.type != VFS_FILE) {
			snprintf(err, n, "%s: no such file", one);
			kfree(fl);
			return E_NOENT;
		}
		count++;
	}
	if (!count) {
		snprintf(err, n, "no image given");
		kfree(fl);
		return E_INVAL;
	}
	/* where the discs are: the file, or for a changer full of them, their folder */
	char where[VFS_PATH_MAX];
	strncpy0(where, fl->paths[0], sizeof where);
	if (count > 1) {
		char *slash = strrchr(where, '\\');
		if (slash && slash > where + 2)
			*slash = 0;
		else if (slash)
			slash[1] = 0;
	}
	struct changer *c = changer_new(letter, where, file_open, fl, task_current()->sdrives != NULL);
	if (!c) {
		snprintf(err, n, "%c: is in use", toupper(letter));
		kfree(fl);
		return E_EXIST;
	}
	for (int i = 0; i < count; i++)
		changer_add(c, vfs_basename(fl->paths[i]));
	int r = changer_insert(c, 0);
	if (r) {
		snprintf(err, n, "%s isn't a CD image (ISO 9660)", fl->paths[0]);
		changer_free(c);
		kfree(fl);
		return r;
	}
	return 0;
}

int cd_unmount(char letter)
{
	struct changer *c = changer_at(letter);
	if (!c)
		return E_BADDRIVE;
	void *ctx = c->ctx;
	int local = c->open == file_open;
	if (!local)
		return E_ACCES;		/* bterm's: it goes when the session does */
	changer_free(c);
	if (local)
		kfree(ctx);
	return 0;
}

/* one more disc in a MOUNTed changer (or a new drive, if there's none at the letter) */
int cd_add_file(char letter, const char *file, char *err, size_t n)
{
	struct changer *c = changer_at(letter);
	if (!c)
		return cd_mount_files(letter, file, err, n);
	if (c->open != file_open) {
		snprintf(err, n, "%c:'s discs are bterm's: give it another -d", c->letter);
		return E_ACCES;
	}
	if (c->ndiscs >= CD_MAX_DISCS) {
		snprintf(err, n, "%c: has all the discs it can take (%d)", c->letter, CD_MAX_DISCS);
		return E_NOSPC;
	}
	struct files *fl = c->ctx;
	struct vstat st;
	char *path = fl->paths[c->ndiscs];
	if (resolve_path(file, path, VFS_PATH_MAX) || vfs_stat(path, &st) || st.type != VFS_FILE) {
		snprintf(err, n, "%s: no such file", file);
		return E_NOENT;
	}
	return changer_add(c, vfs_basename(path)) < 0 ? E_NOSPC : 0;
}

/* where a MOUNTed changer's discs come from: 1 here, 0 bterm's */
int changer_local(struct changer *c)
{
	return c->open == file_open;
}
