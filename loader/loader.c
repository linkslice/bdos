/*
 * loader.c - bdos UEFI boot loader (BOOTX64.EFI)
 *
 *  1. pick a graphics mode through GOP
 *  2. load \BDOS\KERNEL.SYS (ELF64) from the boot volume
 *  3. find the ACPI RSDP
 *  4. take the memory map and exit boot services
 *  5. jump to the kernel with a struct bdos_bootinfo
 */
#include "efi.h"
#include "../include/bootinfo.h"

#define KERNEL_PATH	L"\\BDOS\\KERNEL.SYS"
#define STACK_PAGES	16	/* 64 KiB kernel stack */
#define WANT_W		1024
#define WANT_H		768

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* clang may emit calls to these even in freestanding code */
void *memset(void *d, int c, size_t n)
{
	uint8_t *p = d;
	while (n--)
		*p++ = (uint8_t)c;
	return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
	uint8_t *dp = d;
	const uint8_t *sp = s;
	while (n--)
		*dp++ = *sp++;
	return d;
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{
	const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
	for (int i = 0; i < 16; i++)
		if (x[i] != y[i])
			return 0;
	return 1;
}

static void puts16(CHAR16 *s)
{
	ST->ConOut->OutputString(ST->ConOut, s);
}

static void puthex(uint64_t v)
{
	CHAR16 buf[19];
	buf[0] = L'0';
	buf[1] = L'x';
	for (int i = 0; i < 16; i++) {
		int d = (v >> ((15 - i) * 4)) & 0xF;
		buf[2 + i] = (CHAR16)(d < 10 ? L'0' + d : L'A' + d - 10);
	}
	buf[18] = 0;
	puts16(buf);
}

static void putdec(uint64_t v)
{
	CHAR16 buf[21];
	int i = 20;
	buf[i] = 0;
	do {
		buf[--i] = (CHAR16)(L'0' + v % 10);
		v /= 10;
	} while (v);
	puts16(&buf[i]);
}

static void fail(CHAR16 *what, EFI_STATUS st)
{
	puts16(L"\r\nbdos loader: ");
	puts16(what);
	puts16(L" (status ");
	puthex(st);
	puts16(L")\r\nSystem halted.\r\n");
	for (;;)
		__asm__ volatile("hlt");
}

/* ---- Graphics ----------------------------------------------------------- */

static void setup_graphics(struct bdos_framebuffer *fb)
{
	EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
	EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
	EFI_STATUS st;

	st = BS->LocateProtocol(&gop_guid, NULL, (void **)&gop);
	if (EFI_ERROR(st))
		fail(L"no graphics output (GOP)", st);

	/*
	 * Prefer exactly 1024x768. Otherwise keep the firmware's current mode
	 * unless it is smaller than 800x600 and something bigger exists.
	 */
	uint32_t best = gop->Mode->Mode;
	uint32_t best_w = gop->Mode->Info->HorizontalResolution;
	uint32_t best_h = gop->Mode->Info->VerticalResolution;
	int exact = (best_w == WANT_W && best_h == WANT_H);

	for (uint32_t m = 0; m < gop->Mode->MaxMode && !exact; m++) {
		EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info;
		UINTN sz;
		if (EFI_ERROR(gop->QueryMode(gop, m, &sz, &info)))
			continue;
		if (info->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
		    info->PixelFormat != PixelBlueGreenRedReserved8BitPerColor)
			continue;
		if (info->HorizontalResolution == WANT_W &&
		    info->VerticalResolution == WANT_H) {
			best = m;
			exact = 1;
		} else if (best_w < 800 && info->HorizontalResolution > best_w &&
		    info->HorizontalResolution <= 1280) {
			best = m;
			best_w = info->HorizontalResolution;
			best_h = info->VerticalResolution;
		}
	}

	if (best != gop->Mode->Mode) {
		st = gop->SetMode(gop, best);
		if (EFI_ERROR(st))
			fail(L"could not set graphics mode", st);
	}

	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *info = gop->Mode->Info;
	if (info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor)
		fb->format = BDOS_PIXEL_RGBX;
	else if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor)
		fb->format = BDOS_PIXEL_BGRX;
	else
		fail(L"unsupported pixel format", info->PixelFormat);

	fb->base = gop->Mode->FrameBufferBase;
	fb->size = gop->Mode->FrameBufferSize;
	fb->width = info->HorizontalResolution;
	fb->height = info->VerticalResolution;
	fb->pitch = info->PixelsPerScanLine;
}

/* ---- Kernel file -------------------------------------------------------- */

