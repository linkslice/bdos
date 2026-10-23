/* sys.c - system calls, and the POSIX-ish layer over them */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <locale.h>
#include <bdos.h>
#include "../../include/bdos_sys.h"

int errno;

static long sc(long n, long a, long b, long c, long d)
{
	return bdos_syscall(n, a, b, c, d);
}

/* bdos error codes to errno values */
static long fix(long r)
{
	if (r >= 0)
		return r;
	switch (r) {
	case BS_ENOENT: errno = ENOENT; break;
	case BS_EEXIST: errno = EEXIST; break;
	case BS_ENOTDIR: errno = ENOTDIR; break;
	case BS_EISDIR: errno = EISDIR; break;
	case BS_ENOSPC: errno = ENOSPC; break;
	case BS_ENOTEMPTY: errno = ENOTEMPTY; break;
	case BS_ENAMETOOLONG: errno = ENAMETOOLONG; break;
	case BS_EACCES: errno = EACCES; break;
	case BS_ENOMEM: errno = ENOMEM; break;
	case BS_EBADF: errno = EBADF; break;
	case BS_ENFILE: errno = EMFILE; break;
	case BS_ENOSYS: errno = ENOSYS; break;
	default: errno = EINVAL;
	}
	return -1;
}

ssize_t read(int fd, void *buf, size_t n) { return fix(sc(BS_READ, fd, (long)buf, (long)n, 0)); }
ssize_t write(int fd, const void *buf, size_t n) { return fix(sc(BS_WRITE, fd, (long)buf, (long)n, 0)); }
int close(int fd) { return (int)fix(sc(BS_CLOSE, fd, 0, 0, 0)); }
off_t lseek(int fd, off_t off, int whence) { return fix(sc(BS_SEEK, fd, off, whence, 0)); }
int unlink(const char *p) { return (int)fix(sc(BS_REMOVE, (long)p, 0, 0, 0)); }
int remove(const char *p) { return unlink(p); }
int rmdir(const char *p) { return (int)fix(sc(BS_RMDIR, (long)p, 0, 0, 0)); }
int mkdir(const char *p, mode_t m) { (void)m; return (int)fix(sc(BS_MKDIR, (long)p, 0, 0, 0)); }
int rename(const char *a, const char *b) { return (int)fix(sc(BS_RENAME, (long)a, (long)b, 0, 0)); }
int chdir(const char *p) { return (int)fix(sc(BS_CHDIR, (long)p, 0, 0, 0)); }
int chmod(const char *p, mode_t m) { (void)p; (void)m; return 0; }
mode_t umask(mode_t m) { (void)m; return 0; }
int isatty(int fd) { return fd < 3; }
pid_t getpid(void) { return 1; }
int dup(int fd) { (void)fd; errno = ENOSYS; return -1; }
int dup2(int a, int b) { (void)a; (void)b; errno = ENOSYS; return -1; }
int execvp(const char *f, char *const argv[]) { (void)f; (void)argv; errno = ENOSYS; return -1; }

char *getcwd(char *buf, size_t n)
{
	static char own[256];
	if (!buf) {
		buf = own;
		n = sizeof own;
	}
	return fix(sc(BS_GETCWD, (long)buf, (long)n, 0, 0)) < 0 ? NULL : buf;
}

int open(const char *path, int flags, ...)
{
	int bf = 0;
	switch (flags & O_ACCMODE) {
	case O_RDONLY: bf = BS_O_READ; break;
	case O_WRONLY: bf = BS_O_WRITE; break;
	default: bf = BS_O_READ | BS_O_WRITE;
	}
	if (flags & O_CREAT) bf |= BS_O_CREATE;
	if (flags & O_TRUNC) bf |= BS_O_TRUNC;
	if (flags & O_APPEND) bf |= BS_O_APPEND;
	if ((flags & O_EXCL) && (flags & O_CREAT)) {
		struct stat st;
		if (!stat(path, &st)) {
			errno = EEXIST;
			return -1;
		}
	}
	return (int)fix(sc(BS_OPEN, (long)path, bf, 0, 0));
}

int creat(const char *path, mode_t m) { (void)m; return open(path, O_WRONLY | O_CREAT | O_TRUNC); }

int stat(const char *path, struct stat *st)
{
	struct bs_stat b;
	if (fix(sc(BS_STAT, (long)path, (long)&b, 0, 0)) < 0)
		return -1;
	memset(st, 0, sizeof *st);
	st->st_size = (off_t)b.size;
	st->st_mtime = st->st_atime = st->st_ctime = (time_t)b.mtime;
	st->st_mode = (b.is_dir ? S_IFDIR : S_IFREG) | 0755;
	st->st_nlink = 1;
	return 0;
}

