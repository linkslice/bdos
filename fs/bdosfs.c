/*
 * bdosfs.c - the bdos filesystem (see bdosfs.h for the format).
 *
 * Built twice: into the kernel (BDOS_KERNEL) and into the host tools.
 */
#include "bdosfs.h"

#ifdef BDOS_KERNEL
#include "../kernel/kernel.h"
#define fs_malloc(n)	kmalloc(n)
#define fs_free(p)	kfree(p)
#else
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#define fs_malloc(n)	malloc(n)
#define fs_free(p)	free(p)
static uint32_t crc32c(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0x82F63B78 & (0 - (crc & 1)));
	}
	return ~crc;
}
#endif

#ifndef MIN
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#endif

#define CACHE_N		192
#define INODES_PER_BLOCK (BFS_BLOCK / 256)
#define DIRENTS_PER_BLOCK (BFS_BLOCK / 256)
#define BITS_PER_BLOCK	(BFS_BLOCK * 8)
#define JDESC_MAX	((BFS_BLOCK - 32) / 8)

struct cbuf {
	uint64_t blk;
	uint8_t *data;
	int valid;
	int txn;		/* pinned: part of the open transaction */
	uint64_t lru;
};

struct bfs {
	struct bfs_dev *dev;
	struct bfs_super sb;
	struct cbuf cache[CACHE_N];
	uint64_t clock;
	int txn_depth;
	uint64_t txn[BFS_TXN_MAX];
	int ntxn;
	int txn_failed;
	uint64_t free_blocks;
	uint64_t alloc_hint;
	uint32_t inode_hint;
	int replayed;
	uint8_t *zero;		/* one block of zeros */
};

struct jdesc {
	uint64_t magic;
	uint64_t seq;
	uint32_t count;
	uint32_t data_crc;
	uint32_t desc_crc;
	uint32_t _pad;
	uint64_t blocks[JDESC_MAX];
};

struct jcommit {
	uint64_t magic;
	uint64_t seq;
	uint32_t data_crc;
	uint32_t count;
};

static int fold(int c)
{
	return (c >= 'a' && c <= 'z') ? c - 32 : c;
}

static int name_eq(const char *a, const char *b)
{
	while (*a && fold((unsigned char)*a) == fold((unsigned char)*b))
		a++, b++;
	return !*a && !*b;
}

static uint32_t name_hash(const char *s)
{
	uint32_t crc = 0;
	for (; *s; s++) {
		uint8_t c = (uint8_t)fold((unsigned char)*s);
		crc = crc32c(crc, &c, 1);
	}
	return crc;
}

const char *bfs_strerror(int err)
{
	switch (err) {
	case BFS_OK: return "OK";
	case BFS_ENOENT: return "File not found";
	case BFS_EEXIST: return "File already exists";
	case BFS_ENOTDIR: return "Not a directory";
	case BFS_EISDIR: return "Is a directory";
	case BFS_ENOSPC: return "Insufficient disk space";
	case BFS_ENOTEMPTY: return "Directory not empty";
	case BFS_EINVAL: return "Invalid name or parameter";
	case BFS_EIO: return "Disk I/O error";
	case BFS_ECORRUPT: return "Filesystem damaged - run CHKDSK";
	case BFS_ENAMETOOLONG: return "Name too long";
	case BFS_EACCES: return "Access denied";
	case BFS_ENOMEM: return "Out of memory";
	}
	return "Unknown error";
}

/* ---- raw I/O ------------------------------------------------------------ */

static int dev_read(struct bfs *fs, uint64_t blk, uint32_t n, void *buf)
{
	if (blk + n > fs->dev->nblocks)
		return BFS_EIO;
	return fs->dev->read(fs->dev->ctx, blk, n, buf) ? BFS_EIO : 0;
}

static int dev_write(struct bfs *fs, uint64_t blk, uint32_t n, const void *buf)
{
	if (blk + n > fs->dev->nblocks)
		return BFS_EIO;
	return fs->dev->write(fs->dev->ctx, blk, n, buf) ? BFS_EIO : 0;
}

static int dev_flush(struct bfs *fs)
{
	return fs->dev->flush ? (fs->dev->flush(fs->dev->ctx) ? BFS_EIO : 0) : 0;
}

/* ---- superblock --------------------------------------------------------- */

static uint32_t super_crc(struct bfs_super *sb)
{
	uint32_t save = sb->crc;
	sb->crc = 0;
	uint32_t c = crc32c(0, sb, sizeof *sb);
	sb->crc = save;
	return c;
}

static int super_write(struct bfs *fs)
{
	fs->sb.generation++;
	fs->sb.crc = super_crc(&fs->sb);
	uint8_t *buf = fs->zero;	/* borrow, restore after */
	memcpy(buf, &fs->sb, sizeof fs->sb);
	int r = dev_write(fs, fs->sb.generation & 1, 1, buf);
	memset(buf, 0, BFS_BLOCK);
	if (!r)
		r = dev_flush(fs);
	return r;
}

/* ---- block cache -------------------------------------------------------- */

static struct cbuf *cache_find(struct bfs *fs, uint64_t blk)
{
	for (int i = 0; i < CACHE_N; i++)
		if (fs->cache[i].valid && fs->cache[i].blk == blk)
			return &fs->cache[i];
	return NULL;
}

static struct cbuf *cache_slot(struct bfs *fs)
{
	struct cbuf *best = NULL;
	for (int i = 0; i < CACHE_N; i++) {
		struct cbuf *c = &fs->cache[i];
		if (!c->valid)
			return c;
		if (!c->txn && (!best || c->lru < best->lru))
			best = c;
	}
	return best;	/* NULL only if every block is pinned */
}

/* Get a metadata block. fresh = don't read, start from zeros. */
static uint8_t *bget(struct bfs *fs, uint64_t blk, int fresh)
{
	struct cbuf *c = cache_find(fs, blk);
	if (!c) {
		c = cache_slot(fs);
		if (!c)
			return NULL;
		c->valid = 0;
		if (fresh)
			memset(c->data, 0, BFS_BLOCK);
		else if (dev_read(fs, blk, 1, c->data))
			return NULL;
		c->blk = blk;
		c->valid = 1;
		c->txn = 0;
	} else if (fresh) {
		memset(c->data, 0, BFS_BLOCK);
	}
	c->lru = ++fs->clock;
	return c->data;
}

static void cache_drop(struct bfs *fs, uint64_t blk)
{
	struct cbuf *c = cache_find(fs, blk);
	if (c && !c->txn)
		c->valid = 0;
}

/* ---- transactions ------------------------------------------------------- */

static void txn_begin(struct bfs *fs)
{
	if (fs->txn_depth++ == 0) {
		fs->ntxn = 0;
		fs->txn_failed = 0;
	}
}

/* Mark a cached block as modified in the open transaction. */
static int bdirty(struct bfs *fs, uint64_t blk)
{
	struct cbuf *c = cache_find(fs, blk);
	if (!c)
		return BFS_EIO;
	if (c->txn)
		return 0;
	if (fs->ntxn >= BFS_TXN_MAX) {
		fs->txn_failed = 1;
		return BFS_ENOSPC;
	}
	c->txn = 1;
	fs->txn[fs->ntxn++] = blk;
	return 0;
}

static void txn_forget(struct bfs *fs, uint64_t blk)
{
	for (int i = 0; i < fs->ntxn; i++) {
		if (fs->txn[i] == blk) {
			fs->txn[i] = fs->txn[--fs->ntxn];
			struct cbuf *c = cache_find(fs, blk);
			if (c) {
				c->txn = 0;
				c->valid = 0;
			}
			return;
		}
	}
	cache_drop(fs, blk);
}