/* Read a whole file from the boot volume. optional: return NULL if absent. */
static void *read_file(EFI_HANDLE image, CHAR16 *path, UINTN *size_out, int optional,
    EFI_MEMORY_TYPE type)
{
	EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	EFI_GUID fs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	EFI_LOADED_IMAGE_PROTOCOL *li;
	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
	EFI_FILE_PROTOCOL *root, *file;
	EFI_STATUS st;

	st = BS->HandleProtocol(image, &li_guid, (void **)&li);
	if (EFI_ERROR(st))
		fail(L"no loaded-image protocol", st);
	st = BS->HandleProtocol(li->DeviceHandle, &fs_guid, (void **)&fs);
	if (EFI_ERROR(st))
		fail(L"boot volume has no file system", st);
	st = fs->OpenVolume(fs, &root);
	if (EFI_ERROR(st))
		fail(L"cannot open boot volume", st);
	st = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(st)) {
		root->Close(root);
		if (optional)
			return NULL;
		puts16(L"\r\n");
		puts16(path);
		fail(L"cannot open file", st);
	}

	uint64_t size;
	file->SetPosition(file, 0xFFFFFFFFFFFFFFFFULL);	/* seek to end */
	file->GetPosition(file, &size);
	file->SetPosition(file, 0);

	void *buf;
	EFI_PHYSICAL_ADDRESS pa;
	st = BS->AllocatePages(AllocateAnyPages, type, (size + 4095) / 4096, &pa);
	if (EFI_ERROR(st))
		fail(L"out of memory reading a file", st);
	buf = (void *)pa;
	UINTN n = size;
	st = file->Read(file, &n, buf);
	if (EFI_ERROR(st) || n != size)
		fail(L"error reading a file", st);
	file->Close(file);
	root->Close(root);

	*size_out = size;
	return buf;
}

/* Minimal ELF64 definitions */
typedef struct {
	uint8_t  e_ident[16];
	uint16_t e_type, e_machine;
	uint32_t e_version;
	uint64_t e_entry, e_phoff, e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Elf64_Ehdr;

typedef struct {
	uint32_t p_type, p_flags;
	uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} Elf64_Phdr;

#define PT_LOAD 1

static uint64_t load_elf(uint8_t *img, UINTN size, uint64_t *lo, uint64_t *hi)
{
	Elf64_Ehdr *eh = (Elf64_Ehdr *)img;

	if (size < sizeof(*eh) || eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' ||
	    eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
		fail(L"KERNEL.SYS is not an ELF file", 0);
	if (eh->e_ident[4] != 2 || eh->e_machine != 0x3E)
		fail(L"KERNEL.SYS is not an x86-64 ELF64 image", eh->e_machine);

	*lo = ~0ULL;
	*hi = 0;
	for (int i = 0; i < eh->e_phnum; i++) {
		Elf64_Phdr *ph = (Elf64_Phdr *)(img + eh->e_phoff + (uint64_t)i * eh->e_phentsize);
		if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
			continue;

		uint64_t start = ph->p_paddr & ~0xFFFULL;
		uint64_t end = (ph->p_paddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
		EFI_PHYSICAL_ADDRESS addr = start;
		EFI_STATUS st = BS->AllocatePages(AllocateAddress, EfiLoaderData,
		    (end - start) / 4096, &addr);
		if (EFI_ERROR(st)) {
			puts16(L"\r\nsegment at ");
			puthex(start);
			fail(L"memory for the kernel is not free", st);
		}
		memset((void *)start, 0, end - start);
		memcpy((void *)ph->p_paddr, img + ph->p_offset, ph->p_filesz);
		if (start < *lo)
			*lo = start;
		if (end > *hi)
			*hi = end;
	}
	if (*hi == 0)
		fail(L"KERNEL.SYS has no loadable segments", 0);
	return eh->e_entry;
}

/* ---- ACPI --------------------------------------------------------------- */

static uint64_t find_rsdp(void)
{
	EFI_GUID acpi20 = EFI_ACPI_20_TABLE_GUID;
	EFI_GUID acpi10 = EFI_ACPI_10_TABLE_GUID;
	uint64_t v1 = 0;

	for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
		EFI_CONFIGURATION_TABLE *t = &ST->ConfigurationTable[i];
		if (guid_eq(&t->VendorGuid, &acpi20))
			return (uint64_t)t->VendorTable;
		if (guid_eq(&t->VendorGuid, &acpi10))
			v1 = (uint64_t)t->VendorTable;
	}
	return v1;
}

/* ---- Memory map + exit -------------------------------------------------- */

static uint32_t collapse_type(uint32_t t)
{
	switch (t) {
	case EfiConventionalMemory:
	case EfiBootServicesCode:
	case EfiBootServicesData:
		return BDOS_MEM_FREE;
	case EfiLoaderCode:
	case EfiLoaderData:
		return BDOS_MEM_LOADER;
	case EfiACPIReclaimMemory:
		return BDOS_MEM_ACPI;
	default:
		return BDOS_MEM_RESERVED;
	}
}

static void exit_boot(EFI_HANDLE image, struct bdos_bootinfo *bi)
{
	UINTN size = 0, key, dsize;
	uint32_t dver;
	EFI_MEMORY_DESCRIPTOR *map = NULL;
	EFI_STATUS st;

	st = BS->GetMemoryMap(&size, NULL, &key, &dsize, &dver);
	if (st != EFI_BUFFER_TOO_SMALL)
		fail(L"GetMemoryMap failed", st);
	size += 16 * dsize;	/* room for the pool allocation we are making */
	st = BS->AllocatePool(EfiLoaderData, size, (void **)&map);
	if (EFI_ERROR(st))
		fail(L"out of memory for the memory map", st);

	/*
	 * The map key goes stale if anything allocates between GetMemoryMap
	 * and ExitBootServices, so retry a few times. No console output in
	 * this loop: printing can allocate.
	 */
	for (int tries = 0; tries < 8; tries++) {
		UINTN sz = size;
		st = BS->GetMemoryMap(&sz, map, &key, &dsize, &dver);
		if (EFI_ERROR(st))
			continue;
		st = BS->ExitBootServices(image, key);
		if (!EFI_ERROR(st)) {
			/* Boot services are gone: only touch our own memory now. */
			uint32_t n = 0;
			for (UINTN off = 0; off < sz && n < BDOS_MAX_MEMRANGES; off += dsize) {
				EFI_MEMORY_DESCRIPTOR *d = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)map + off);
				uint32_t t = collapse_type(d->Type);
				if (n > 0 && bi->ranges[n - 1].type == t &&
				    bi->ranges[n - 1].base + bi->ranges[n - 1].pages * 4096 == d->PhysicalStart) {
					bi->ranges[n - 1].pages += d->NumberOfPages;
					continue;
				}
				bi->ranges[n].base = d->PhysicalStart;
				bi->ranges[n].pages = d->NumberOfPages;
				bi->ranges[n].type = t;
				n++;
			}
			bi->nranges = n;
			return;
		}
	}
	fail(L"ExitBootServices failed", st);
}

