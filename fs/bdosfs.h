/*
 * bdosfs.h - the bdos filesystem. Shared by the kernel and the host tools.
 *
 * On disk (little endian, 4 KiB blocks):
 *
 *   block 0, 1      superblock A and B (newest valid generation wins)
 *   journal         one metadata transaction at a time (ordered mode)
 *   bitmap          one bit per block, 1 = in use
 *   inode table     256-byte inodes, 16 per block, inode 1 = root
 *   data            file contents and directory blocks
 *
 * Every metadata change (bitmap, inodes, directories, extent blocks) goes
 * through the journal: logged, committed with a CRC32C, written home, then
 * the superblock records it as checkpointed. File data is written before
 * the metadata that points at it, so a crash never exposes stale blocks.
 * Inodes and the superblock carry CRC32C checksums.
 */
#ifndef BDOSFS_H
#define BDOSFS_H

#include <stdint.h>
#include <stddef.h>

#define BFS_BLOCK	4096
#define BFS_MAGIC	0x31305346534F4442ULL	/* "BDOSFS01" */
#define BFS_JDESC	0x4353454400444A42ULL	/* "BJD\0DESC" */
#define BFS_JCOMMIT	0x544D4D4F43444A42ULL	/* "BJDCOMMT" */
#define BFS_ROOT	1
#define BFS_NAME_MAX	233
#define BFS_INLINE_EXT	8
#define BFS_EXT_PER_BLOCK (BFS_BLOCK / 16)
#define BFS_XATTR_SPACE	72
#define BFS_TXN_MAX	96	/* metadata blocks per transaction */

/* inode types */
enum { BFS_FREE = 0, BFS_FILE = 1, BFS_DIR = 2 };

/* DOS attribute bits, stored as-is */
#define BFS_ATTR_RO	0x01
#define BFS_ATTR_HIDDEN	0x02
#define BFS_ATTR_SYSTEM	0x04
#define BFS_ATTR_DIR	0x10
#define BFS_ATTR_ARCHIVE 0x20

/* errors (negative returns) */
enum {
	BFS_OK = 0,
	BFS_ENOENT = -2,
	BFS_EEXIST = -3,
	BFS_ENOTDIR = -4,
	BFS_EISDIR = -5,
	BFS_ENOSPC = -6,
	BFS_ENOTEMPTY = -7,
	BFS_EINVAL = -8,
	BFS_EIO = -9,
	BFS_ECORRUPT = -10,
	BFS_ENAMETOOLONG = -11,
	BFS_EACCES = -12,
	BFS_ENOMEM = -13,
};

struct bfs_super {
	uint64_t magic;
	uint32_t version;
	uint32_t crc;			/* over the whole struct with crc = 0 */
	uint64_t generation;
	uint64_t total_blocks;
	uint64_t journal_start, journal_blocks;
	uint64_t bitmap_start, bitmap_blocks;
	uint64_t inode_start, inode_blocks, inode_count;
	uint64_t data_start;
	uint64_t checkpoint_seq;	/* last journal transaction applied */
	uint64_t features;		/* reserved for future format additions */
	uint64_t created;
	char label[32];
	uint8_t reserved[256];
} __attribute__((packed));

struct bfs_extent {
	uint64_t start;
	uint64_t len;
} __attribute__((packed));

struct bfs_inode {
	uint8_t mode;			/* BFS_FREE / BFS_FILE / BFS_DIR */
	uint8_t attr;
	uint16_t nextents;
	uint32_t crc;
	uint64_t size;
	uint64_t ctime, mtime, atime;
	uint64_t ext_block;		/* extents 8.. live here */
	uint32_t parent;
	uint16_t xattr_len;
	uint16_t _pad;
	struct bfs_extent ext[BFS_INLINE_EXT];
	uint8_t xattr[BFS_XATTR_SPACE];
} __attribute__((packed));

struct bfs_dirent {			/* 256 bytes, 16 per block */
	uint32_t ino;			/* 0 = empty slot */
	uint32_t hash;			/* crc32c of the upper-cased name */
	uint8_t name_len;
	uint8_t type;
	char short_name[11];		/* 8.3 alias, space padded, no dot */
	uint8_t _pad;
	char name[BFS_NAME_MAX + 1];
} __attribute__((packed));

_Static_assert(sizeof(struct bfs_inode) == 256, "inode size");
_Static_assert(sizeof(struct bfs_dirent) == 256, "dirent size");
_Static_assert(sizeof(struct bfs_super) <= BFS_BLOCK, "super size");

/* The block device underneath, in 4 KiB blocks. */
struct bfs_dev {
	int (*read)(void *ctx, uint64_t blk, uint32_t n, void *buf);
	int (*write)(void *ctx, uint64_t blk, uint32_t n, const void *buf);
	int (*flush)(void *ctx);
	void *ctx;
	uint64_t nblocks;
};

struct bfs_stat {
	uint32_t ino;
	int type;
	uint8_t attr;
	uint64_t size;
	uint64_t ctime, mtime;
	char short_name[13];		/* "README.TXT" form */
};

struct bfs_listent {
	char name[BFS_NAME_MAX + 1];
	struct bfs_stat st;
};

struct bfs;	/* opaque mounted filesystem */

int bfs_format(struct bfs_dev *dev, const char *label, uint64_t now);
int bfs_mount(struct bfs_dev *dev, struct bfs **out);
void bfs_unmount(struct bfs *fs);

/* Path-free core: operate on inode numbers. */
int bfs_lookup(struct bfs *fs, uint32_t dir, const char *name, uint32_t *ino);
/* Same, also returning the name as stored on disk (canon: BFS_NAME_MAX + 1). */
int bfs_lookup_name(struct bfs *fs, uint32_t dir, const char *name, uint32_t *ino,
    char *canon);
int bfs_stat(struct bfs *fs, uint32_t ino, struct bfs_stat *st);
long bfs_read(struct bfs *fs, uint32_t ino, uint64_t off, void *buf, size_t n);
long bfs_write(struct bfs *fs, uint32_t ino, uint64_t off, const void *buf, size_t n);
int bfs_truncate(struct bfs *fs, uint32_t ino, uint64_t size);
int bfs_create(struct bfs *fs, uint32_t dir, const char *name, int type, uint32_t *ino);
int bfs_unlink(struct bfs *fs, uint32_t dir, const char *name);
int bfs_rename(struct bfs *fs, uint32_t sdir, const char *sname,
    uint32_t ddir, const char *dname);
int bfs_setattr(struct bfs *fs, uint32_t ino, uint8_t attr);
int bfs_touch(struct bfs *fs, uint32_t ino, uint64_t mtime);
/* Iterate a directory: *cookie starts at 0, returns 1 per entry, 0 at end. */
int bfs_readdir(struct bfs *fs, uint32_t dir, uint64_t *cookie, struct bfs_listent *e);
int bfs_xattr_get(struct bfs *fs, uint32_t ino, const char *key, char *val, size_t n);
int bfs_xattr_set(struct bfs *fs, uint32_t ino, const char *key, const char *val);

/* Volume info and maintenance */
uint64_t bfs_free_bytes(struct bfs *fs);
uint64_t bfs_total_bytes(struct bfs *fs);
const char *bfs_label(struct bfs *fs);
int bfs_set_label(struct bfs *fs, const char *label);
int bfs_replayed(struct bfs *fs);	/* journal transactions replayed at mount */
/* Consistency check; fix != 0 rebuilds the bitmap. Reports via out(). */
int bfs_check(struct bfs *fs, int fix, void (*out)(const char *msg));

const char *bfs_strerror(int err);

#endif
