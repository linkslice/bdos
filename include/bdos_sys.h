/*
 * bdos_sys.h - the system calls native programs make, shared by the kernel
 * (kernel/proc.c) and the C library (lib/libc).
 *
 * SYSCALL with the number in RAX and arguments in RDI, RSI, RDX, R10;
 * the result comes back in RAX: negative is an error (the E_ codes below).
 */
#ifndef BDOS_SYS_H
#define BDOS_SYS_H

#include <stdint.h>

enum {
	BS_EXIT = 0,		/* code */
	BS_WRITE,		/* fd, buf, n -> written */
	BS_READ,		/* fd, buf, n -> read (fd 0: a console line) */
	BS_OPEN,		/* path, BS_O_ flags -> fd */
	BS_CLOSE,		/* fd */
	BS_SEEK,		/* fd, offset, whence -> position */
	BS_SBRK,		/* increment -> old break */
	BS_MMAP,		/* length -> address (anonymous, zeroed) */
	BS_MUNMAP,		/* address, length */
	BS_GETKEY,		/* wait -> key (bdos key codes), -1 if none */
	BS_UPTIME_MS,
	BS_UPTIME_US,
	BS_SLEEP,		/* ms */
	BS_YIELD,
	BS_THREAD,		/* entry, arg, stack size -> thread id */
	BS_THREAD_EXIT,		/* code */
	BS_JOIN,		/* thread id -> its exit code */
	BS_SYS,			/* name, struct bs_sysval args[], argc, ret: the SYS calls */
	BS_GETCWD,		/* buf, size */
	BS_CHDIR,		/* path */
	BS_STAT,		/* path, struct bs_stat * */
	BS_LIST,		/* path, buf, size -> bytes: NUL-separated names, folders end in \ */
	BS_REMOVE,		/* path */
	BS_MKDIR,		/* path */
	BS_RMDIR,		/* path */
	BS_RENAME,		/* from, to */
	BS_SET_FS,		/* thread-local storage base */
	BS_NCPUS,		/* -> cores */
	BS_CONSOLE,		/* op (0 clear, 1 goto x y, 2 attr fg bg, 3 cursor on, 4 size), a, b */
	BS_SYSTEM,		/* command line -> its result (a bdos shell command) */
	BS_WAIT_NE,		/* uint32 *addr, value, timeout ms: wait while *addr == value */
	BS_TIME,		/* -> seconds since 1970 */
	BS_GETENV,		/* name, buf, size: bdos settings */
	BS_RESOLVE,		/* name -> IPv4 address (host order), or an error */
	BS_TCP_CONNECT,		/* address, port, timeout ms -> socket */
	BS_TCP_LISTEN,		/* port -> socket */
	BS_TCP_ACCEPT,		/* socket, timeout ms (-1 forever) -> socket */
	BS_TCP_SEND,		/* socket, buf, n -> n */
	BS_TCP_RECV,		/* socket, buf, n, timeout ms -> n, 0 at the end */
	BS_TCP_CLOSE,		/* socket */
	BS_AUDIO_OPEN,		/* sample rate -> stream (16-bit samples, mixed to the sound card) */
	BS_AUDIO_WRITE,		/* stream, samples, frames, channels (1 or 2) -> frames; waits for room */
	BS_AUDIO_CLOSE,		/* stream: what's queued still plays */
	BS_AUDIO_QUEUED,	/* stream -> frames not played yet */
};

#define BS_O_READ	1
#define BS_O_WRITE	2
#define BS_O_CREATE	4
#define BS_O_TRUNC	8
#define BS_O_APPEND	16

struct bs_stat {
	uint64_t size;
	uint64_t mtime;		/* seconds since 1970 */
	int32_t is_dir;
	int32_t attr;
};

/* SYS calls: the same as BASIC's SYS and the C interpreter's built-ins */
struct bs_sysval {
	int32_t is_str;
	int32_t _pad;
	long long n;
	double f;
	char s[256];
};

/* errors (negative results) */
#define BS_ENOENT	-2
#define BS_EEXIST	-3
#define BS_ENOTDIR	-4
#define BS_EISDIR	-5
#define BS_ENOSPC	-6
#define BS_ENOTEMPTY	-7
#define BS_EINVAL	-8
#define BS_EIO		-9
#define BS_ENAMETOOLONG	-11
#define BS_EACCES	-12
#define BS_ENOMEM	-13
#define BS_EBADF	-23
#define BS_ENFILE	-24
#define BS_ENOSYS	-25
#define BS_ETIMEDOUT	-26
#define BS_EREFUSED	-27
#define BS_ERESET	-28
#define BS_ENOHOST	-29
#define BS_ENONET	-30

/* keys from BS_GETKEY */
#define BS_KEY_UP	0x100
#define BS_KEY_DOWN	0x101
#define BS_KEY_LEFT	0x102
#define BS_KEY_RIGHT	0x103
#define BS_KEY_F1	0x120

#endif