/* ---- Entry -------------------------------------------------------------- */

typedef void (__attribute__((sysv_abi)) *kernel_entry_t)(struct bdos_bootinfo *);

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
	ST = systab;
	BS = systab->BootServices;
	EFI_STATUS st;

	BS->SetWatchdogTimer(0, 0, 0, NULL);
	ST->ConOut->ClearScreen(ST->ConOut);
	puts16(L"Starting bdos...\r\n");

	/* Boot info and stack: one block of loader pages. */
	EFI_PHYSICAL_ADDRESS bi_addr;
	UINTN bi_pages = (sizeof(struct bdos_bootinfo) + 4095) / 4096;
	st = BS->AllocatePages(AllocateAnyPages, EfiLoaderData, bi_pages + STACK_PAGES, &bi_addr);
	if (EFI_ERROR(st))
		fail(L"out of memory for boot info", st);
	struct bdos_bootinfo *bi = (struct bdos_bootinfo *)bi_addr;
	memset(bi, 0, sizeof(*bi));
	bi->magic = BDOS_BOOT_MAGIC;
	uint64_t stack_top = bi_addr + (bi_pages + STACK_PAGES) * 4096;

	UINTN ksize;
	/* The ELF file itself is scratch: boot-services memory, reusable later. */
	uint8_t *kimg = read_file(image, KERNEL_PATH, &ksize, 0, EfiBootServicesData);
	uint64_t lo, hi;
	uint64_t entry = load_elf(kimg, ksize, &lo, &hi);
	bi->kernel_base = lo;
	bi->kernel_size = hi - lo;
	puts16(L"Kernel loaded at ");
	puthex(lo);
	puts16(L", ");
	putdec((hi - lo) / 1024);
	puts16(L" KiB\r\n");

	UINTN rsize;
	void *rootfs = read_file(image, L"\\BDOS\\ROOTFS.IMG", &rsize, 1, EfiLoaderData);
	if (rootfs) {
		bi->rootfs_base = (uint64_t)rootfs;
		bi->rootfs_size = rsize;
		puts16(L"Root filesystem image loaded, ");
		putdec(rsize / 1024);
		puts16(L" KiB\r\n");
	}

	UINTN lsize;
	if (read_file(image, L"\\BDOS\\LIVE.TXT", &lsize, 1, EfiLoaderData)) {
		bi->flags |= BDOS_BOOT_LIVE;
		puts16(L"Live CD: C: is the RAM disk\r\n");
	}

	bi->acpi_rsdp = find_rsdp();

	/* Last chance to talk through the firmware console. */
	setup_graphics(&bi->fb);

	exit_boot(image, bi);

	/* Interrupts off, our own stack, then into the kernel. Never returns. */
	__asm__ volatile(
	    "cli\n\t"
	    "mov %0, %%rsp\n\t"
	    "xor %%ebp, %%ebp\n\t"
	    "mov %1, %%rdi\n\t"
	    "call *%2\n\t"
	    "1: hlt\n\t"
	    "jmp 1b\n\t"
	    :
	    : "r"(stack_top), "r"(bi), "r"(entry)
	    : "rdi", "memory");
	return EFI_SUCCESS;
}
