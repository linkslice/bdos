/* kernel.h - shared kernel declarations */
#ifndef BDOS_KERNEL_H
#define BDOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../include/bootinfo.h"

#define BDOS_NAME	"bdos"
#define BDOS_LONGNAME	"Bryan Disk Operating System"
#define BDOS_VERSION	"0.1"
#define BDOS_ARCH	"x86-64"

/* ---- port I/O ---- */
static inline void outb(uint16_t port, uint8_t v)
{
	__asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
	uint8_t v;
	__asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}
static inline void cpu_pause(void)
{
	__asm__ volatile("pause");
}

/* ---- lib.c ---- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy0(char *d, const char *s, size_t n);	/* always terminates */
int toupper(int c);
int tolower(int c);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void kprintf(const char *fmt, ...);
void fmt_commas(char *buf, size_t n, uint64_t v);	/* 1234567 -> "1,234,567" */
_Noreturn void panic(const char *fmt, ...);

/* ---- serial.c ---- */
void serial_init(void);
void serial_putc(char c);
int serial_getc(void);	/* -1 if nothing waiting */

/* ---- console.c ---- */
void con_init(const struct bdos_framebuffer *fb);
void con_putc(char c);
void con_write(const char *s);
void con_clear(void);
void con_cursor(int on);
int con_cols(void);
int con_rows(void);

/* ---- kbd.c ---- */
enum {
	KEY_UP = 0x100,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_DEL,
};
void kbd_init(void);
int kbd_getc(void);	/* -1 if nothing waiting */
int getkey(void);	/* blocks; keyboard or serial */

/* ---- cpu.c ---- */
void cpu_init(void);
_Noreturn void reboot(void);

/* ---- mem.c ---- */
void mem_init(const struct bdos_bootinfo *bi);
uint64_t mem_total(void);	/* bytes of RAM the firmware reported */
uint64_t mem_free(void);	/* bytes not used by firmware or bdos */
void mem_report(void);

/* ---- ramdisk.c ---- */
struct node {
	char name[64];
	int is_dir;
	uint8_t attr;		/* DOS attribute bits */
	const char *data;	/* file contents */
	size_t size;
	struct node *parent;
	struct node *child;	/* first child (dirs) */
	struct node *next;	/* next sibling */
};
#define ATTR_RO		0x01
#define ATTR_HIDDEN	0x02
#define ATTR_SYSTEM	0x04
#define ATTR_ARCHIVE	0x20

void rd_init(void);
struct node *rd_root(void);
struct node *rd_lookup(struct node *cwd, const char *path, int unix_style);
void rd_path(struct node *n, char *buf, size_t len, int unix_style);

/* ---- shell.c ---- */
void shell_run(const struct bdos_bootinfo *bi);

extern const struct bdos_bootinfo *bootinfo;

#endif
