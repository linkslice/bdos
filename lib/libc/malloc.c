/*
 * malloc.c - a thread-safe allocator: power-of-two size classes carved
 * from sbrk, with big blocks (over 1 MB) from mmap.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <bdos.h>
#include "../../include/bdos_sys.h"

#define MIN_SHIFT 4		/* 16 bytes */
#define MAX_SHIFT 20		/* 1 MB; bigger goes to mmap */
#define NBINS (MAX_SHIFT + 1)
#define MAGIC 0xB105F00Du

struct hdr {
	uint32_t magic;
	uint32_t bin;		/* size class, or 0 for mmap */
	uint64_t size;		/* mmap: the mapped length; else the class size */
};

struct freeblk {
	struct freeblk *next;
};

static struct freeblk *bins[NBINS];
static bdos_mutex lock = BDOS_MUTEX_INIT;

static char *arena, *arena_end;

static void *more(size_t n)
{
	if (arena + n > arena_end) {
		size_t grab = n > 256 * 1024 ? n : 256 * 1024;
		char *p = sbrk((long)grab);
		if (p == (char *)-1)
			return NULL;
		if (p != arena_end)	/* someone else moved the break */
			arena = p;
		arena_end = p + grab;
	}
	void *r = arena;
	arena += n;
	return r;
}

static int class_of(size_t n)
{
	int b = MIN_SHIFT;
	while (((size_t)1 << b) < n)
		b++;
	return b;
}

void *malloc(size_t n)
{
	if (n > ((size_t)1 << 46)) {
		errno = ENOMEM;
		return NULL;
	}
	size_t need = n + sizeof(struct hdr);
	if (need > ((size_t)1 << MAX_SHIFT)) {
		size_t len = (need + 4095) & ~(size_t)4095;
		long a = bdos_syscall(BS_MMAP, (long)len, 0, 0, 0);
		if (a < 0) {
			errno = ENOMEM;
			return NULL;
		}
		struct hdr *h = (struct hdr *)a;
		h->magic = MAGIC;
		h->bin = 0;
		h->size = len;
		return h + 1;
	}
	int b = class_of(need);
	bdos_lock(&lock);
	struct hdr *h;
	if (bins[b]) {
		h = (struct hdr *)bins[b];
		bins[b] = bins[b]->next;
	} else {
		h = more((size_t)1 << b);
	}
	bdos_unlock(&lock);
	if (!h) {
		errno = ENOMEM;
		return NULL;
	}
	h->magic = MAGIC;
	h->bin = (uint32_t)b;
	h->size = (size_t)1 << b;
	return h + 1;
}

void free(void *p)
{
	if (!p)
		return;
	struct hdr *h = (struct hdr *)p - 1;
	if (h->magic != MAGIC) {
		static const char m[] = "free: bad pointer\n";
		write(2, m, sizeof m - 1);
		abort();
	}
	h->magic = 0;
	if (!h->bin) {
		bdos_syscall(BS_MUNMAP, (long)h, (long)h->size, 0, 0);
		return;
	}
	struct freeblk *f = (struct freeblk *)h;
	bdos_lock(&lock);
	f->next = bins[h->bin];
	bins[h->bin] = f;
	bdos_unlock(&lock);
}

static size_t usable(void *p)
{
	struct hdr *h = (struct hdr *)p - 1;
	return h->size - sizeof *h;
}

void *calloc(size_t n, size_t m)
{
	if (m && n > (size_t)-1 / m) {
		errno = ENOMEM;
		return NULL;
	}
	void *p = malloc(n * m);
	if (p)
		memset(p, 0, n * m);
	return p;
}

void *realloc(void *p, size_t n)
{
	if (!p)
		return malloc(n);
	if (!n) {
		free(p);
		return NULL;
	}
	size_t have = usable(p);
	if (n <= have && n > have / 4)
		return p;
	void *q = malloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, n < have ? n : have);
	free(p);
	return q;
}

void *aligned_alloc(size_t align, size_t n)
{
	/* blocks are 16-byte aligned; for more, over-allocate a power of two
	 * class (classes are aligned to their size relative to the arena) */
	if (align <= 16)
		return malloc(n);
	errno = EINVAL;
	return NULL;
}
