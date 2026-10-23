/* disk.h - partition tables (MBR and GPT) and the FAT32 writer for EFI partitions */
#ifndef BDOS_DISK_H
#define BDOS_DISK_H
#include <stdint.h>
#include <stddef.h>
#include "drv/drv.h"

enum { PART_OTHER, PART_BDOS, PART_ESP };

struct part {
	int type;		/* PART_* */
	uint64_t start, count;	/* in 512-byte sectors */
	int index;		/* 1-based, as the table numbers it */
	int gpt;
};

#define MAX_PARTS 16

/* the partitions on a disk: from its GPT if it has one, else its MBR. -1 if
 * the disk can't be read, 0 if it has no partition table at all */
int disk_partitions(struct blkdev *b, struct part *parts, int max);

/* wipe the disk and give it a GPT: an EFI system partition of esp_mb
 * megabytes, then bdosfs over the rest. out[0] is the ESP, out[1] bdosfs */
int disk_make_gpt(struct blkdev *b, uint32_t esp_mb, struct part out[2]);

/* describe what's on a disk in a few words: "empty", "bdos", "2 partitions" */
void disk_describe(struct blkdev *b, char *out, size_t n);

/* ---- FAT32, enough to fill a new EFI system partition ---- */

struct fatw;
int fat_format(struct blkdev *b, uint64_t start, uint64_t count, const char *label, struct fatw **out);
int fat_mkdir(struct fatw *w, const char *path);			/* "EFI/BOOT" */
int fat_add(struct fatw *w, const char *path, const void *data, uint32_t len);
int fat_finish(struct fatw *w);					/* writes the FATs; frees w */

uint32_t crc32(uint32_t crc, const void *buf, size_t n);		/* IEEE 802.3, as GPT uses */
#endif