static int txn_commit(struct bfs *fs)
{
	if (!fs->ntxn)
		return 0;
	struct jdesc *d = fs_malloc(BFS_BLOCK);
	if (!d)
		return BFS_ENOMEM;
	memset(d, 0, BFS_BLOCK);
	d->magic = BFS_JDESC;
	d->seq = fs->sb.checkpoint_seq + 1;
	d->count = (uint32_t)fs->ntxn;
	uint32_t crc = 0;
	for (int i = 0; i < fs->ntxn; i++) {
		d->blocks[i] = fs->txn[i];
		crc = crc32c(crc, cache_find(fs, fs->txn[i])->data, BFS_BLOCK);
	}
	d->data_crc = crc;
	d->desc_crc = 0;
	d->desc_crc = crc32c(0, d, BFS_BLOCK);

	uint64_t j = fs->sb.journal_start;
	int r = dev_write(fs, j, 1, d);
	for (int i = 0; i < fs->ntxn && !r; i++)
		r = dev_write(fs, j + 1 + i, 1, cache_find(fs, fs->txn[i])->data);
	if (!r)
		r = dev_flush(fs);
	if (!r) {
		struct jcommit *cm = (struct jcommit *)d;
		uint64_t seq = d->seq;
		memset(d, 0, BFS_BLOCK);
		cm->magic = BFS_JCOMMIT;
		cm->seq = seq;
		cm->data_crc = crc;
		cm->count = (uint32_t)fs->ntxn;
		r = dev_write(fs, j + 1 + fs->ntxn, 1, cm);
		if (!r)
			r = dev_flush(fs);
		/* Committed: now it's safe to write the blocks home. */
		for (int i = 0; i < fs->ntxn && !r; i++)
			r = dev_write(fs, fs->txn[i], 1, cache_find(fs, fs->txn[i])->data);
		if (!r)
			r = dev_flush(fs);
		if (!r) {
			fs->sb.checkpoint_seq = seq;
			r = super_write(fs);
		}
	}
	for (int i = 0; i < fs->ntxn; i++) {
		struct cbuf *c = cache_find(fs, fs->txn[i]);
		if (c) {
			c->txn = 0;
			if (r)
				c->valid = 0;	/* don't trust what didn't reach disk */
		}
	}
	fs->ntxn = 0;
	fs_free(d);
	return r;
}

/*
 * End of an operation. If anything inside it failed we still commit what
 * was changed so far: every individual step leaves the structures
 * consistent, it's only the operation that's incomplete.
 */
static int txn_end(struct bfs *fs, int result)
{
	if (--fs->txn_depth > 0)
		return result;
	int r = txn_commit(fs);
	if (fs->txn_failed && result >= 0)
		result = BFS_ENOSPC;
	return result < 0 ? result : (r ? r : result);
}

/* Commit early when a long operation is about to overflow the journal. */
static int txn_room(struct bfs *fs, int need)
{
	if (fs->ntxn + need <= BFS_TXN_MAX)
		return 0;
	return txn_commit(fs);
}

static int journal_replay(struct bfs *fs)
{
	struct jdesc *d = fs_malloc(BFS_BLOCK);
	uint8_t *blk = fs_malloc(BFS_BLOCK);
	int r = 0;
	if (!d || !blk) {
		r = BFS_ENOMEM;
		goto out;
	}
	uint64_t j = fs->sb.journal_start;
	if (dev_read(fs, j, 1, d))
		goto out;
	uint32_t dc = d->desc_crc;
	d->desc_crc = 0;
	if (d->magic != BFS_JDESC || d->seq != fs->sb.checkpoint_seq + 1 ||
	    d->count == 0 || d->count > JDESC_MAX ||
	    d->count + 2 > fs->sb.journal_blocks || crc32c(0, d, BFS_BLOCK) != dc)
		goto out;	/* nothing pending */

	uint32_t crc = 0;
	for (uint32_t i = 0; i < d->count; i++) {
		if (dev_read(fs, j + 1 + i, 1, blk)) {
			r = BFS_EIO;
			goto out;
		}
		crc = crc32c(crc, blk, BFS_BLOCK);
	}
	struct jcommit cm;
	if (dev_read(fs, j + 1 + d->count, 1, blk)) {
		r = BFS_EIO;
		goto out;
	}
	memcpy(&cm, blk, sizeof cm);
	if (cm.magic != BFS_JCOMMIT || cm.seq != d->seq || cm.data_crc != crc ||
	    crc != d->data_crc)
		goto out;	/* never committed: discard */

	for (uint32_t i = 0; i < d->count; i++) {
		if (d->blocks[i] >= fs->sb.total_blocks || dev_read(fs, j + 1 + i, 1, blk) ||
		    dev_write(fs, d->blocks[i], 1, blk)) {
			r = BFS_EIO;
			goto out;
		}
	}
	dev_flush(fs);
	fs->sb.checkpoint_seq = d->seq;
	r = super_write(fs);
	fs->replayed++;
out:
	fs_free(d);
	fs_free(blk);
	return r;
}

/* ---- bitmap ------------------------------------------------------------- */

static int bit_get(struct bfs *fs, uint64_t b, int *v)
{
	uint8_t *m = bget(fs, fs->sb.bitmap_start + b / BITS_PER_BLOCK, 0);
	if (!m)
		return BFS_EIO;
	uint64_t i = b % BITS_PER_BLOCK;
	*v = (m[i / 8] >> (i % 8)) & 1;
	return 0;
}

static int bit_set(struct bfs *fs, uint64_t b, int v)
{
	uint64_t mb = fs->sb.bitmap_start + b / BITS_PER_BLOCK;
	uint8_t *m = bget(fs, mb, 0);
	if (!m)
		return BFS_EIO;
	uint64_t i = b % BITS_PER_BLOCK;
	if (v)
		m[i / 8] |= (uint8_t)(1 << (i % 8));
	else
		m[i / 8] &= (uint8_t)~(1 << (i % 8));
	return bdirty(fs, mb);
}

/* Allocate up to `want` contiguous blocks, preferably at `goal`. */
static int alloc_run(struct bfs *fs, uint64_t goal, uint64_t want,
    uint64_t *start, uint64_t *got)
{
	uint64_t lo = fs->sb.data_start, hi = fs->sb.total_blocks;
	if (goal < lo || goal >= hi)
		goal = fs->alloc_hint >= lo && fs->alloc_hint < hi ? fs->alloc_hint : lo;
	if (!fs->free_blocks)
		return BFS_ENOSPC;

	for (int pass = 0; pass < 2; pass++) {
		uint64_t b = pass ? lo : goal, end = pass ? goal : hi;
		while (b < end) {
			/* skip full bytes quickly */
			uint8_t *m = bget(fs, fs->sb.bitmap_start + b / BITS_PER_BLOCK, 0);
			if (!m)
				return BFS_EIO;
			uint64_t i = b % BITS_PER_BLOCK;
			if (!(i % 8) && m[i / 8] == 0xFF && b + 8 <= end) {
				b += 8;
				continue;
			}
			int used;
			if (bit_get(fs, b, &used))
				return BFS_EIO;
			if (used) {
				b++;
				continue;
			}
			uint64_t n = 0;
			while (n < want && b + n < hi) {
				if (bit_get(fs, b + n, &used))
					return BFS_EIO;
				if (used)
					break;
				n++;
			}
			/* bitmap blocks touched by this run, plus slack */
			if (fs->ntxn + 3 > BFS_TXN_MAX && n > 1)
				n = 1;
			for (uint64_t k = 0; k < n; k++) {
				int r = bit_set(fs, b + k, 1);
				if (r)
					return r;
			}
			*start = b;
			*got = n;
			fs->free_blocks -= n;
			fs->alloc_hint = b + n;
			return 0;
		}
	}
	return BFS_ENOSPC;
}