int fstat(int fd, struct stat *st)
{
	memset(st, 0, sizeof *st);
	if (fd < 3) {
		st->st_mode = 0020000;	/* a character device */
		return 0;
	}
	off_t cur = lseek(fd, 0, SEEK_CUR);
	if (cur < 0)
		return -1;
	st->st_size = lseek(fd, 0, SEEK_END);
	lseek(fd, cur, SEEK_SET);
	st->st_mode = S_IFREG | 0644;
	return 0;
}

int access(const char *path, int mode)
{
	struct stat st;
	(void)mode;
	return stat(path, &st);
}

void *sbrk(long inc)
{
	long r = sc(BS_SBRK, inc, 0, 0, 0);
	if (r < 0) {
		errno = ENOMEM;
		return (void *)-1;
	}
	return (void *)r;
}

unsigned sleep(unsigned s) { sc(BS_SLEEP, (long)s * 1000, 0, 0, 0); return 0; }
int usleep(unsigned long us) { sc(BS_SLEEP, (long)(us + 999) / 1000, 0, 0, 0); return 0; }

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	sc(BS_SLEEP, req->tv_sec * 1000 + req->tv_nsec / 1000000, 0, 0, 0);
	if (rem)
		rem->tv_sec = rem->tv_nsec = 0;
	return 0;
}

long sysconf(int name)
{
	if (name == _SC_NPROCESSORS_ONLN)
		return sc(BS_NCPUS, 0, 0, 0, 0);
	if (name == _SC_PAGESIZE)
		return 4096;
	return -1;
}

time_t time(time_t *t)
{
	time_t v = sc(BS_TIME, 0, 0, 0, 0);
	if (t)
		*t = v;
	return v;
}

clock_t clock(void)
{
	return sc(BS_UPTIME_MS, 0, 0, 0, 0);
}

int gettimeofday(struct timeval *tv, void *tz)
{
	(void)tz;
	long us = sc(BS_UPTIME_US, 0, 0, 0, 0);
	long base = sc(BS_TIME, 0, 0, 0, 0);
	tv->tv_sec = base;
	tv->tv_usec = us % 1000000;
	return 0;
}

char *getenv(const char *name)
{
	static char val[256];
	if (!strcmp(name, "HOME") || !strcmp(name, "TMPDIR")) {
		return strcmp(name, "HOME") ? "C:\\" : "C:\\";
	}
	return sc(BS_GETENV, (long)name, (long)val, sizeof val, 0) < 0 ? NULL : val;
}

int system(const char *cmd)
{
	if (!cmd)
		return 1;
	fflush(stdout);
	return (int)sc(BS_SYSTEM, (long)cmd, 0, 0, 0);
}

sighandler_t signal(int sig, sighandler_t h) { (void)sig; (void)h; return SIG_DFL; }
int raise(int sig) { if (sig == SIGABRT || sig == SIGTERM || sig == SIGINT) _exit(128 + sig); return 0; }

char *setlocale(int cat, const char *loc) { (void)cat; (void)loc; return "C"; }
struct lconv *localeconv(void) { static struct lconv l = { "." }; return &l; }

/* ---- directories ---- */

struct DIR {
	char *names;
	long len, pos;
	struct dirent ent;
};

DIR *opendir(const char *path)
{
	DIR *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	long cap = 16384;
	for (;;) {
		d->names = malloc((size_t)cap);
		if (!d->names) {
			free(d);
			return NULL;
		}
		long r = sc(BS_LIST, (long)path, (long)d->names, cap, 0);
		if (r == BS_ENAMETOOLONG && cap < (1 << 22)) {
			free(d->names);
			cap *= 4;
			continue;
		}
		if (r < 0) {
			free(d->names);
			free(d);
			fix(r);
			return NULL;
		}
		d->len = r;
		break;
	}
	return d;
}

struct dirent *readdir(DIR *d)
{
	if (d->pos >= d->len)
		return NULL;
	char *n = d->names + d->pos;
	size_t l = strlen(n);
	d->pos += (long)l + 1;
	strncpy(d->ent.d_name, n, sizeof d->ent.d_name - 1);
	d->ent.d_type = DT_REG;
	if (l && d->ent.d_name[l - 1] == '\\') {
		d->ent.d_name[l - 1] = 0;
		d->ent.d_type = DT_DIR;
	}
	return &d->ent;
}

int closedir(DIR *d)
{
	free(d->names);
	free(d);
	return 0;
}

/* ---- bdos.h ---- */

struct trampoline {
	int (*fn)(void *);
	void *arg;
};

static void thread_start(struct trampoline *t)
{
	int (*fn)(void *) = t->fn;
	void *arg = t->arg;
	free(t);
	int r = fn(arg);
	sc(BS_THREAD_EXIT, r, 0, 0, 0);
}

int bdos_thread(int (*fn)(void *), void *arg)
{
	struct trampoline *t = malloc(sizeof *t);
	if (!t)
		return -1;
	t->fn = fn;
	t->arg = arg;
	long r = sc(BS_THREAD, (long)thread_start, (long)t, 0, 0);
	if (r < 0) {
		free(t);
		return (int)fix(r);
	}
	return (int)r;
}

