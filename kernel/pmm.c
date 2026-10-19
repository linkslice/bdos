/*
 * pmm.c - physical page allocator: one bit per 4 KiB page.
 *
 * Usable RAM is everything the firmware called conventional or boot
 * services memory (we own all of it after ExitBootServices and after
 * switching to our own page tables). Memory below 1 MiB stays reserved.
 */
#include "kernel.h"

static uint8_t *bitmap;		/* 1 = used */
static uint64_t npages;		/* pages covered by the bitmap */
static uint64_t total_bytes, free_pages;
static uint64_t hint;

static inline int used(uint64_t p) { return bitmap[p >> 3] & (1 << (p & 7)); }
static inline void set(uint64_t p) { bitmap[p >> 3] |= (uint8_t)(1 << (p & 7)); }
static inline void clr(uint64_t p) { bitmap[p >> 3] &= (uint8_t)~(1 << (p & 7)); }

void pmm_init(const struct bdos_bootinfo *bi)
{
	uint64_t top = 0;
	for (uint32_t i = 0; i < bi->nranges; i++) {
		const struct bdos_memrange *r = &bi->ranges[i];
		if (r->type == BDOS_MEM_RESERVED)
			continue;
		uint64_t end = r->base + r->pages * PAGE_SIZE;
		if (end > top)
			top = end;
		total_bytes += r->pages * PAGE_SIZE;
	}
	npages = top / PAGE_SIZE;
	uint64_t bm_bytes = (npages + 7) / 8;
	uint64_t bm_pages = (bm_bytes + PAGE_SIZE - 1) / PAGE_SIZE;

	/* Put the bitmap in the first free range above 1 MiB that fits. */
	for (uint32_t i = 0; i < bi->nranges && !bitmap; i++) {
		const struct bdos_memrange *r = &bi->ranges[i];
		uint64_t base = r->base < 0x100000 ? 0x100000 : r->base;
		uint64_t end = r->base + r->pages * PAGE_SIZE;
		if (r->type == BDOS_MEM_FREE && end > base && end - base >= bm_pages * PAGE_SIZE)
			bitmap = (uint8_t *)base;
	}
	if (!bitmap)
		panic("no room for the page bitmap");

	memset(bitmap, 0xFF, bm_bytes);
	for (uint32_t i = 0; i < bi->nranges; i++) {
		const struct bdos_memrange *r = &bi->ranges[i];
		if (r->type != BDOS_MEM_FREE)
			continue;
		for (uint64_t p = r->base / PAGE_SIZE; p < r->base / PAGE_SIZE + r->pages; p++) {
			if (p < 0x100000 / PAGE_SIZE)
				continue;
			clr(p);
			free_pages++;
		}
	}
	uint64_t b0 = (uint64_t)bitmap / PAGE_SIZE;
	for (uint64_t p = b0; p < b0 + bm_pages; p++) {
		set(p);
		free_pages--;
	}
	hint = 0x100000 / PAGE_SIZE;
}

void *pmm_alloc(size_t n)
{
	if (!n)
		return NULL;
	uint64_t f = irq_save();
	for (int pass = 0; pass < 2; pass++) {
		uint64_t start = pass ? 0x100000 / PAGE_SIZE : hint;
		uint64_t run = 0;
		for (uint64_t p = start; p < npages; p++) {
			if (!(p & 7) && bitmap[p >> 3] == 0xFF) {
				run = 0;
				p += 7;
				continue;
			}
			if (used(p)) {
				run = 0;
				continue;
			}
			if (++run == n) {
				uint64_t first = p + 1 - n;
				for (uint64_t q = first; q <= p; q++)
					set(q);
				free_pages -= n;
				hint = p + 1;
				irq_restore(f);
				void *mem = (void *)(first * PAGE_SIZE);
				memset(mem, 0, n * PAGE_SIZE);
				return mem;
			}
		}
	}
	irq_restore(f);
	return NULL;
}

void pmm_free(void *mem, size_t n)
{
	uint64_t f = irq_save();
	uint64_t first = (uint64_t)mem / PAGE_SIZE;
	for (uint64_t p = first; p < first + n; p++) {
		if (!used(p))
			panic("pmm_free: page %lx already free", p);
		clr(p);
	}
	free_pages += n;
	if (first < hint)
		hint = first;
	irq_restore(f);
}

uint64_t pmm_total(void) { return total_bytes; }
uint64_t pmm_free_bytes(void) { return free_pages * PAGE_SIZE; }