static int free_run(struct bfs *fs, uint64_t start, uint64_t len)
{
	for (uint64_t k = 0; k < len; k++) {
		int r = bit_set(fs, start + k, 0);
		if (r)
			return r;
		txn_forget(fs, start + k);
	}
	fs->free_blocks += len;
	return 0;
}

/* ---- inodes ------------------------------------------------------------- */

static uint32_t inode_crc(struct bfs_inode *in)
{
	uint32_t save = in->crc;
	in->crc = 0;
	uint32_t c = crc32c(0, in, sizeof *in);
	in->crc = save;
	return c;
}

static int inode_read(struct bfs *fs, uint32_t ino, struct bfs_inode *in)
{
	if (!ino || ino > fs->sb.inode_count)
		return BFS_EINVAL;
	uint8_t *b = bget(fs, fs->sb.inode_start + (ino - 1) / INODES_PER_BLOCK, 0);
	if (!b)
		return BFS_EIO;
	memcpy(in, b + ((ino - 1) % INODES_PER_BLOCK) * 256, 256);
	if (in->mode != BFS_FREE && inode_crc(in) != in->crc)
		return BFS_ECORRUPT;
	return 0;
}

static int inode_write(struct bfs *fs, uint32_t ino, struct bfs_inode *in)
{
	uint64_t blk = fs->sb.inode_start + (ino - 1) / INODES_PER_BLOCK;
	uint8_t *b = bget(fs, blk, 0);
	if (!b)
		return BFS_EIO;
	if (in->mode != BFS_FREE)
		in->crc = inode_crc(in);
	memcpy(b + ((ino - 1) % INODES_PER_BLOCK) * 256, in, 256);
	return bdirty(fs, blk);
}

static int inode_alloc(struct bfs *fs, uint32_t *out)
{
	uint32_t n = (uint32_t)fs->sb.inode_count;
	for (uint32_t k = 0; k < n; k++) {
		uint32_t ino = 1 + (fs->inode_hint + k) % n;
		uint8_t *b = bget(fs, fs->sb.inode_start + (ino - 1) / INODES_PER_BLOCK, 0);
		if (!b)
			return BFS_EIO;
		if (b[((ino - 1) % INODES_PER_BLOCK) * 256] == BFS_FREE) {
			fs->inode_hint = ino;
			*out = ino;
			return 0;
		}
	}
	return BFS_ENOSPC;
}

/* ---- extents ------------------------------------------------------------ */

static int ext_get(struct bfs *fs, struct bfs_inode *in, int i, struct bfs_extent *e)
{
	if (i < BFS_INLINE_EXT) {
		*e = in->ext[i];
		return 0;
	}
	uint8_t *b = bget(fs, in->ext_block, 0);
	if (!b)
		return BFS_EIO;
	memcpy(e, b + (i - BFS_INLINE_EXT) * 16, 16);
	return 0;
}

static int ext_set(struct bfs *fs, struct bfs_inode *in, int i, struct bfs_extent *e)
{
	if (i < BFS_INLINE_EXT) {
		in->ext[i] = *e;
		return 0;
	}
	if (!in->ext_block) {
		uint64_t s, g;
		int r = alloc_run(fs, in->ext[BFS_INLINE_EXT - 1].start, 1, &s, &g);
		if (r)
			return r;
		in->ext_block = s;
		if (!bget(fs, s, 1))
			return BFS_EIO;
	}
	uint8_t *b = bget(fs, in->ext_block, 0);
	if (!b)
		return BFS_EIO;
	memcpy(b + (i - BFS_INLINE_EXT) * 16, e, 16);
	return bdirty(fs, in->ext_block);
}

static uint64_t nblocks_of(uint64_t size)
{
	return (size + BFS_BLOCK - 1) / BFS_BLOCK;
}

static uint64_t allocated_blocks(struct bfs *fs, struct bfs_inode *in)
{
	uint64_t n = 0;
	struct bfs_extent e;
	for (int i = 0; i < in->nextents; i++)
		if (!ext_get(fs, in, i, &e))
			n += e.len;
	return n;
}

/* File block -> disk block, plus how many follow contiguously. */
static int map(struct bfs *fs, struct bfs_inode *in, uint64_t fblk, uint64_t *disk,
    uint64_t *run)
{
	uint64_t base = 0;
	struct bfs_extent e;
	for (int i = 0; i < in->nextents; i++) {
		int r = ext_get(fs, in, i, &e);
		if (r)
			return r;
		if (fblk < base + e.len) {
			*disk = e.start + (fblk - base);
			*run = e.len - (fblk - base);
			return 0;
		}
		base += e.len;
	}
	return BFS_ECORRUPT;
}

/* Append `add` blocks to the inode (in memory; caller writes the inode). */
static int grow(struct bfs *fs, struct bfs_inode *in, uint64_t add)
{
	while (add) {
		struct bfs_extent last = { 0, 0 };
		if (in->nextents)
			ext_get(fs, in, in->nextents - 1, &last);
		uint64_t s, got;
		int r = txn_room(fs, 4);
		if (!r)
			r = alloc_run(fs, last.start + last.len, add, &s, &got);
		if (r)
			return r;
		if (in->nextents && s == last.start + last.len) {
			last.len += got;
			r = ext_set(fs, in, in->nextents - 1, &last);
		} else {
			if (in->nextents >= BFS_INLINE_EXT + BFS_EXT_PER_BLOCK) {
				free_run(fs, s, got);
				return BFS_ENOSPC;	/* too fragmented */
			}
			struct bfs_extent e = { s, got };
			r = ext_set(fs, in, in->nextents, &e);
			if (!r)
				in->nextents++;
		}
		if (r)
			return r;
		add -= got;
	}
	return 0;
}

/* Drop blocks beyond `keep`. */
static int shrink(struct bfs *fs, struct bfs_inode *in, uint64_t keep)
{
	uint64_t have = allocated_blocks(fs, in);
	while (have > keep && in->nextents) {
		struct bfs_extent e;
		int r = txn_room(fs, 4);
		if (!r)
			r = ext_get(fs, in, in->nextents - 1, &e);
		if (r)
			return r;
		uint64_t drop = MIN(e.len, have - keep);
		r = free_run(fs, e.start + e.len - drop, drop);
		if (r)
			return r;
		e.len -= drop;
		have -= drop;
		if (!e.len) {
			in->nextents--;
			struct bfs_extent z = { 0, 0 };
			ext_set(fs, in, in->nextents, &z);
		} else {
			ext_set(fs, in, in->nextents - 1, &e);
		}
	}
	if (in->nextents <= BFS_INLINE_EXT && in->ext_block) {
		free_run(fs, in->ext_block, 1);
		in->ext_block = 0;
	}
	return 0;
}

/* ---- names -------------------------------------------------------------- */

static int valid_name(const char *n)
{
	size_t len = strlen(n);
	if (!len)
		return BFS_EINVAL;
	if (len > BFS_NAME_MAX)
		return BFS_ENAMETOOLONG;
	if (!strcmp(n, ".") || !strcmp(n, ".."))
		return BFS_EINVAL;
	for (const char *p = n; *p; p++)
		if ((unsigned char)*p < 32 || strchr("\\/:*?\"<>|", *p))
			return BFS_EINVAL;
	return 0;
}

static int short_char_ok(int c)
{
	return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	    (c && strchr("!#$%&'()-@^_`{}~", c));
}

