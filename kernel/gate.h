/*
 * gate.h - for code that runs without the big kernel lock.
 *
 * The interpreters and the DOS emulator let go of the BKL while they
 * compute, so they can run on any core alongside everything else. Every
 * kernel service they call goes through one of these wrappers, which
 * takes the lock for the length of the call. Include this last: it
 * renames the calls in the file that includes it.
 *
 * Calls that are safe without the lock aren't wrapped: kmalloc and friends
 * (they have their own lock), the string and math helpers, uptime_ms,
 * task_yield, task_idle, sleep_ms and input_key.
 */
#ifndef BDOS_GATE_H
#define BDOS_GATE_H

#include "kernel.h"
#include "vfs.h"
#include "sysapi.h"
#include "settings.h"

static inline void gate_con_attr(int fg, int bg)
{
	bkl_enter();
	con_attr(fg, bg);
	bkl_leave();
}

static inline void gate_con_clear(void)
{
	bkl_enter();
	con_clear();
	bkl_leave();
}

static inline int gate_con_getkey(void)
{
	bkl_enter();
	int r_ = con_getkey();
	bkl_leave();
	return r_;
}

static inline void gate_con_gotoxy(int x, int y)
{
	bkl_enter();
	con_gotoxy(x, y);
	bkl_leave();
}

static inline int gate_con_pollkey(void)
{
	bkl_enter();
	int r_ = con_pollkey();
	bkl_leave();
	return r_;
}

static inline void gate_con_putc(char c)
{
	bkl_enter();
	con_putc(c);
	bkl_leave();
}

static inline void gate_con_write(const char *s)
{
	bkl_enter();
	con_write(s);
	bkl_leave();
}

static inline void gate_con_cursor(int on)
{
	bkl_enter();
	con_cursor(on);
	bkl_leave();
}

static inline void gate_con_size(int *cols, int *rows)
{
	bkl_enter();
	con_size(cols, rows);
	bkl_leave();
}

static inline int gate_read_line(char *buf, size_t cap)
{
	bkl_enter();
	int r_ = read_line(buf, cap);
	bkl_leave();
	return r_;
}

static inline int gate_resolve_path(const char *in, char *out, size_t n)
{
	bkl_enter();
	int r_ = resolve_path(in, out, n);
	bkl_leave();
	return r_;
}

static inline int gate_shell_exec(const char *line)
{
	bkl_enter();
	int r_ = shell_exec(line);
	bkl_leave();
	return r_;
}

static inline uint64_t gate_rtc_epoch(void)
{
	bkl_enter();
	uint64_t r_ = rtc_epoch();
	bkl_leave();
	return r_;
}

static inline void gate_epoch_to_date(uint64_t t, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	bkl_enter();
	epoch_to_date(t, y, mo, d, h, mi, s);
	bkl_leave();
}

static inline void gate_arch_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	bkl_enter();
	arch_rtc(y, mo, d, h, mi, s);
	bkl_leave();
}

static inline int gate_clock_set(int y, int mo, int d, int h, int mi, int s)
{
	bkl_enter();
	int r_ = clock_set(y, mo, d, h, mi, s);
	bkl_leave();
	return r_;
}

static inline int gate_sys_call(const char *name, int argc, struct sysval *argv, struct sysval *ret)
{
	bkl_enter();
	int r_ = sys_call(name, argc, argv, ret);
	bkl_leave();
	return r_;
}

static inline void gate_sys_list(void (*cb)(const struct sys_entry *e, void *arg), void *arg)
{
	bkl_enter();
	sys_list(cb, arg);
	bkl_leave();
}

