/*
 * cdrom.h - CD drives: ISO 9660 images in a changer.
 *
 * A CD drive is a drive letter with a list of discs (ISO images) and one of
 * them in it, or none. The discs come from files (MOUNT D: GAME.ISO) or from
 * bterm (bterm -d game1.iso,game2.iso), which sends the sectors as they're
 * read. A remote session's drives are its own, as its T: is.
 */
#ifndef BDOS_CDROM_H
#define BDOS_CDROM_H
#include <stdint.h>
#include <stddef.h>

#define CD_SECTOR	2048
#define CD_MAX_DISCS	16

struct changer;

/* where a disc's sectors come from */
struct cdsrc {
	long (*read)(struct cdsrc *s, uint64_t off, void *buf, size_t n);	/* bytes, or <0 */
	void (*close)(struct cdsrc *s);
	void *ctx;
	uint64_t size;			/* bytes, if known (0: ask the disc) */
};

/* a changer's discs: open number i (0-based) */
typedef int (*cd_open_fn)(void *ctx, int i, struct cdsrc *out);

struct changer *changer_new(char letter, const char *desc, cd_open_fn open, void *ctx, int session);
int changer_add(struct changer *c, const char *name);
int changer_insert(struct changer *c, int disc);	/* -1 ejects; 0 or an error */
void changer_free(struct changer *c);

/* the CD drive at a letter (as the caller's session sees it), or NULL */
struct changer *changer_at(char letter);
int changer_letter(struct changer *c);
int changer_discs(struct changer *c);
int changer_current(struct changer *c);			/* -1: empty */
const char *changer_disc_name(struct changer *c, int i);
const char *changer_source(struct changer *c);		/* "\\MYMAC", "C:\GAMES\X.ISO" */
uint32_t changer_generation(struct changer *c);		/* goes up at every change of disc */
const char *changer_label(struct changer *c);
uint64_t changer_size(struct changer *c);
int changer_read_sectors(struct changer *c, uint32_t lba, uint32_t n, void *buf);

/* MOUNT: image files at a letter (comma-separated: a changer) */
int cd_mount_files(char letter, const char *files, char *err, size_t n);
int cd_unmount(char letter);				/* E_ACCES: bterm's */
int cd_add_file(char letter, const char *file, char *err, size_t n);
int changer_local(struct changer *c);

/* letters of the CD drives the caller can see, "DE" */
int cd_letters(char *out, size_t n);
#endif