static void short_format(const char *sn, char *out)	/* 11 chars -> "NAME.EXT" */
{
	int o = 0;
	for (int i = 0; i < 8 && sn[i] != ' '; i++)
		out[o++] = sn[i];
	if (sn[8] != ' ') {
		out[o++] = '.';
		for (int i = 8; i < 11 && sn[i] != ' '; i++)
			out[o++] = sn[i];
	}
	out[o] = 0;
}

/* Does `name` already look like an 8.3 name? Then use it verbatim. */
static int is_83(const char *name, char *sn)
{
	const char *dot = strrchr(name, '.');
	size_t base = dot ? (size_t)(dot - name) : strlen(name);
	size_t ext = dot ? strlen(dot + 1) : 0;
	if (!base || base > 8 || ext > 3 || (dot && !ext))
		return 0;
	memset(sn, ' ', 11);
	for (size_t i = 0; i < base; i++) {
		int c = fold((unsigned char)name[i]);
		if (!short_char_ok(c))
			return 0;
		sn[i] = (char)c;
	}
	for (size_t i = 0; i < ext; i++) {
		int c = fold((unsigned char)dot[1 + i]);
		if (!short_char_ok(c))
			return 0;
		sn[8 + i] = (char)c;
	}
	return 1;
}

static int dir_short_taken(struct bfs *fs, uint32_t dir, const char *sn);

static int make_short(struct bfs *fs, uint32_t dir, const char *name, char *sn)
{
	if (is_83(name, sn) && !dir_short_taken(fs, dir, sn))
		return 0;
	char base[9], ext[4];
	int nb = 0, ne = 0;
	const char *dot = strrchr(name, '.');
	if (dot == name)
		dot = NULL;	/* ".profile": all base */
	for (const char *p = name; *p && p != dot && nb < 8; p++) {
		int c = fold((unsigned char)*p);
		if (c == ' ' || c == '.')
			continue;
		base[nb++] = (char)(short_char_ok(c) ? c : '_');
	}
	if (dot)
		for (const char *p = dot + 1; *p && ne < 3; p++) {
			int c = fold((unsigned char)*p);
			if (c == ' ' || c == '.')
				continue;
			ext[ne++] = (char)(short_char_ok(c) ? c : '_');
		}
	if (!nb)
		base[nb++] = '_';
	for (int n = 1; n < 100000; n++) {
		char tail[8];
		int tl = 0;
		int v = n, digits[6], nd = 0;
		while (v) {
			digits[nd++] = v % 10;
			v /= 10;
		}
		tail[tl++] = '~';
		while (nd)
			tail[tl++] = (char)('0' + digits[--nd]);
		int keep = MIN(nb, 8 - tl);
		memset(sn, ' ', 11);
		memcpy(sn, base, (size_t)keep);
		memcpy(sn + keep, tail, (size_t)tl);
		memcpy(sn + 8, ext, (size_t)ne);
		if (!dir_short_taken(fs, dir, sn))
			return 0;
	}
	return BFS_ENOSPC;
}

/* ---- directories -------------------------------------------------------- */

/* Visit every used entry; fn returns nonzero to stop. */
typedef int (*dir_fn)(struct bfs *fs, struct bfs_dirent *de, uint64_t slot, void *arg);

static int dir_walk(struct bfs *fs, uint32_t dir, dir_fn fn, void *arg)
{
	struct bfs_inode in;
	int r = inode_read(fs, dir, &in);
	if (r)
		return r;
	if (in.mode != BFS_DIR)
		return BFS_ENOTDIR;
	uint64_t nb = nblocks_of(in.size);
	for (uint64_t fb = 0; fb < nb; fb++) {
		uint64_t disk, run;
		if ((r = map(fs, &in, fb, &disk, &run)))
			return r;
		uint8_t *b = bget(fs, disk, 0);
		if (!b)
			return BFS_EIO;
		for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
			struct bfs_dirent *de = (struct bfs_dirent *)(b + i * 256);
			if (!de->ino)
				continue;
			int stop = fn(fs, de, fb * DIRENTS_PER_BLOCK + (uint64_t)i, arg);
			if (stop)
				return stop;
			/* fn may have evicted b from the cache */
			b = bget(fs, disk, 0);
			if (!b)
				return BFS_EIO;
		}
	}
	return 0;
}

struct find_arg {
	const char *name;
	uint32_t hash;
	char sfmt[13];
	int match_short;
	struct bfs_dirent found;
	uint64_t slot;
};

static int find_fn(struct bfs *fs, struct bfs_dirent *de, uint64_t slot, void *arg)
{
	(void)fs;
	struct find_arg *a = arg;
	int hit = (de->hash == a->hash && name_eq(de->name, a->name));
	if (!hit && a->match_short) {
		char s[13];
		short_format(de->short_name, s);
		hit = name_eq(s, a->name);
	}
	if (hit) {
		a->found = *de;
		a->slot = slot;
		return 1;
	}
	return 0;
}

static int dir_find(struct bfs *fs, uint32_t dir, const char *name,
    struct bfs_dirent *out, uint64_t *slot)
{
	struct find_arg a;
	memset(&a, 0, sizeof a);
	a.name = name;
	a.hash = name_hash(name);
	a.match_short = strlen(name) <= 12;
	int r = dir_walk(fs, dir, find_fn, &a);
	if (r < 0)
		return r;
	if (r != 1)
		return BFS_ENOENT;
	if (out)
		*out = a.found;
	if (slot)
		*slot = a.slot;
	return 0;
}

struct short_arg {
	const char *sn;
};

static int short_fn(struct bfs *fs, struct bfs_dirent *de, uint64_t slot, void *arg)
{
	(void)fs;
	(void)slot;
	return !memcmp(de->short_name, ((struct short_arg *)arg)->sn, 11);
}

static int dir_short_taken(struct bfs *fs, uint32_t dir, const char *sn)
{
	struct short_arg a = { sn };
	return dir_walk(fs, dir, short_fn, &a) == 1;
}

static int dir_add(struct bfs *fs, uint32_t dir, struct bfs_dirent *ent)
{
	struct bfs_inode in;
	int r = inode_read(fs, dir, &in);
	if (r)
		return r;
	uint64_t nb = nblocks_of(in.size);
	for (uint64_t fb = 0; fb < nb; fb++) {
		uint64_t disk, run;
		if ((r = map(fs, &in, fb, &disk, &run)))
			return r;
		uint8_t *b = bget(fs, disk, 0);
		if (!b)
			return BFS_EIO;
		for (int i = 0; i < DIRENTS_PER_BLOCK; i++) {
			struct bfs_dirent *de = (struct bfs_dirent *)(b + i * 256);
			if (!de->ino) {
				memcpy(de, ent, 256);
				return bdirty(fs, disk);
			}
		}
	}
	/* full: add a block */
	if ((r = grow(fs, &in, 1)))
		return r;
	uint64_t disk, run;
	if ((r = map(fs, &in, nb, &disk, &run)))
		return r;
	uint8_t *b = bget(fs, disk, 1);
	if (!b)
		return BFS_EIO;
	memcpy(b, ent, 256);
	if ((r = bdirty(fs, disk)))
		return r;
	in.size += BFS_BLOCK;
	return inode_write(fs, dir, &in);
}

static int dir_clear_slot(struct bfs *fs, uint32_t dir, uint64_t slot)
{
	struct bfs_inode in;
	int r = inode_read(fs, dir, &in);
	if (r)
		return r;
	uint64_t disk, run;
	if ((r = map(fs, &in, slot / DIRENTS_PER_BLOCK, &disk, &run)))
		return r;
	uint8_t *b = bget(fs, disk, 0);
	if (!b)
		return BFS_EIO;
	memset(b + (slot % DIRENTS_PER_BLOCK) * 256, 0, 256);
	return bdirty(fs, disk);
}