static inline struct vfs_drive *gate_vfs_drive(char letter)
{
	bkl_enter();
	struct vfs_drive * r_ = vfs_drive(letter);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_canon(const char *abs, char *out, size_t n)
{
	bkl_enter();
	int r_ = vfs_canon(abs, out, n);
	bkl_leave();
	return r_;
}

static inline const char *gate_vfs_basename(const char *abs)
{
	bkl_enter();
	const char * r_ = vfs_basename(abs);
	bkl_leave();
	return r_;
}

static inline void gate_vfs_dirname(const char *abs, char *out, size_t n)
{
	bkl_enter();
	vfs_dirname(abs, out, n);
	bkl_leave();
}

static inline int gate_vfs_stat(const char *abs, struct vstat *st)
{
	bkl_enter();
	int r_ = vfs_stat(abs, st);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_list(const char *absdir, int (*cb)(void *arg, struct vstat *st), void *arg)
{
	bkl_enter();
	int r_ = vfs_list(absdir, cb, arg);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_mkdir(const char *abs)
{
	bkl_enter();
	int r_ = vfs_mkdir(abs);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_rmdir(const char *abs)
{
	bkl_enter();
	int r_ = vfs_rmdir(abs);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_delete(const char *abs)
{
	bkl_enter();
	int r_ = vfs_delete(abs);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_rename(const char *from, const char *to)
{
	bkl_enter();
	int r_ = vfs_rename(from, to);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_setattr(const char *abs, uint8_t attr)
{
	bkl_enter();
	int r_ = vfs_setattr(abs, attr);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_info(char letter, char *label, size_t n, uint64_t *free_, uint64_t *total)
{
	bkl_enter();
	int r_ = vfs_info(letter, label, n, free_, total);
	bkl_leave();
	return r_;
}

static inline struct vfile *gate_vfs_open(const char *abs, int flags, int *err)
{
	bkl_enter();
	struct vfile * r_ = vfs_open(abs, flags, err);
	bkl_leave();
	return r_;
}

static inline long gate_vfs_read(struct vfile *f, void *buf, size_t n)
{
	bkl_enter();
	long r_ = vfs_read(f, buf, n);
	bkl_leave();
	return r_;
}

static inline long gate_vfs_write(struct vfile *f, const void *buf, size_t n)
{
	bkl_enter();
	long r_ = vfs_write(f, buf, n);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_seek(struct vfile *f, uint64_t pos)
{
	bkl_enter();
	int r_ = vfs_seek(f, pos);
	bkl_leave();
	return r_;
}

static inline uint64_t gate_vfs_tell(struct vfile *f)
{
	bkl_enter();
	uint64_t r_ = vfs_tell(f);
	bkl_leave();
	return r_;
}

static inline uint64_t gate_vfs_size(struct vfile *f)
{
	bkl_enter();
	uint64_t r_ = vfs_size(f);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_isdev(struct vfile *f)
{
	bkl_enter();
	int r_ = vfs_isdev(f);
	bkl_leave();
	return r_;
}

static inline void gate_vfs_close(struct vfile *f)
{
	bkl_enter();
	vfs_close(f);
	bkl_leave();
}

static inline char *gate_vfs_slurp(const char *abs, size_t *len, int *err)
{
	bkl_enter();
	char * r_ = vfs_slurp(abs, len, err);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_spew(const char *abs, const void *data, size_t len)
{
	bkl_enter();
	int r_ = vfs_spew(abs, data, len);
	bkl_leave();
	return r_;
}

static inline const char *gate_vfs_strerror(int err)
{
	bkl_enter();
	const char * r_ = vfs_strerror(err);
	bkl_leave();
	return r_;
}

static inline int gate_vfs_xattr_get(const char *abs, const char *key, char *val, size_t n)
{
	bkl_enter();
	int r = vfs_xattr_get(abs, key, val, n);
	bkl_leave();
	return r;
}

static inline void gate_kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static inline void gate_kprintf(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	bkl_enter();
	con_write(buf);
	bkl_leave();
}

#define con_attr gate_con_attr
#define con_clear gate_con_clear
#define con_getkey gate_con_getkey
#define con_gotoxy gate_con_gotoxy
#define con_pollkey gate_con_pollkey
#define con_putc gate_con_putc
#define con_write gate_con_write
#define con_cursor gate_con_cursor
#define con_size gate_con_size
#define read_line gate_read_line
#define resolve_path gate_resolve_path
#define shell_exec gate_shell_exec
#define rtc_epoch gate_rtc_epoch
#define epoch_to_date gate_epoch_to_date
#define arch_rtc gate_arch_rtc
#define clock_set gate_clock_set
#define sys_call gate_sys_call
#define sys_list gate_sys_list
#define vfs_drive gate_vfs_drive
#define vfs_canon gate_vfs_canon
#define vfs_basename gate_vfs_basename
#define vfs_dirname gate_vfs_dirname
#define vfs_stat gate_vfs_stat
#define vfs_list gate_vfs_list
#define vfs_mkdir gate_vfs_mkdir
#define vfs_rmdir gate_vfs_rmdir
#define vfs_delete gate_vfs_delete
#define vfs_rename gate_vfs_rename
#define vfs_setattr gate_vfs_setattr
#define vfs_info gate_vfs_info
#define vfs_open gate_vfs_open
#define vfs_read gate_vfs_read
#define vfs_write gate_vfs_write
#define vfs_seek gate_vfs_seek
#define vfs_tell gate_vfs_tell
#define vfs_size gate_vfs_size
#define vfs_isdev gate_vfs_isdev
#define vfs_close gate_vfs_close
#define vfs_slurp gate_vfs_slurp
#define vfs_spew gate_vfs_spew
#define vfs_strerror gate_vfs_strerror
#define kprintf gate_kprintf
#define vfs_xattr_get gate_vfs_xattr_get

#endif