int bdos_join(int tid) { return (int)sc(BS_JOIN, tid, 0, 0, 0); }
int bdos_cores(void) { return (int)sc(BS_NCPUS, 0, 0, 0, 0); }
void bdos_yield(void) { sc(BS_YIELD, 0, 0, 0, 0); }
long bdos_ms(void) { return sc(BS_UPTIME_MS, 0, 0, 0, 0); }
long bdos_us(void) { return sc(BS_UPTIME_US, 0, 0, 0, 0); }
int bdos_key(int wait) { fflush(stdout); return (int)sc(BS_GETKEY, wait, 0, 0, 0); }
void bdos_cls(void) { fflush(stdout); sc(BS_CONSOLE, 0, 0, 0, 0); }
void bdos_goto(int x, int y) { fflush(stdout); sc(BS_CONSOLE, 1, x, y, 0); }
void bdos_color(int fg, int bg) { fflush(stdout); sc(BS_CONSOLE, 2, fg, bg, 0); }
void bdos_cursor(int on) { sc(BS_CONSOLE, 3, on, 0, 0); }

void bdos_size(int *cols, int *rows)
{
	long r = sc(BS_CONSOLE, 4, 0, 0, 0);
	*cols = (int)(r >> 32);
	*rows = (int)(r & 0xFFFFFFFF);
}

void bdos_lock(bdos_mutex *m)
{
	/* 0 free, 1 held, 2 held with someone waiting */
	uint32_t c = 0;
	if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return;
	for (int spin = 0; spin < 100; spin++) {
		c = 0;
		if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return;
		__builtin_ia32_pause();
	}
	while (__atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE) != 0)
		sc(BS_WAIT_NE, (long)&m->v, 2, 0, 0);
}

void bdos_unlock(bdos_mutex *m)
{
	__atomic_store_n(&m->v, 0, __ATOMIC_RELEASE);
}

static int sys_v(const char *name, long long *ret_n, char *ret_s, const char *types, va_list ap)
{
	struct bs_sysval args[8], ret;
	int argc = 0;
	memset(args, 0, sizeof args);
	for (; types && *types && argc < 8; types++, argc++) {
		if (*types == 's') {
			const char *s = va_arg(ap, const char *);
			args[argc].is_str = 1;
			strncpy(args[argc].s, s ? s : "", sizeof args[argc].s - 1);
		} else if (*types == 'f') {
			args[argc].f = va_arg(ap, double);
			args[argc].n = (long long)args[argc].f;
		} else {
			args[argc].n = va_arg(ap, long long);
			args[argc].f = (double)args[argc].n;
		}
	}
	memset(&ret, 0, sizeof ret);
	fflush(stdout);
	int r = (int)sc(BS_SYS, (long)name, (long)args, argc, (long)&ret);
	if (ret_n)
		*ret_n = ret.n;
	if (ret_s)
		strcpy(ret_s, ret.s);
	return r;
}

int bdos_sys(const char *name, const char *types, ...)
{
	va_list ap;
	va_start(ap, types);
	int r = sys_v(name, NULL, NULL, types, ap);
	va_end(ap);
	return r;
}

int bdos_sysr(const char *name, long long *ret_n, char *ret_s, const char *types, ...)
{
	va_list ap;
	va_start(ap, types);
	int r = sys_v(name, ret_n, ret_s, types, ap);
	va_end(ap);
	return r;
}

/* ---- endings (exit() itself is in start.c) ---- */

_Noreturn void _exit(int code)
{
	bdos_syscall(BS_EXIT, code, 0, 0, 0);
	for (;;)
		;
}

_Noreturn void _Exit(int code) { _exit(code); }

_Noreturn void abort(void)
{
	fputs("abort\n", stderr);
	_exit(134);
}

void __assert_fail(const char *expr, const char *file, int line)
{
	fprintf(stderr, "%s:%d: assertion failed: %s\n", file, line, expr);
	abort();
}

/* ---- sys/mman.h ---- */

#include <sys/mman.h>

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	(void)addr; (void)prot; (void)off;
	if (!(flags & MAP_ANONYMOUS) || fd != -1 || (flags & MAP_FIXED)) {
		errno = ENOSYS;
		return MAP_FAILED;
	}
	long r = sc(BS_MMAP, (long)len, 0, 0, 0);
	if (r < 0) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return (void *)r;
}

int munmap(void *addr, size_t len) { return (int)fix(sc(BS_MUNMAP, (long)addr, (long)len, 0, 0)); }
int mprotect(void *addr, size_t len, int prot) { (void)addr; (void)len; (void)prot; return 0; }

char *__environ_empty[1];
char **environ = __environ_empty;