static int empty_fn(struct bfs *fs, struct bfs_dirent *de, uint64_t slot, void *arg)
{
	(void)fs; (void)de; (void)slot; (void)arg;
	return 1;
}

/* ---- public API --------------------------------------------------------- */

int bfs_format(struct bfs_dev *dev, const char *label, uint64_t now)
{
	uint64_t total = dev->nblocks;
	if (total < 64)
		return BFS_EINVAL;
	struct bfs_super sb;
	memset(&sb, 0, sizeof sb);
	sb.magic = BFS_MAGIC;
	sb.version = 1;
	sb.total_blocks = total;
	sb.journal_start = 2;
	sb.journal_blocks = total / 64;
	if (sb.journal_blocks < 16)
		sb.journal_blocks = 16;
	if (sb.journal_blocks > 1024)
		sb.journal_blocks = 1024;
	sb.bitmap_start = sb.journal_start + sb.journal_blocks;
	sb.bitmap_blocks = (total + BITS_PER_BLOCK - 1) / BITS_PER_BLOCK;
	sb.inode_start = sb.bitmap_start + sb.bitmap_blocks;
	sb.inode_count = total / 4;		/* one inode per 16 KiB */
	if (sb.inode_count < 64)
		sb.inode_count = 64;
	sb.inode_count = (sb.inode_count + INODES_PER_BLOCK - 1) / INODES_PER_BLOCK * INODES_PER_BLOCK;
	sb.inode_blocks = sb.inode_count / INODES_PER_BLOCK;
	sb.data_start = sb.inode_start + sb.inode_blocks;
	if (sb.data_start + 8 > total)
		return BFS_ENOSPC;
	sb.created = now;
	for (int i = 0; label && label[i] && i < 31; i++)
		sb.label[i] = (char)fold((unsigned char)label[i]);

	uint8_t *buf = fs_malloc(BFS_BLOCK);
	if (!buf)
		return BFS_ENOMEM;
	memset(buf, 0, BFS_BLOCK);
	/* zero journal header, bitmap, inode table */
	for (uint64_t b = 0; b < sb.data_start; b++)
		if (dev->write(dev->ctx, b, 1, buf)) {
			fs_free(buf);
			return BFS_EIO;
		}
	/* bitmap: metadata blocks are used */
	for (uint64_t mb = 0; mb < sb.bitmap_blocks; mb++) {
		memset(buf, 0, BFS_BLOCK);
		for (uint64_t i = 0; i < BITS_PER_BLOCK; i++) {
			uint64_t b = mb * BITS_PER_BLOCK + i;
			if (b < sb.data_start + 1 || b >= total)	/* +1: root dir block */
				buf[i / 8] |= (uint8_t)(1 << (i % 8));
		}
		dev->write(dev->ctx, sb.bitmap_start + mb, 1, buf);
	}
	/* root directory: inode 1 with one empty block */
	memset(buf, 0, BFS_BLOCK);
	dev->write(dev->ctx, sb.data_start, 1, buf);
	struct bfs_inode *root = (struct bfs_inode *)buf;
	root->mode = BFS_DIR;
	root->attr = BFS_ATTR_DIR;
	root->nextents = 1;
	root->ext[0].start = sb.data_start;
	root->ext[0].len = 1;
	root->size = BFS_BLOCK;
	root->ctime = root->mtime = now;
	root->parent = BFS_ROOT;
	root->crc = inode_crc(root);
	dev->write(dev->ctx, sb.inode_start, 1, buf);

	/* both superblocks, generations 1 and 2 */
	for (int i = 0; i < 2; i++) {
		sb.generation = (uint64_t)i + 1;
		sb.crc = 0;
		sb.crc = super_crc(&sb);
		memset(buf, 0, BFS_BLOCK);
		memcpy(buf, &sb, sizeof sb);
		dev->write(dev->ctx, sb.generation & 1, 1, buf);
	}
	if (dev->flush)
		dev->flush(dev->ctx);
	fs_free(buf);
	return 0;
}

int bfs_mount(struct bfs_dev *dev, struct bfs **out)
{
	struct bfs *fs = fs_malloc(sizeof *fs);
	if (!fs)
		return BFS_ENOMEM;
	memset(fs, 0, sizeof *fs);
	fs->dev = dev;
	fs->zero = fs_malloc(BFS_BLOCK);
	if (!fs->zero) {
		fs_free(fs);
		return BFS_ENOMEM;
	}
	memset(fs->zero, 0, BFS_BLOCK);
	for (int i = 0; i < CACHE_N; i++) {
		fs->cache[i].data = fs_malloc(BFS_BLOCK);
		if (!fs->cache[i].data) {
			bfs_unmount(fs);
			return BFS_ENOMEM;
		}
	}

	/* pick the newest valid superblock */
	uint8_t *buf = fs->cache[0].data;
	int found = 0;
	for (int i = 0; i < 2; i++) {
		struct bfs_super sb;
		if (dev->read(dev->ctx, (uint64_t)i, 1, buf))
			continue;
		memcpy(&sb, buf, sizeof sb);
		if (sb.magic != BFS_MAGIC || super_crc(&sb) != sb.crc)
			continue;
		if (!found || sb.generation > fs->sb.generation)
			fs->sb = sb;
		found = 1;
	}
	if (!found) {
		bfs_unmount(fs);
		return BFS_ECORRUPT;
	}
	if (fs->sb.total_blocks > dev->nblocks) {
		bfs_unmount(fs);
		return BFS_ECORRUPT;
	}
	int r = journal_replay(fs);
	if (r) {
		bfs_unmount(fs);
		return r;
	}
	/* count free blocks from the bitmap */
	uint64_t used = 0;
	for (uint64_t mb = 0; mb < fs->sb.bitmap_blocks; mb++) {
		uint8_t *m = bget(fs, fs->sb.bitmap_start + mb, 0);
		if (!m) {
			bfs_unmount(fs);
			return BFS_EIO;
		}
		for (int i = 0; i < BFS_BLOCK; i++) {
			uint8_t v = m[i];
			while (v) {
				used += v & 1;
				v >>= 1;
			}
		}
	}
	/* bits past the end are set by format; don't count them */
	uint64_t pad = fs->sb.bitmap_blocks * BITS_PER_BLOCK - fs->sb.total_blocks;
	used -= pad;
	fs->free_blocks = fs->sb.total_blocks - used;
	fs->alloc_hint = fs->sb.data_start;
	*out = fs;
	return 0;
}

void bfs_unmount(struct bfs *fs)
{
	if (!fs)
		return;
	for (int i = 0; i < CACHE_N; i++)
		if (fs->cache[i].data)
			fs_free(fs->cache[i].data);
	if (fs->zero)
		fs_free(fs->zero);
	fs_free(fs);
}

int bfs_replayed(struct bfs *fs) { return fs->replayed; }

static void fill_stat(uint32_t ino, struct bfs_inode *in, const char *sn, struct bfs_stat *st)
{
	st->ino = ino;
	st->type = in->mode;
	st->attr = in->attr;
	st->size = in->mode == BFS_DIR ? 0 : in->size;
	st->ctime = in->ctime;
	st->mtime = in->mtime;
	if (sn)
		short_format(sn, st->short_name);
	else
		st->short_name[0] = 0;
}

int bfs_lookup(struct bfs *fs, uint32_t dir, const char *name, uint32_t *ino)
{
	struct bfs_dirent de;
	int r = dir_find(fs, dir, name, &de, NULL);
	if (r)
		return r;
	*ino = de.ino;
	return 0;
}

