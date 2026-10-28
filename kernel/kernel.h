/* kernel.h - shared kernel declarations */
#ifndef BDOS_KERNEL_H
#define BDOS_KERNEL_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "../include/bootinfo.h"
#include "arch.h"
#include "spinlock.h"

#define BDOS_NAME	"bdos"
#define BDOS_LONGNAME	"Bryan Disk Operating System"
#define BDOS_VERSION	"0.12"

#define NELEM(a)	(sizeof(a) / sizeof((a)[0]))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define MAX(a, b)	((a) > (b) ? (a) : (b))

/* ---- keys (shared by every input source) -------------------------------- */
enum {
	KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
	KEY_HOME, KEY_END, KEY_PGUP, KEY_PGDN, KEY_DEL, KEY_INS, KEY_SHIFT_TAB,
	KEY_F1 = 0x120,		/* ... KEY_F1 + 11 = F12 */
	KEY_ALT = 0x1000,	/* KEY_ALT | 'x' */
};

/* ---- lib.c -------------------------------------------------------------- */
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strncpy0(char *d, const char *s, size_t n);	/* always terminates */
char *strcat0(char *d, const char *s, size_t n);	/* bounded append */
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
int toupper(int c);
int tolower(int c);
int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
long strtol(const char *s, const char **end, int base);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void kprintf(const char *fmt, ...);
void fmt_commas(char *buf, size_t n, uint64_t v);	/* 1234567 -> "1,234,567" */
int wildmatch(const char *pat, const char *s);		/* DOS/Unix * and ?, no case */
uint32_t crc32c(uint32_t crc, const void *buf, size_t n);
_Noreturn void panic(const char *fmt, ...);

/* ---- pmm.c / heap.c ----------------------------------------------------- */
void pmm_init(const struct bdos_bootinfo *bi);
void *pmm_alloc(size_t pages);		/* contiguous, zeroed, or NULL */
void pmm_free(void *p, size_t pages);
uint64_t pmm_total(void);
uint64_t pmm_free_bytes(void);
void *kmalloc(size_t n);
void *kzalloc(size_t n);
void *krealloc(void *p, size_t n);
void kfree(void *p);
char *kstrdup(const char *s);
uint64_t heap_in_use(void);

/* ---- time.c ------------------------------------------------------------- */
void timer_tick(void);			/* called from the timer IRQ */
uint64_t uptime_ms(void);
uint64_t uptime_us(void);		/* from the CPU's cycle counter */
void delay_us(uint64_t us);		/* spins */
void time_calibrate(void);
void sleep_ms(uint64_t ms);		/* yields */

/* ---- input.c ------------------------------------------------------------ */
struct mouse_event {
	int dx, dy;
	int buttons;	/* bit 0 left, 1 right, 2 middle */
	int abs;	/* 1: ax, ay are an absolute position, 0..65535 each way */
	int ax, ay;
	int wheel;	/* clicks of the wheel: + away from you (scroll up), - towards */
};
void input_wheel(int clicks);		/* from drivers: + up, - down */
void input_key(int key);		/* from drivers, any context */
void input_mouse_abs(int x, int y, int buttons);	/* tablets, vmmouse: 0..65535 */
int input_mouse_is_abs(void);
int input_mods(void);		/* keys held now: 1 shift, 2 ctrl, 4 alt */
void input_key_press(int id, int key);	/* a physical key went down (id = scancode) */
void input_key_release(int id);
void input_settings_init(void);
void input_mouse(int dx, int dy, int buttons);
int input_getkey(void);			/* -1 if none */
int input_getmouse(struct mouse_event *ev);	/* 0 if none */
void input_raw(uint8_t sc);		/* a set-1 scancode byte, as the keyboard sent it */
int input_getraw(void);			/* -1 if none */

/* ---- consoles ------------------------------------------------------------
 * A console is where a task's text goes and where its keys come from:
 * the full screen, a desktop window, COM1, or a remote session.
 */
struct console {
	const char *name;
	void (*write)(struct console *, const char *, size_t);
	int (*getkey)(struct console *);	/* non-blocking, -1 if none */
	void (*clear)(struct console *);
	void (*cursor)(struct console *, int on);
	void (*size)(struct console *, int *cols, int *rows);
	void (*gotoxy)(struct console *, int x, int y);		/* optional */
	void (*attr)(struct console *, int fg, int bg);		/* optional, 0-15 */
	void *priv;
	int refs;
	void (*rawchar)(struct console *, int ch);	/* optional: a glyph, controls and all */
};

void screen_init(const struct bdos_framebuffer *fb);
struct console *screen_console(void);
void screen_suspend(int off);		/* the desktop takes over the display */
void screen_mirror(int on);		/* echo the screen to COM1 */
uint32_t pixel_pack(uint32_t rgb);	/* 0xRRGGBB -> framebuffer pixel */
uint32_t pixel_unpack(uint32_t px);	/* framebuffer pixel -> 0xRRGGBB */
extern const uint32_t cga_palette[16];
void console_panic_mode(void);		/* force output to the screen */
struct console *serial_console(void);

