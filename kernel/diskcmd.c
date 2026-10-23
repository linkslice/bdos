/*
 * diskcmd.c - FDISK, FORMAT and INSTALL at the prompt.
 *
 *   FDISK                 the disks, and what's on each
 *   FDISK 1 /GPT          wipe disk 1: a GPT with an EFI partition and bdosfs
 *   FORMAT 1              format the bdosfs partition on disk 1
 *   INSTALL               on the desktop: the Install bdos window
 *   INSTALL 1             put bdos on disk 1 (asks first; /Y doesn't)
 *   INSTALL 1 /UPGRADE    renew bdos there, keeping files and settings
 *
 * Everything that erases asks for YES, unless /Y was given.
 */
#include "kernel.h"
#include "vfs.h"
#include "disk.h"
#include "install.h"

static int has_flag(int argc, char **argv, const char *f)
{
	for (int i = 1; i < argc; i++)
		if (!strcasecmp(argv[i], f))
			return 1;
	return 0;
}

static int disk_arg(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		if (argv[i][0] >= '0' && argv[i][0] <= '9')
			return (int)strtol(argv[i], NULL, 10);
	return -1;
}

static const char *part_name(int t)
{
	return t == PART_BDOS ? "bdosfs" : t == PART_ESP ? "EFI system" : "other";
}

static void list_disks(void)
{
	kprintf("\n  Disk  Device   Size       Model                    Contents\n");
	for (int i = 0; i < blk_count(); i++) {
		struct blkdev *b = blk_get(i);
		char sz[16], what[40];
		install_size(b->sectors, sz, sizeof sz);
		disk_describe(b, what, sizeof what);
		const char *c = "";
		for (char l = 'C'; l <= 'Z'; l++)
			if (vfs_drive_disk(l) == b && l == 'C')
				c = "  (C: is here)";
		char model[24];
		strncpy0(model, b->model[0] ? b->model : "-", sizeof model);
		kprintf("  %-4d  %-7s  %-9s  %-23s  %s%s\n", i, b->name, sz, model, what, c);
		struct part p[MAX_PARTS];
		int n = disk_partitions(b, p, MAX_PARTS);
		for (int k = 0; k < n; k++) {
			install_size(p[k].count, sz, sizeof sz);
			kprintf("          partition %d: %-10s %s\n", p[k].index, part_name(p[k].type), sz);
		}
	}
	kprintf("\n");
}

static int confirm(const char *what)
{
	kprintf("%s\nType YES to go ahead: ", what);
	char line[16];
	if (read_line(line, sizeof line) < 0)
		return 0;
	return !strcmp(line, "YES") || !strcmp(line, "yes");
}

int fdisk_cmd(int argc, char **argv)
{
	int d = disk_arg(argc, argv);
	if (d < 0 || !has_flag(argc, argv, "/GPT")) {
		list_disks();
		if (argc > 1)
			kprintf("FDISK n /GPT wipes disk n and gives it an EFI partition and bdosfs.\n");
		return 0;
	}
	char err[96];
	if (install_check_disk(d, err, sizeof err)) {
		kprintf("%s\n", err);
		return 1;
	}
	struct blkdev *b = blk_get(d);
	char msg[128], sz[16];
	install_size(b->sectors, sz, sizeof sz);
	snprintf(msg, sizeof msg, "Everything on disk %d (%s, %s) will be erased.", d, b->name, sz);
	if (!has_flag(argc, argv, "/Y") && !confirm(msg))
		return 1;
	struct part p[2];
	install_release_disk(b);
	int e = disk_make_gpt(b, INSTALL_ESP_MB, p);
	if (e) {
		kprintf("Can't partition it: %s\n", vfs_strerror(e));
		return 1;
	}
	kprintf("Disk %d: EFI partition %d MB, bdosfs on the rest. FORMAT %d next.\n", d, INSTALL_ESP_MB, d);
	return 0;
}

int format_cmd(int argc, char **argv)
{
	int d = disk_arg(argc, argv);
	if (d < 0) {
		kprintf("Usage: FORMAT n   (formats the bdosfs partition on disk n; FDISK lists them)\n");
		return 1;
	}
	char err[96];
	if (install_check_disk(d, err, sizeof err)) {
		kprintf("%s\n", err);
		return 1;
	}
	struct blkdev *b = blk_get(d);
	struct part p[MAX_PARTS];
	int n = disk_partitions(b, p, MAX_PARTS), k = -1;
	for (int i = 0; i < n && k < 0; i++)
		if (p[i].type == PART_BDOS)
			k = i;
	if (k < 0) {
		kprintf("Disk %d has no bdosfs partition. FDISK %d /GPT makes one.\n", d, d);
		return 1;
	}
	char msg[96];
	snprintf(msg, sizeof msg, "Everything in the bdosfs partition on disk %d will be erased.", d);
	if (!has_flag(argc, argv, "/Y") && !confirm(msg))
		return 1;
	install_release_disk(b);
	int e = vfs_format_bdos(b, p[k].start, p[k].count, "BDOS");
	kprintf(e ? "Format failed: %s\n" : "Formatted.\n", vfs_strerror(e));
	return e ? 1 : 0;
}

static void text_progress(void *arg, int pct, const char *what)
{
	(void)arg;
	static int last = -1;
	if (pct / 10 != last / 10 || pct == 100 || pct < 10) {
		kprintf("  %3d%%  %s\n", pct, what);
		last = pct;
	}
}

int install_cmd(int argc, char **argv, void *desk)
{
	extern int installer_open(void *wm);
	int d = disk_arg(argc, argv);
	if (d < 0) {
		if (desk && !installer_open(desk))
			return 0;
		kprintf("Usage: INSTALL n [/UPGRADE]   put bdos on disk n (FDISK lists the disks)\n");
		list_disks();
		return 1;
	}
	struct install_opts o;
	install_defaults(&o);
	o.disk = d;
	o.upgrade = has_flag(argc, argv, "/UPGRADE");
	o.desktop = !has_flag(argc, argv, "/PROMPT");
	char err[160];
	if (install_check_disk(d, err, sizeof err)) {
		kprintf("%s\n", err);
		return 1;
	}
	struct blkdev *b = blk_get(d);
	char msg[160], sz[16];
	install_size(b->sectors, sz, sizeof sz);
	if (o.upgrade)
		snprintf(msg, sizeof msg, "bdos on disk %d (%s, %s) will be upgraded; your files and settings stay.",
		    d, b->name, sz);
	else
		snprintf(msg, sizeof msg, "Everything on disk %d (%s, %s) will be erased and bdos put there.", d,
		    b->name, sz);
	if (!has_flag(argc, argv, "/Y") && !confirm(msg))
		return 1;
	int e = install_run(&o, text_progress, NULL, err, sizeof err);
	if (e) {
		kprintf("Install failed: %s\n", err);
		return 1;
	}
	kprintf("bdos is on disk %d. Take the CD out (or boot from that disk) and restart.\n", d);
	return 0;
}