int bfs_lookup_name(struct bfs *fs, uint32_t dir, const char *name, uint32_t *ino,
    char *canon)
{
	struct bfs_dirent de;
	int r = dir_find(fs, dir, name, &de, NULL);
	if (r)
		return r;
	*ino = de.ino;
	memcpy(canon, de.name, de.name_len);
	canon[de.name_len] = 0;
	return 0;
}

int bfs_stat(struct bfs *fs, uint32_t ino, struct bfs_stat *st)
{
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (r)
		return r;
	if (in.mode == BFS_FREE)
		return BFS_ENOENT;
	fill_stat(ino, &in, NULL, st);
	return 0;
}

long bfs_read(struct bfs *fs, uint32_t ino, uint64_t off, void *buf, size_t n)
{
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (r)
		return r;
	if (in.mode != BFS_FILE)
		return in.mode == BFS_DIR ? BFS_EISDIR : BFS_ENOENT;
	if (off >= in.size)
		return 0;
	if (n > in.size - off)
		n = (size_t)(in.size - off);

	uint8_t *out = buf, *tmp = NULL;
	size_t done = 0;
	while (done < n) {
		uint64_t pos = off + done;
		uint64_t fb = pos / BFS_BLOCK, within = pos % BFS_BLOCK;
		uint64_t disk, run;
		if ((r = map(fs, &in, fb, &disk, &run)))
			break;
		if (!within && n - done >= BFS_BLOCK) {
			/* whole blocks straight into the caller's buffer */
			uint64_t cnt = MIN(run, (n - done) / BFS_BLOCK);
			if (cnt > 64)
				cnt = 64;
			if ((r = dev_read(fs, disk, (uint32_t)cnt, out + done)))
				break;
			done += cnt * BFS_BLOCK;
			continue;
		}
		if (!tmp && !(tmp = fs_malloc(BFS_BLOCK))) {
			r = BFS_ENOMEM;
			break;
		}
		if ((r = dev_read(fs, disk, 1, tmp)))
			break;
		size_t take = MIN((size_t)(BFS_BLOCK - within), n - done);
		memcpy(out + done, tmp + within, take);
		done += take;
	}
	if (tmp)
		fs_free(tmp);
	return r ? r : (long)done;
}

long bfs_write(struct bfs *fs, uint32_t ino, uint64_t off, const void *buf, size_t n)
{
	const uint8_t *src = buf;
	uint8_t *tmp = fs_malloc(BFS_BLOCK);
	if (!tmp)
		return BFS_ENOMEM;
	size_t done = 0;
	int r = 0;

	while (done < n || (!n && !done)) {
		txn_begin(fs);
		struct bfs_inode in;
		r = inode_read(fs, ino, &in);
		if (!r && in.mode != BFS_FILE)
			r = in.mode == BFS_DIR ? BFS_EISDIR : BFS_ENOENT;
		if (!r && (in.attr & BFS_ATTR_RO))
			r = BFS_EACCES;
		if (r) {
			txn_end(fs, r);
			break;
		}
		/* Work in chunks of up to 4 MiB so one transaction stays small. */
		size_t chunk = MIN(n - done, (size_t)4 << 20);
		uint64_t start = off + done, end = start + chunk;
		uint64_t have = nblocks_of(in.size), need = nblocks_of(end);
		uint64_t old_size = in.size;
		if (need > have && (r = grow(fs, &in, need - have))) {
			txn_end(fs, r);
			break;
		}
		/* gap between the old end and the write: zero it */
		for (uint64_t fb = have; fb < start / BFS_BLOCK; fb++) {
			uint64_t disk, run;
			if ((r = map(fs, &in, fb, &disk, &run)) ||
			    (r = dev_write(fs, disk, 1, fs->zero)))
				break;
		}
		if (old_size % BFS_BLOCK && start > old_size && have > 0) {
			/* tail of the old last block past EOF must read as zero */
			uint64_t disk, run;
			if (!(r = map(fs, &in, have - 1, &disk, &run)) &&
			    !(r = dev_read(fs, disk, 1, tmp))) {
				memset(tmp + old_size % BFS_BLOCK, 0, BFS_BLOCK - old_size % BFS_BLOCK);
				r = dev_write(fs, disk, 1, tmp);
			}
		}
		size_t pos = 0;
		while (!r && pos < chunk) {
			uint64_t p = start + pos;
			uint64_t fb = p / BFS_BLOCK, within = p % BFS_BLOCK;
			uint64_t disk, run;
			if ((r = map(fs, &in, fb, &disk, &run)))
				break;
			if (!within && chunk - pos >= BFS_BLOCK) {
				uint64_t cnt = MIN(run, (chunk - pos) / BFS_BLOCK);
				if (cnt > 64)
					cnt = 64;
				r = dev_write(fs, disk, (uint32_t)cnt, src + done + pos);
				pos += cnt * BFS_BLOCK;
				continue;
			}
			/* partial block: read-modify-write (fresh blocks start as zero) */
			if (fb < have && (r = dev_read(fs, disk, 1, tmp)))
				break;
			if (fb >= have)
				memset(tmp, 0, BFS_BLOCK);
			size_t take = MIN((size_t)(BFS_BLOCK - within), chunk - pos);
			memcpy(tmp + within, src + done + pos, take);
			r = dev_write(fs, disk, 1, tmp);
			pos += take;
		}
		if (!r) {
			extern uint64_t bfs_now(void);
			if (end > in.size)
				in.size = end;
			in.attr |= BFS_ATTR_ARCHIVE;
			in.mtime = bfs_now();
			r = inode_write(fs, ino, &in);
		}
		r = txn_end(fs, r);
		if (r)
			break;
		done += chunk;
		if (!n)
			break;
	}
	fs_free(tmp);
	return r ? r : (long)done;
}

int bfs_truncate(struct bfs *fs, uint32_t ino, uint64_t size)
{
	txn_begin(fs);
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (!r && in.mode != BFS_FILE)
		r = BFS_EISDIR;
	if (!r && (in.attr & BFS_ATTR_RO))
		r = BFS_EACCES;
	if (!r) {
		if (size < in.size) {
			r = shrink(fs, &in, nblocks_of(size));
			in.size = size;
		} else if (size > in.size) {
			txn_end(fs, 0);
			/* extending: write zeros through the normal path */
			uint8_t z = 0;
			long w = size ? bfs_write(fs, ino, size - 1, &z, 1) : 0;
			return w < 0 ? (int)w : 0;
		}
		in.attr |= BFS_ATTR_ARCHIVE;
		if (!r)
			r = inode_write(fs, ino, &in);
	}
	return txn_end(fs, r);
}