void con_write(const char *s);		/* to the current task's console */
void con_writen(const char *s, size_t n);
void con_putc(char c);
int con_getkey(void);			/* blocks */
int con_pollkey(void);
void con_clear(void);
void con_cursor(int on);
void con_size(int *cols, int *rows);
void con_gotoxy(int x, int y);
void con_attr(int fg, int bg);
void con_rawchar(int ch);		/* CP437 glyph at the cursor, even 0x01-0x1F */

/* ---- tasks -------------------------------------------------------------- */
#define TASK_NAME_MAX 24

enum { TASK_READY, TASK_DEAD };

struct task {
	struct arch_context ctx;	/* first: it wants 16-byte alignment */
	int id;
	char name[TASK_NAME_MAX];
	volatile int state;
	volatile int cpu;		/* the core running it, or -1 */
	int bkl;			/* holds on the big kernel lock (0: running free) */
	uint64_t poll_epoch;		/* polled and found nothing at this epoch */
	uint64_t ticks;			/* CPU time used, in ms */
	int last_cpu;
	void *stack;
	size_t stack_pages;
	struct console *con;		/* where this task's text goes */
	char cwd[256];			/* "C:\GAMES" */
	void (*entry)(void *);
	void *arg;
	int exit_code;
	volatile int waiters;
	struct task *next;
	void *user;			/* per-subsystem data (shell state) */
	void *desk;			/* the desktop this task lives on, if any */
	void *tdrive;			/* a session's T: drive (struct vfs_drive) */
	int asess;			/* the audio session it sounds in (0: the console) */
	void *proc;			/* the native program this is a thread of, if any */
	uint64_t fs_base;		/* its thread-local storage */
	char dos_speed[12];		/* DOS programs here run at this pace ("": as usual) */
};

/* One per core. GS points at it; self must stay first. */
struct cpu {
	struct cpu *self;
	uint64_t kstack;		/* +8: the current task's kernel stack top (SYSCALL uses it) */
	uint64_t scratch;		/* +16: the user stack pointer, while SYSCALL switches */
	int id;
	uint32_t hwid;			/* local APIC ID */
	struct task *current, *idle, *prev;
	volatile int online, parked;
	volatile int need_resched;
	uint64_t slice;			/* ticks the current task has run */
	volatile uint64_t busy_ticks, idle_ticks;	/* for Pulse */
	uint64_t last_busy, last_idle;
	volatile int load;		/* percent busy over the last quarter second */
	void *arch;			/* GDT, TSS: the arch layer's business */
};
extern struct cpu cpus[MAX_CPUS];
extern int ncpus;

void task_init(void);
void sched_start_cpu(void);		/* an extra core enters the scheduler */
void sched_tick(void);			/* per-core timer tick */
void sched_preempt_check(void);		/* end of an interrupt: switch if due */
struct task *task_spawn(const char *name, void (*fn)(void *), void *arg,
    struct console *con);
struct task *task_current(void);
void task_yield(void);
void task_idle(void);			/* nothing to do until something happens */
void sched_kick(void);			/* something happened: idle pollers, look again */
_Noreturn void task_exit(void);
int task_wait(struct task *t);		/* until t exits, returns its code */
int task_alive(struct task *t);
void task_list(void);
struct task_info {
	int id, cpu;
	uint64_t ms;
	char name[TASK_NAME_MAX], con[12], cwd[40];
};
int task_snapshot(struct task_info *out, int max);	/* the live tasks, how many */
int cpu_park(int cpu, int parked);	/* Pulse's buttons; 0 or -1 */
void cpu_sample(void);			/* every 250 ms: each core's load */

/*
 * The big kernel lock. Tasks hold it by default, so kernel code stays as
 * single-threaded as it always was. Code that computes on its own memory
 * (the interpreters, the DOS emulator, native programs) lets go of it, runs
 * in parallel on any core, and takes it back to call into the kernel.
 */
int bkl_release_all(void);		/* returns the depth to give back */
void bkl_reacquire(int depth);
void bkl_enter(void);			/* gates: take it (nesting is fine) */
void bkl_leave(void);
int bkl_held(void);

/* ---- shell.c ------------------------------------------------------------ */
#define KEY_EOF 0x1A			/* Ctrl-Z ends console input, as in DOS */
void shell_task(void *arg);		/* arg: optional command line to run */
int shell_exec(const char *line);	/* run one command in the current task */
int read_line(char *buf, size_t cap);	/* line editor; -1 at end of input */
int stdin_is_pipe(void);
int stdout_is_redirected(void);
int resolve_path(const char *in, char *out, size_t n);	/* against task cwd */
void kernel_late_init(void);

/* ---- edit.c ------------------------------------------------------------- */
int edit_file(const char *abs_path);	/* full-screen editor */

/* ---- basic.c ------------------------------------------------------------ */
int basic_run_file(const char *abs_path, int argc, char **argv);
int basic_immediate(void);		/* interactive BASIC prompt */

extern const struct bdos_bootinfo *bootinfo;

#endif
