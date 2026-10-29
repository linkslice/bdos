/*
 * vfs.h - drive letters, paths and DOS devices.
 *
 * Paths handed to the VFS are absolute: "C:\GAMES\LANDER.BAS". Either
 * separator works everywhere; the shell decides whether a leading '/'
 * is a switch or a path before it gets here.
 */
#ifndef BDOS_VFS_H
#define BDOS_VFS_H

#include <stdint.h>
#include <stddef.h>

#define VFS_PATH_MAX	256
#define VFS_NAME_MAX	234

enum { VFS_FILE = 1, VFS_DIR = 2, VFS_DEV = 3 };

/* attribute bits (the DOS ones) */
#define A_RO		0x01
#define A_HIDDEN	0x02
#define A_SYSTEM	0x04
#define A_DIR		0x10
#define A_ARCHIVE	0x20

/* errors: the bdosfs codes, shared by every drive */
enum {
	E_NOENT = -2, E_EXIST = -3, E_NOTDIR = -4, E_ISDIR = -5, E_NOSPC = -6,
	E_NOTEMPTY = -7, E_INVAL = -8, E_IO = -9, E_CORRUPT = -10,
	E_NAMETOOLONG = -11, E_ACCES = -12, E_NOMEM = -13,
	E_BADDRIVE = -20, E_NOTSUP = -21, E_XDEV = -22,
	E_BADF = -23, E_NFILE = -24, E_NOSYS = -25, E_TIMEDOUT = -26,
	E_REFUSED = -27, E_RESET = -28, E_NOHOST = -29, E_NONET = -30,
	E_NOTREADY = -31,	/* a CD drive with no disc in it */
};

struct vstat {
	int type;
	uint8_t attr;
	uint64_t size;
	uint64_t mtime;			/* seconds since 1970, local time */
	char short_name[13];
	char name[VFS_NAME_MAX];
};

/* What a drive implements. Paths are relative to the drive root, '/' separated. */
struct vfs_ops {
	int (*stat)(void *ctx, const char *path, struct vstat *st);
	int (*list)(void *ctx, const char *path,
	    int (*cb)(void *arg, struct vstat *st), void *arg);
	long (*read)(void *ctx, const char *path, uint64_t off, void *buf, size_t n);
	long (*write)(void *ctx, const char *path, uint64_t off, const void *buf, size_t n);
	int (*create)(void *ctx, const char *path, int type);
	int (*truncate)(void *ctx, const char *path, uint64_t size);
	int (*remove)(void *ctx, const char *path);
	int (*rename)(void *ctx, const char *from, const char *to);
	int (*setattr)(void *ctx, const char *path, uint8_t attr);
	int (*xattr_get)(void *ctx, const char *path, const char *key, char *val, size_t n);
	int (*xattr_set)(void *ctx, const char *path, const char *key, const char *val);
	int (*info)(void *ctx, char *label, size_t n, uint64_t *free, uint64_t *total);
	int (*check)(void *ctx, int fix, void (*out)(const char *));
	int (*set_label)(void *ctx, const char *label);
};

struct vfs_drive {
	char letter;
	const struct vfs_ops *ops;
	void *ctx;
	char desc[48];			/* "bdosfs on disk0 partition 2" */
};

/* Character devices: CON, NUL, COM1, DESK$, ... */
struct chardev {
	const char *name;
	long (*read)(struct chardev *, void *buf, size_t n);	/* non-blocking */
	long (*write)(struct chardev *, const void *buf, size_t n);
	void *priv;
	int once;	/* a status device: one read per open, then end-of-file */
};

struct vfile;

const char *vfs_unmounted(int i);		/* partitions that wouldn't mount, and why */
void *vfs_session_new(void);			/* a session's own drives */
void vfs_session_free(void *s);
int vfs_session_mount(char letter, struct vfs_drive *d);	/* in the caller's session; NULL: away */
int vfs_mount(char letter, const struct vfs_ops *ops, void *ctx, const char *desc);
int vfs_unmount(char letter);
/* SUBST X: target (NULL: undo it); what X: stands for, or NULL */
int vfs_subst(char letter, const char *target);
const char *vfs_subst_target(char letter);
/* bdosfs volumes the installer makes */
struct blkdev;
int vfs_mount_bdos(char letter, struct blkdev *b, uint64_t start, uint64_t sectors, const char *what);
int vfs_format_bdos(struct blkdev *b, uint64_t start, uint64_t sectors, const char *label);
int vfs_eject(char letter);
struct blkdev *vfs_drive_disk(char letter);
struct vfs_drive *vfs_drive(char letter);
void vfs_drives(void (*cb)(struct vfs_drive *d, void *arg), void *arg);
void dev_register(struct chardev *d);
struct chardev *dev_find(const char *name);

/* Turn what the user typed into an absolute path ("C:\A\B"). */
int vfs_resolve(const char *cwd, const char *in, char *out, size_t n);
/* Same path with the on-disk spelling of every component. */
int vfs_canon(const char *abs, char *out, size_t n);
const char *vfs_basename(const char *abs);
void vfs_dirname(const char *abs, char *out, size_t n);
int vfs_join(const char *dir, const char *name, char *out, size_t n);

int vfs_stat(const char *abs, struct vstat *st);
int vfs_list(const char *absdir, int (*cb)(void *arg, struct vstat *st), void *arg);
int vfs_mkdir(const char *abs);
int vfs_rmdir(const char *abs);
int vfs_delete(const char *abs);
int vfs_rename(const char *from, const char *to);
int vfs_setattr(const char *abs, uint8_t attr);
int vfs_xattr_get(const char *abs, const char *key, char *val, size_t n);
int vfs_xattr_set(const char *abs, const char *key, const char *val);
int vfs_info(char letter, char *label, size_t n, uint64_t *free, uint64_t *total);
int vfs_check(char letter, int fix, void (*out)(const char *));
int vfs_set_label(char letter, const char *label);

/* Open files */
#define O_READ		0x01
#define O_WRITE		0x02
#define O_CREATE	0x04
#define O_TRUNC		0x08
#define O_APPEND	0x10

struct vfile *vfs_open(const char *abs, int flags, int *err);
long vfs_read(struct vfile *f, void *buf, size_t n);
long vfs_write(struct vfile *f, const void *buf, size_t n);
int vfs_seek(struct vfile *f, uint64_t pos);
uint64_t vfs_tell(struct vfile *f);
uint64_t vfs_size(struct vfile *f);
int vfs_isdev(struct vfile *f);
void vfs_close(struct vfile *f);

/* Read a whole file into a fresh kmalloc'd buffer (NUL-terminated). */
char *vfs_slurp(const char *abs, size_t *len, int *err);
int vfs_spew(const char *abs, const void *data, size_t len);

const char *vfs_strerror(int err);

/* time helpers */
uint64_t rtc_epoch(void);
void epoch_to_date(uint64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s);

/* mount everything found at boot */
void storage_init(void);

#endif