int bfs_create(struct bfs *fs, uint32_t dir, const char *name, int type, uint32_t *out)
{
	int r = valid_name(name);
	if (r)
		return r;
	if (type != BFS_FILE && type != BFS_DIR)
		return BFS_EINVAL;
	txn_begin(fs);
	struct bfs_inode din;
	if ((r = inode_read(fs, dir, &din)))
		return txn_end(fs, r);
	if (din.mode != BFS_DIR)
		return txn_end(fs, BFS_ENOTDIR);
	if (!dir_find(fs, dir, name, NULL, NULL))
		return txn_end(fs, BFS_EEXIST);

	uint32_t ino;
	if ((r = inode_alloc(fs, &ino)))
		return txn_end(fs, r);
	struct bfs_inode in;
	memset(&in, 0, sizeof in);
	in.mode = (uint8_t)type;
	in.attr = type == BFS_DIR ? BFS_ATTR_DIR : BFS_ATTR_ARCHIVE;
	in.parent = dir;
	extern uint64_t bfs_now(void);
	in.ctime = in.mtime = bfs_now();
	if (type == BFS_DIR) {
		if ((r = grow(fs, &in, 1)))
			return txn_end(fs, r);
		if (!bget(fs, in.ext[0].start, 1) || (r = bdirty(fs, in.ext[0].start)))
			return txn_end(fs, r ? r : BFS_EIO);
		in.size = BFS_BLOCK;
	}
	if ((r = inode_write(fs, ino, &in)))
		return txn_end(fs, r);

	struct bfs_dirent de;
	memset(&de, 0, sizeof de);
	de.ino = ino;
	de.hash = name_hash(name);
	de.name_len = (uint8_t)strlen(name);
	de.type = (uint8_t)type;
	memcpy(de.name, name, de.name_len);
	if ((r = make_short(fs, dir, name, de.short_name)) || (r = dir_add(fs, dir, &de)))
		return txn_end(fs, r);
	if (!(r = inode_read(fs, dir, &din))) {
		din.mtime = in.mtime;
		r = inode_write(fs, dir, &din);
	}
	if (!r && out)
		*out = ino;
	return txn_end(fs, r);
}

int bfs_unlink(struct bfs *fs, uint32_t dir, const char *name)
{
	txn_begin(fs);
	struct bfs_dirent de;
	uint64_t slot;
	int r = dir_find(fs, dir, name, &de, &slot);
	if (r)
		return txn_end(fs, r);
	if (de.ino == BFS_ROOT)
		return txn_end(fs, BFS_EACCES);
	struct bfs_inode in;
	if ((r = inode_read(fs, de.ino, &in)))
		return txn_end(fs, r);
	if (in.attr & BFS_ATTR_RO)
		return txn_end(fs, BFS_EACCES);
	if (in.mode == BFS_DIR && dir_walk(fs, de.ino, empty_fn, NULL) == 1)
		return txn_end(fs, BFS_ENOTEMPTY);
	if ((r = shrink(fs, &in, 0)))
		return txn_end(fs, r);
	memset(&in, 0, sizeof in);	/* mode = FREE */
	if ((r = inode_write(fs, de.ino, &in)) || (r = dir_clear_slot(fs, dir, slot)))
		return txn_end(fs, r);
	struct bfs_inode din;
	if (!(r = inode_read(fs, dir, &din))) {
		extern uint64_t bfs_now(void);
		din.mtime = bfs_now();
		r = inode_write(fs, dir, &din);
	}
	return txn_end(fs, r);
}

int bfs_rename(struct bfs *fs, uint32_t sdir, const char *sname,
    uint32_t ddir, const char *dname)
{
	int r = valid_name(dname);
	if (r)
		return r;
	txn_begin(fs);
	struct bfs_dirent de, other;
	uint64_t slot, oslot;
	if ((r = dir_find(fs, sdir, sname, &de, &slot)))
		return txn_end(fs, r);
	struct bfs_inode dd;
	if ((r = inode_read(fs, ddir, &dd)))
		return txn_end(fs, r);
	if (dd.mode != BFS_DIR)
		return txn_end(fs, BFS_ENOTDIR);
	if (!dir_find(fs, ddir, dname, &other, &oslot) && other.ino != de.ino)
		return txn_end(fs, BFS_EEXIST);

	/* A directory can't move inside itself. */
	if (de.type == BFS_DIR) {
		uint32_t p = ddir;
		for (int depth = 0; depth < 256; depth++) {
			if (p == de.ino)
				return txn_end(fs, BFS_EINVAL);
			if (p == BFS_ROOT)
				break;
			struct bfs_inode pi;
			if ((r = inode_read(fs, p, &pi)))
				return txn_end(fs, r);
			p = pi.parent;
		}
	}
	if ((r = dir_clear_slot(fs, sdir, slot)))
		return txn_end(fs, r);
	de.hash = name_hash(dname);
	de.name_len = (uint8_t)strlen(dname);
	memset(de.name, 0, sizeof de.name);
	memcpy(de.name, dname, de.name_len);
	if ((r = make_short(fs, ddir, dname, de.short_name)) || (r = dir_add(fs, ddir, &de)))
		return txn_end(fs, r);
	struct bfs_inode in;
	if (!(r = inode_read(fs, de.ino, &in)) && in.parent != ddir) {
		in.parent = ddir;
		r = inode_write(fs, de.ino, &in);
	}
	return txn_end(fs, r);
}

int bfs_setattr(struct bfs *fs, uint32_t ino, uint8_t attr)
{
	txn_begin(fs);
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (!r) {
		in.attr = (uint8_t)((attr & ~BFS_ATTR_DIR) | (in.mode == BFS_DIR ? BFS_ATTR_DIR : 0));
		r = inode_write(fs, ino, &in);
	}
	return txn_end(fs, r);
}

int bfs_touch(struct bfs *fs, uint32_t ino, uint64_t mtime)
{
	txn_begin(fs);
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (!r) {
		in.mtime = mtime;
		r = inode_write(fs, ino, &in);
	}
	return txn_end(fs, r);
}

int bfs_readdir(struct bfs *fs, uint32_t dir, uint64_t *cookie, struct bfs_listent *e)
{
	struct bfs_inode in;
	int r = inode_read(fs, dir, &in);
	if (r)
		return r;
	if (in.mode != BFS_DIR)
		return BFS_ENOTDIR;
	uint64_t total = nblocks_of(in.size) * DIRENTS_PER_BLOCK;
	while (*cookie < total) {
		uint64_t slot = (*cookie)++;
		uint64_t disk, run;
		if ((r = map(fs, &in, slot / DIRENTS_PER_BLOCK, &disk, &run)))
			return r;
		uint8_t *b = bget(fs, disk, 0);
		if (!b)
			return BFS_EIO;
		struct bfs_dirent de;
		memcpy(&de, b + (slot % DIRENTS_PER_BLOCK) * 256, 256);
		if (!de.ino)
			continue;
		struct bfs_inode ci;
		if ((r = inode_read(fs, de.ino, &ci)))
			return r;
		memcpy(e->name, de.name, de.name_len);
		e->name[de.name_len] = 0;
		fill_stat(de.ino, &ci, de.short_name, &e->st);
		return 1;
	}
	return 0;
}

int bfs_xattr_get(struct bfs *fs, uint32_t ino, const char *key, char *val, size_t n)
{
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (r)
		return r;
	size_t kl = strlen(key);
	for (size_t p = 0; p + 2 <= in.xattr_len; ) {
		size_t klen = in.xattr[p], vlen = in.xattr[p + 1];
		if (klen == kl && !memcmp(in.xattr + p + 2, key, kl)) {
			size_t c = MIN(vlen, n - 1);
			memcpy(val, in.xattr + p + 2 + klen, c);
			val[c] = 0;
			return (int)vlen;
		}
		p += 2 + klen + vlen;
	}
	return BFS_ENOENT;
}

int bfs_xattr_set(struct bfs *fs, uint32_t ino, const char *key, const char *val)
{
	txn_begin(fs);
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (r)
		return txn_end(fs, r);
	uint8_t nx[BFS_XATTR_SPACE];
	size_t o = 0, kl = strlen(key), vl = val ? strlen(val) : 0;
	for (size_t p = 0; p + 2 <= in.xattr_len; ) {
		size_t klen = in.xattr[p], vlen = in.xattr[p + 1];
		if (!(klen == kl && !memcmp(in.xattr + p + 2, key, kl))) {
			memcpy(nx + o, in.xattr + p, 2 + klen + vlen);
			o += 2 + klen + vlen;
		}
		p += 2 + klen + vlen;
	}
	if (val) {
		if (kl > 255 || vl > 255 || o + 2 + kl + vl > BFS_XATTR_SPACE)
			return txn_end(fs, BFS_ENOSPC);
		nx[o] = (uint8_t)kl;
		nx[o + 1] = (uint8_t)vl;
		memcpy(nx + o + 2, key, kl);
		memcpy(nx + o + 2 + kl, val, vl);
		o += 2 + kl + vl;
	}
	memset(in.xattr, 0, sizeof in.xattr);
	memcpy(in.xattr, nx, o);
	in.xattr_len = (uint16_t)o;
	return txn_end(fs, inode_write(fs, ino, &in));
}

