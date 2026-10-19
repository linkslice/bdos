/* mem.c - what the firmware told us about RAM. No allocator yet. */
#include "kernel.h"

static uint64_t total, freeb, loader, acpi;

void mem_init(const struct bdos_bootinfo *bi)
{
	for (uint32_t i = 0; i < bi->nranges; i++) {
		const struct bdos_memrange *r = &bi->ranges[i];
		uint64_t bytes = r->pages * 4096;
		switch (r->type) {
		case BDOS_MEM_FREE:
			freeb += bytes;
			total += bytes;
			break;
		case BDOS_MEM_LOADER:
			loader += bytes;
			total += bytes;
			break;
		case BDOS_MEM_ACPI:
			acpi += bytes;
			total += bytes;
			break;
		default:
			break;
		}
	}
}

uint64_t mem_total(void) { return total; }
uint64_t mem_free(void) { return freeb; }

void mem_report(void)
{
	char a[32];
	kprintf("\nMemory Type          Total\n");
	kprintf("----------------  ---------------\n");
	fmt_commas(a, sizeof a, freeb / 1024);
	kprintf("Free              %12s K\n", a);
	fmt_commas(a, sizeof a, loader / 1024);
	kprintf("bdos + loader     %12s K\n", a);
	fmt_commas(a, sizeof a, acpi / 1024);
	kprintf("ACPI tables       %12s K\n", a);
	kprintf("----------------  ---------------\n");
	fmt_commas(a, sizeof a, total / 1024);
	kprintf("Total RAM         %12s K\n\n", a);

	const struct bdos_bootinfo *bi = bootinfo;
	fmt_commas(a, sizeof a, bi->kernel_size / 1024);
	kprintf("Kernel at %p, %s K\n", (void *)bi->kernel_base, a);
	kprintf("No 640K limit here. Largest program: as big as RAM.\n\n");
}
