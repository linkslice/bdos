/*
 * heap.c - kmalloc. Power-of-two size classes from 32 to 2048 bytes carved
 * out of whole pages; anything bigger gets its own run of pages. Each
 * block carries a 16-byte header so kfree knows where it came from.
 */
#include "kernel.h"

#define MIN_SHIFT 5		/* 32 bytes */
#define MAX_SHIFT 11		/* 2048 bytes */
#define NCLASSES (MAX_SHIFT - MIN_SHIFT + 1)
#define MAGIC 0xB0D05EA1

struct hdr {
	uint32_t magic;
	uint32_t cls;		/* size class, or 0xFF for page runs */
	uint64_t pages;		/* page runs: count; classes: user size */
};

struct freeblk {
	struct freeblk *next;
};

static struct freeblk *freelist[NCLASSES];
static uint64_t in_use;

static int class_for(size_t total)
{
	for (int c = 0; c < NCLASSES; c++)
		if (total <= (1UL << (c + MIN_SHIFT)))
			return c;
	return -1;
}

void *kmalloc(size_t n)
{
	size_t total = n + sizeof(struct hdr);
	int c = class_for(total);
	struct hdr *h;
	uint64_t f = irq_save();

	if (c < 0) {
		size_t pages = (total + PAGE_SIZE - 1) / PAGE_SIZE;
		h = pmm_alloc(pages);
		if (!h) {
			irq_restore(f);
			return NULL;
		}
		h->cls = 0xFF;
		h->pages = pages;
		in_use += pages * PAGE_SIZE;
	} else {
		if (!freelist[c]) {
			uint8_t *page = pmm_alloc(1);
			if (!page) {
				irq_restore(f);
				return NULL;
			}
			size_t sz = 1UL << (c + MIN_SHIFT);
			for (size_t off = 0; off + sz <= PAGE_SIZE; off += sz) {
				struct freeblk *b = (struct freeblk *)(page + off);
				b->next = freelist[c];
				freelist[c] = b;
			}
		}
		h = (struct hdr *)freelist[c];
		freelist[c] = freelist[c]->next;
		h->cls = (uint32_t)c;
		h->pages = n;
		in_use += 1UL << (c + MIN_SHIFT);
	}
	h->magic = MAGIC;
	irq_restore(f);
	return h + 1;
}

void *kzalloc(size_t n)
{
	void *p = kmalloc(n);
	if (p)
		memset(p, 0, n);
	return p;
}

static size_t usable(struct hdr *h)
{
	if (h->cls == 0xFF)
		return h->pages * PAGE_SIZE - sizeof *h;
	return (1UL << (h->cls + MIN_SHIFT)) - sizeof *h;
}

void kfree(void *p)
{
	if (!p)
		return;
	struct hdr *h = (struct hdr *)p - 1;
	if (h->magic != MAGIC)
		panic("kfree: bad pointer %p", p);
	h->magic = 0;
	uint64_t f = irq_save();
	if (h->cls == 0xFF) {
		in_use -= h->pages * PAGE_SIZE;
		pmm_free(h, h->pages);
	} else {
		/* read the class first: the free-list link overwrites the header */
		uint32_t cls = h->cls;
		in_use -= 1UL << (cls + MIN_SHIFT);
		struct freeblk *b = (struct freeblk *)h;
		b->next = freelist[cls];
		freelist[cls] = b;
	}
	irq_restore(f);
}

void *krealloc(void *p, size_t n)
{
	if (!p)
		return kmalloc(n);
	struct hdr *h = (struct hdr *)p - 1;
	if (h->magic != MAGIC)
		panic("krealloc: bad pointer %p", p);
	if (n <= usable(h))
		return p;
	void *q = kmalloc(n);
	if (!q)
		return NULL;
	memcpy(q, p, usable(h));
	kfree(p);
	return q;
}

char *kstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = kmalloc(n);
	if (d)
		memcpy(d, s, n);
	return d;
}

uint64_t heap_in_use(void) { return in_use; }