uint64_t bfs_free_bytes(struct bfs *fs) { return fs->free_blocks * BFS_BLOCK; }
uint64_t bfs_total_bytes(struct bfs *fs)
{
	return (fs->sb.total_blocks - fs->sb.data_start) * BFS_BLOCK;
}
const char *bfs_label(struct bfs *fs) { return fs->sb.label; }

int bfs_set_label(struct bfs *fs, const char *label)
{
	memset(fs->sb.label, 0, sizeof fs->sb.label);
	for (int i = 0; label[i] && i < 31; i++)
		fs->sb.label[i] = (char)fold((unsigned char)label[i]);
	return super_write(fs);
}

/* ---- CHKDSK ------------------------------------------------------------- */

struct check {
	struct bfs *fs;
	uint8_t *seen_blk;	/* blocks reached from the tree */
	uint8_t *seen_ino;
	void (*out)(const char *);
	uint64_t files, dirs, bytes, problems;
	char msg[160];
};

static void cmark(struct check *c, uint64_t b, uint32_t ino)
{
	if (b >= c->fs->sb.total_blocks) {
		snprintf(c->msg, sizeof c->msg, "Inode %u points past the end of the disk", ino);
		c->out(c->msg);
		c->problems++;
		return;
	}
	if (c->seen_blk[b / 8] & (1 << (b % 8))) {
		snprintf(c->msg, sizeof c->msg, "Block %llu is cross-linked (inode %u)",
		    (unsigned long long)b, ino);
		c->out(c->msg);
		c->problems++;
	}
	c->seen_blk[b / 8] |= (uint8_t)(1 << (b % 8));
}

static int check_inode(struct check *c, uint32_t ino, int depth);

static int check_dir_fn(struct bfs *fs, struct bfs_dirent *de, uint64_t slot, void *arg)
{
	(void)fs;
	(void)slot;
	struct check *c = arg;
	check_inode(c, de->ino, 1);
	return 0;
}

static int check_inode(struct check *c, uint32_t ino, int depth)
{
	struct bfs *fs = c->fs;
	if (depth > 64 || !ino || ino > fs->sb.inode_count)
		return 0;
	if (c->seen_ino[ino / 8] & (1 << (ino % 8))) {
		snprintf(c->msg, sizeof c->msg, "Inode %u is linked twice", ino);
		c->out(c->msg);
		c->problems++;
		return 0;
	}
	c->seen_ino[ino / 8] |= (uint8_t)(1 << (ino % 8));
	struct bfs_inode in;
	int r = inode_read(fs, ino, &in);
	if (r || in.mode == BFS_FREE) {
		snprintf(c->msg, sizeof c->msg, "Inode %u is %s", ino,
		    r ? "damaged (bad checksum)" : "free but still linked");
		c->out(c->msg);
		c->problems++;
		return 0;
	}
	uint64_t nb = 0;
	for (int i = 0; i < in.nextents; i++) {
		struct bfs_extent e;
		if (ext_get(fs, &in, i, &e))
			break;
		for (uint64_t k = 0; k < e.len; k++)
			cmark(c, e.start + k, ino);
		nb += e.len;
	}
	if (in.ext_block)
		cmark(c, in.ext_block, ino);
	if (nb != nblocks_of(in.size)) {
		snprintf(c->msg, sizeof c->msg, "Inode %u: size and allocation disagree", ino);
		c->out(c->msg);
		c->problems++;
	}
	if (in.mode == BFS_DIR) {
		c->dirs++;
		dir_walk(fs, ino, check_dir_fn, c);
	} else {
		c->files++;
		c->bytes += in.size;
	}
	return 0;
}

int bfs_check(struct bfs *fs, int fix, void (*out)(const char *msg))
{
	struct check c;
	memset(&c, 0, sizeof c);
	c.fs = fs;
	c.out = out;
	uint64_t bm = (fs->sb.total_blocks + 7) / 8, im = (fs->sb.inode_count + 8) / 8;
	c.seen_blk = fs_malloc(bm);
	c.seen_ino = fs_malloc(im);
	if (!c.seen_blk || !c.seen_ino) {
		fs_free(c.seen_blk);
		fs_free(c.seen_ino);
		return BFS_ENOMEM;
	}
	memset(c.seen_blk, 0, bm);
	memset(c.seen_ino, 0, im);
	for (uint64_t b = 0; b < fs->sb.data_start; b++)
		c.seen_blk[b / 8] |= (uint8_t)(1 << (b % 8));

	check_inode(&c, BFS_ROOT, 0);

	/* orphans: allocated inodes nobody links to */
	uint64_t orphans = 0;
	for (uint32_t ino = 1; ino <= fs->sb.inode_count; ino++) {
		uint8_t *b = bget(fs, fs->sb.inode_start + (ino - 1) / INODES_PER_BLOCK, 0);
		if (!b)
			break;
		if (b[((ino - 1) % INODES_PER_BLOCK) * 256] != BFS_FREE &&
		    !(c.seen_ino[ino / 8] & (1 << (ino % 8)))) {
			orphans++;
			if (fix) {
				struct bfs_inode z;
				memset(&z, 0, sizeof z);
				txn_begin(fs);
				inode_write(fs, ino, &z);
				txn_end(fs, 0);
			}
		}
	}
	if (orphans) {
		snprintf(c.msg, sizeof c.msg, "%llu lost inode(s)%s",
		    (unsigned long long)orphans, fix ? " freed" : "");
		out(c.msg);
		c.problems += orphans;
	}

	/* compare the bitmap with what the tree actually uses */
	uint64_t lost = 0, missing = 0;
	for (uint64_t b = 0; b < fs->sb.total_blocks; b++) {
		int on;
		if (bit_get(fs, b, &on))
			break;
		int used = (c.seen_blk[b / 8] >> (b % 8)) & 1;
		if (on && !used)
			lost++;
		if (!on && used)
			missing++;
		if (fix && on != used) {
			txn_begin(fs);
			txn_room(fs, 2);
			bit_set(fs, b, used);
			txn_end(fs, 0);
		}
	}
	if (lost) {
		snprintf(c.msg, sizeof c.msg, "%llu lost cluster(s)%s",
		    (unsigned long long)lost, fix ? " recovered" : " found");
		out(c.msg);
		c.problems++;
	}
	if (missing) {
		snprintf(c.msg, sizeof c.msg, "%llu block(s) in use but marked free%s",
		    (unsigned long long)missing, fix ? " - fixed" : "");
		out(c.msg);
		c.problems++;
	}
	if (fix) {
		fs->free_blocks += lost;
		fs->free_blocks -= missing;
	}

	snprintf(c.msg, sizeof c.msg, "%llu file(s), %llu director%s, %llu bytes in files",
	    (unsigned long long)c.files, (unsigned long long)c.dirs,
	    c.dirs == 1 ? "y" : "ies", (unsigned long long)c.bytes);
	out(c.msg);
	fs_free(c.seen_blk);
	fs_free(c.seen_ino);
	return (int)MIN(c.problems, 0x7FFFFFFF);
}
