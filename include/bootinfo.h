/*
 * bootinfo.h - the contract between the bdos UEFI loader and the kernel.
 *
 * The loader fills one of these, exits boot services and jumps to the
 * kernel entry point with a pointer to it in RDI (System V ABI).
 * Everything is physical and identity-mapped at hand-off.
 */
#ifndef BDOS_BOOTINFO_H
#define BDOS_BOOTINFO_H

#include <stdint.h>

#define BDOS_BOOT_MAGIC 0x534F44422D424F4FULL /* "OOB-BDOS" */

enum {
	BDOS_PIXEL_RGBX = 0,	/* byte order R,G,B,x */
	BDOS_PIXEL_BGRX = 1,	/* byte order B,G,R,x */
};

/* Memory types, collapsed from the UEFI ones. */
enum {
	BDOS_MEM_FREE = 1,	/* usable RAM (conventional + boot services) */
	BDOS_MEM_LOADER = 2,	/* kernel image, boot info, stack, memory map */
	BDOS_MEM_RESERVED = 3,	/* firmware, MMIO, ACPI, everything else */
	BDOS_MEM_ACPI = 4,	/* ACPI reclaimable tables */
};

struct bdos_framebuffer {
	uint64_t base;		/* physical address of pixel (0,0) */
	uint64_t size;		/* bytes */
	uint32_t width;		/* pixels */
	uint32_t height;	/* pixels */
	uint32_t pitch;		/* pixels per scanline */
	uint32_t format;	/* BDOS_PIXEL_* */
};

struct bdos_memrange {
	uint64_t base;
	uint64_t pages;		/* 4 KiB pages */
	uint32_t type;		/* BDOS_MEM_* */
	uint32_t _pad;
};

#define BDOS_MAX_MEMRANGES 256

struct bdos_bootinfo {
	uint64_t magic;
	struct bdos_framebuffer fb;
	uint64_t acpi_rsdp;	/* physical address, 0 if not found */
	uint64_t kernel_base;
	uint64_t kernel_size;
	uint32_t nranges;
	uint32_t _pad;
	struct bdos_memrange ranges[BDOS_MAX_MEMRANGES];
};

#endif
