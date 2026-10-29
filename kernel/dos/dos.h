/*
 * dos.h - a DOS machine: a 386/486 (x86.c), PC hardware, a VGA, a BIOS and
 * DOS itself, all for one program and its children. Every DOS program runs
 * in a box of its own, on a task of its own, so each can have a core.
 *
 *   dosbox.c   the box: loading, the main loop, pacing, screen and input
 *   hw.c       PIC, PIT, keyboard controller, CMOS, speaker, A20
 *   vga.c      VGA registers and memory, and turning it into pixels
 *   bios.c     the ROM: interrupt stubs, INT 10h/16h/1Ah/15h/33h...
 *   dosapi.c   INT 21h: files, memory, EXEC, FCBs; XMS
 *   dpmi.c     INT 31h for protected-mode programs
 */
#ifndef BDOS_DOS_H
#define BDOS_DOS_H

#include "x86.h"
#include "sound.h"
#include "../vfs.h"
#include "../gui/wm.h"

#define DOS_MEM_DEFAULT	(16u << 20)	/* 16 MB: conventional + extended */
#define CONV_TOP_SEG	0x9FC0		/* conventional memory ends here (EBDA above) */
#define FIRST_MCB	0x0200
#define DOS_DATA_SEG	0x0070		/* our "kernel" data: LoL, SFT stubs, buffers */
#define BIOS_SEG	0xF000
#define VBIOS_SEG	0xC000

/* where things live in the ROM (offsets in F000) */
#define ROM_VECTORS	0x0000	/* F1 nn CF for every vector: 4 bytes each */
#define ROM_INT8	0x0800
#define ROM_INT9	0x0840
#define ROM_INT1C	0x0880
#define ROM_MOUSE_RET	0x0890	/* the mouse event handler returns here */
#define ROM_XMS		0x08A0	/* XMS driver entry */
#define ROM_DPMI_SW	0x08B0	/* DPMI mode switch entry */
#define ROM_DPMI_RET	0x08C0	/* real-mode callbacks and reflections come back here */
#define ROM_EXIT	0x08D0	/* a program's last RET lands here */
#define ROM_IDLE	0x08E0	/* HLT loop */
#define ROM_VBE_BANK	0x08F0	/* VESA window function */
#define ROM_VGA_FUNCS	0x0900	/* INT 10h 1Bh: the static functionality table */
#define ROM_SYSCONF	0xE6F5	/* INT 15h C0h: the system configuration table */
#define ROM_DUMMY_IRET	0xFF53	/* where unused vectors point, as on a real BIOS */
#define ROM_PM_STUBS	0x1000	/* DPMI protected-mode interrupt stubs: 256 x 4 */

/* HLE numbers that aren't interrupt vectors (F1 nn with nn free) */
#define HLE_INT9	0xF0
#define HLE_MOUSE_RET	0xF1
#define HLE_XMS		0xF2
#define HLE_DPMI_SW	0xF3
#define HLE_DPMI_RET	0xF4
#define HLE_EXIT	0xF5
#define HLE_PM_INT	0xF6	/* reflect a protected-mode interrupt */
#define HLE_PM_EXC	0xF7	/* default exception handler */
#define HLE_RM_CB	0xF8	/* a real-mode callback into protected mode */
#define HLE_VBE_BANK	0xF9
#define HLE_PM_DEF	0xFA	/* the default action for a protected-mode interrupt */
#define HLE_EXC_RET	0xFB	/* an exception handler is done */
#define HLE_RM_TO_PM	0xFC	/* a real-mode interrupt with a protected-mode handler */
#define HLE_RAW_TO_PM	0xFD	/* raw mode switches */
#define HLE_RAW_TO_RM	0xFE

#define MAX_SFT		64
#define MAX_SEARCH	16
#define MAX_EXT		128	/* extended memory blocks: XMS handles and DPMI blocks */

struct sft {
	int refs;
	int dev;		/* 0 file, else a DEV_ */
	struct vfile *f;
	uint8_t mode;
	char name[16];
	char path[VFS_PATH_MAX];	/* to reopen it from a saved state */
	uint64_t pos;			/* where it was, in a saved state */
};

enum { DEV_CON = 1, DEV_NUL, DEV_AUX, DEV_PRN, DEV_CLOCK };

struct search {
	int used;
	struct vstat *ents;
	int n, i;
};

struct exec_frame {
	uint16_t psp;
	uint32_t ss, esp;
	uint32_t regs[8];
	uint16_t ds, es;
	uint32_t dta;
};

struct pit_ch {
	uint32_t reload;	/* 1..65536 */
	int mode, access, write_hi, read_hi;
	int latched;
	uint16_t latch;
	uint16_t partial;
	uint64_t start_us;	/* when the count was last loaded */
	int gate;
};

struct vga {
	uint8_t *vram;		/* 4 planes of VGA_PLANE bytes, one after another */
	uint8_t seq_idx, seq[8];
	uint8_t gc_idx, gc[16];
	uint8_t crtc_idx, crtc[32];
	uint8_t attr_idx, attr[32], attr_flip;
	uint8_t misc;
	uint8_t dac[256][3];
	uint8_t dac_widx, dac_ridx, dac_wsub, dac_rsub, dac_mask;
	uint8_t latch[4];
	int mode;		/* the BIOS mode number last set */
	int vbe_mode, vbe_bank;
	int width, height;	/* for VBE modes */
	int dirty;
	uint64_t retrace_t0;
	/* the display start as the CRTC latched it at the last vertical retrace a
	 * program waited for: a page flip shows whole pages, never half-written
	 * start addresses */
	uint32_t shown_start;
	uint64_t latched_at;
};

#define VGA_PLANE	(256 * 1024)

struct mouse {
	int on;			/* INT 33h reset done */
	int shown;		/* show count: >0 visible */
	int x, y, buttons;
	int minx, maxx, miny, maxy;
	int mickx, micky;
	int press_count[3], press_x[3], press_y[3];
	int rel_count[3], rel_x[3], rel_y[3];
	uint16_t handler_mask, handler_seg, handler_off;
	int pending_events;
	/* events for the handler, one call each, with the buttons as they were then */
	struct { uint16_t ev, buttons; int16_t x, y; } q[16];
	int qh, qt;
	int latched;		/* presses function 3 hasn't reported yet */
	uint64_t motion_read;	/* when the program last asked how far it moved (0Bh) */
	int fx, fy;		/* captured: where the driver has the pointer, in eighths */
	int ratio_x, ratio_y;	/* mickeys per 8 pixels (0Fh): 8 and 16 */
	int sens_x, sens_y, sens_dbl;	/* sensitivity (1Ah), 50 is as it comes */
	/* the program's own graphics cursor (09h): AND and XOR masks, hot spot */
	int cur_set, hot_x, hot_y;
	uint16_t cur_and[16], cur_xor[16];
	int in_handler;
	uint32_t saved[8];
	uint16_t saved_ds, saved_es;
};

struct dosbox {
	struct x86 cpu;
	struct task *task;
	struct console *con;
	int dump_req, debug_keys, log_dos;
	uint32_t ints_taken[256];
	/* the guest's clock: wall time less however far the CPU fell behind its pace */
	uint64_t lag_us, g_last_wall, g_last_ran, g_frac;
	int g_idle;
	int running, exit_code;
	const char *errmsg;
	char errbuf[96];
	char prog[VFS_PATH_MAX];
	/* pacing: instructions per millisecond, 0 for flat out */
	long speed;
	uint64_t t0_us, ran;
	/* hardware */
	uint8_t pic_imr[2], pic_irr[2], pic_isr[2], pic_base[2];
	int pic_init[2], pic_read_isr[2];
	struct pit_ch pit[3];
	uint64_t irq0_next_us;
	uint8_t kbq[64];
	int kbh, kbt;
	uint8_t port60, kbc_status, kbc_cmd, kbc_cmdbyte, port61;
	int kb_wait_cmd;
	uint8_t cmos_idx;
	int speaker_hz;
	/* the PC speaker, played through the mixer: what port 61h and the PIT's
	 * channel 2 did, and when (the program's clock), drawn as a wave */
	struct spk {
		int stream;		/* +1 */
		int mixer;		/* through the mixer (else the PC's own speaker) */
		struct { uint64_t t; uint8_t bits; uint32_t hz; } ev[128];
		int nev;
		uint64_t last_us;	/* drawn up to here */
		uint8_t bits;		/* port 61h's low two bits, as of last_us */
		uint32_t hz;
		uint32_t phase;
		int32_t x1, y1;		/* the DC blocker's memory */
		int idle;		/* blocks of silence since the last sound */
	} spk;
	/* video */
	struct vga vga;
	struct display *disp;
	int disp_failed;
	int text_live;		/* mirroring B800 onto the console */
	uint8_t shown[132 * 60 * 2];
	int cursor_x, cursor_y;
	uint64_t next_frame_us;
	uint32_t *pal32;	/* the 256 colours as pixels */
	/* input */
	int raw_seen;		/* real make/break codes have arrived */
	int synth_break;	/* scancode to release on the next tick, or 0 */
	uint64_t synth_at;
	int idle_polls;		/* INT 16h polled empty: probably waiting */
	int kb_e0;		/* an E0 prefix came before this scancode */
	uint64_t wait_until;	/* INT 15h AH=86h */
	int mouse_wanted;	/* the program reset the mouse driver */
	int mouse_possible;	/* the screen can give us a mouse */
	int want_quit;
	/* DOS */
	uint16_t psp, first_psp;
	uint16_t dta_seg, dta_off;
	struct sft sft[MAX_SFT];
	struct search search[MAX_SEARCH];
	char cwd[26][VFS_PATH_MAX];	/* each drive's current directory, "C:\\GAMES" */
	int drive;
	int retcode;		/* for 4Dh */
	int ctrl_break;
	uint8_t alloc_strategy;
	int verify;
	int line_len;		/* INT 21h 0Ah / handle 0: the line being typed */
	char line[256];
	int line_done, line_pos, line_active;
	int pending_scan;	/* the second half of an extended key, for 06/07/08 */
	int flushed;		/* 0Ch: the buffer's been emptied already */
	int break_check;
	int last_error;
	int search_next;
	struct exec_frame exec[16];
	int exec_depth;
	int input_wait;		/* the BIOS or DOS is waiting for a key */
	int stream;		/* program output goes to the console as text */
	int said_no_graphics;
	/* the last picture shown in a display, so only what changed goes out */
	uint32_t *shadow;
	int shadow_w, shadow_h;
	const uint32_t *shadow_of;
	int last_w, last_h;	/* the mode the display was last sized for */
	int raw_ctrl, raw_alt;
	int rcap;		/* the pointer's captured, on a whole display of our own (bterm -t) */
	int state_req;		/* 1 save, 2 load, at the next chance */
	/* XMS */
	struct { int used; uint32_t base, size; int locks; } xms[MAX_EXT];
	int hma_used;
	/* mouse */
	struct mouse mouse;
	/* sound */
	struct dma_ch dma[8];
	int dma_ff[2];			/* the byte flip-flops, low and high controllers */
	struct sb sb;
	struct opl opl;
	/* DPMI: the host's state lives in the guest's own memory, at this
	 * linear address, so saved states carry it along */
	uint32_t dpmi_lin;
	/* saved state */
	char state_file[VFS_PATH_MAX];
};

/* now, as the program sees it (microseconds): the timers, retrace and sound go by this */
#define PIT_HZ 1193182ULL	/* the 8254's clock */

static inline uint64_t dos_now(struct dosbox *b)
{
	return uptime_us() - b->lag_us;
}

static inline struct dosbox *BOX(struct x86 *c)
{
	return c->box;
}

/* hw.c */
void hw_reset(struct dosbox *b);
uint32_t hw_in(struct x86 *c, uint16_t port, int size);
void hw_out(struct x86 *c, uint16_t port, uint32_t v, int size);
void pic_raise(struct dosbox *b, int irq);
int pic_pending(struct dosbox *b);		/* the vector to deliver, or -1 */
int pic_waiting(struct dosbox *b);		/* one would be, if IF allowed */
void pit_tick(struct dosbox *b, uint64_t now_us);
uint64_t pit_irq0_period_us(struct dosbox *b);
void kbd_push(struct dosbox *b, uint8_t sc);
void kbd_pump(struct dosbox *b);
void speaker_update(struct dosbox *b);

/* vga.c */
int vga_init(struct dosbox *b);
void vga_free(struct dosbox *b);
uint8_t vga_rd(struct x86 *c, uint32_t a);
void vga_wr(struct x86 *c, uint32_t a, uint8_t v);
uint32_t vga_in(struct dosbox *b, uint16_t port);
void vga_out(struct dosbox *b, uint16_t port, uint8_t v);
void vga_set_mode(struct dosbox *b, int mode, int clear);
int vga_is_text(struct dosbox *b);
int vga_text_size(struct dosbox *b, int *cols, int *rows);
void vga_render(struct dosbox *b, struct surface *s, struct rect area);	/* scaled into area */
void vga_screen_size(struct dosbox *b, int *w, int *h);
int vga_vbe(struct dosbox *b);			/* INT 10h AH=4F */
extern const uint8_t vga_font8x16[256 * 16];
extern const uint8_t vga_font8x8[256 * 8];

/* bios.c */
void bios_init(struct dosbox *b);
int bios_hle(struct x86 *c, int n);
void bios_set_cursor(struct dosbox *b, int x, int y);
void bios_tty(struct dosbox *b, uint8_t ch);
int bios_key_avail(struct dosbox *b);
uint16_t bios_key_get(struct dosbox *b);
void mouse_motion(struct dosbox *b, int dx, int dy, int buttons);
int mouse_wants_capture(struct dosbox *b);
void mouse_event(struct dosbox *b, int x, int y, int buttons);
struct rect vga_fit(struct rect area, int w, int h);	/* where a w x h picture goes */
void dosbox_crash_dump(struct dosbox *b, uint16_t cs, uint32_t ip);
void mouse_draw(struct dosbox *b, struct surface *s, struct rect area);
void bios_set_flag(struct x86 *c, uint32_t f, int on);	/* in the caller's stacked FLAGS */

/* dosapi.c */
void dos_init(struct dosbox *b);
int dos_hle(struct dosbox *b, int n);		/* INT 20h-2Fh */
int mouse_callback(struct dosbox *b);
void dos_int21(struct dosbox *b);
int dos_int21_wait(struct dosbox *b);		/* 1: input needed, come back */
void dos_int2f(struct dosbox *b);
int dos_load(struct dosbox *b, const char *path, const char *tail, int is_exec, uint16_t env);
void dos_terminate(struct dosbox *b, int code, int tsr_paras);
void dos_close_all(struct dosbox *b);
int dos_resolve(struct dosbox *b, const char *in, char *out, size_t n);
void xms_entry(struct dosbox *b);
uint16_t mcb_alloc(struct dosbox *b, uint16_t paras, uint16_t owner, uint16_t *largest);
int mcb_free(struct dosbox *b, uint16_t seg);
int mcb_resize(struct dosbox *b, uint16_t seg, uint16_t paras, uint16_t *largest);
void dos_con_out(struct dosbox *b, uint8_t ch);	/* standard output: the screen, or > a file */
void dos_print(struct dosbox *b, const char *s);	/* to the program's screen */

/* dpmi.c */
int dpmi_detect(struct dosbox *b);		/* INT 2Fh 1687h */
int dpmi_hle(struct x86 *c, int n);
void dpmi_int31(struct dosbox *b);
void dpmi_free(struct dosbox *b);
void dpmi_rom(struct dosbox *b);		/* its stubs in the ROM */
void dpmi_terminating(struct dosbox *b, uint16_t psp);	/* a program is ending */

/* state.c: freezing a program and thawing it again */
int dosbox_save_state(struct dosbox *b);
int dosbox_load_state(struct dosbox *b, const char *file);
void dosbox_state_name(const char *prog, char *out, size_t n);

/* dosbox.c */
int dosbox_input_wait(struct dosbox *b);	/* pump input; 1 if a key may be ready */
void dosbox_set_graphics(struct dosbox *b, int on);
void dosbox_text_dirty(struct dosbox *b);
void dosbox_poll(struct dosbox *b);		/* events, keys, Ctrl-Break */
void dosbox_stream_char(struct dosbox *b, uint8_t ch);	/* teletype output, to the console */
void dosbox_screen_mode(struct dosbox *b);	/* the program draws the whole screen now */
uint32_t ext_alloc(struct dosbox *b, uint32_t size, int *handle);
void ext_free(struct dosbox *b, int handle);
uint32_t ext_free_kb(struct dosbox *b, uint32_t *largest_kb);
long dosbox_speed_for(const char *name);	/* "xt", "at", "386", "486", "max" */

/* guest memory helpers */
static inline uint32_t sl(uint16_t seg, uint16_t off)
{
	return ((uint32_t)seg << 4) + off;
}
static inline uint8_t mb(struct dosbox *b, uint32_t a) { return x86_peek8(&b->cpu, a); }
static inline void mbw(struct dosbox *b, uint32_t a, uint8_t v) { x86_poke8(&b->cpu, a, v); }
static inline uint16_t mw(struct dosbox *b, uint32_t a) { return (uint16_t)(mb(b, a) | mb(b, a + 1) << 8); }
static inline void mww(struct dosbox *b, uint32_t a, uint16_t v) { mbw(b, a, (uint8_t)v); mbw(b, a + 1, (uint8_t)(v >> 8)); }
static inline uint32_t md(struct dosbox *b, uint32_t a) { return (uint32_t)mw(b, a) | (uint32_t)mw(b, a + 2) << 16; }
static inline void mdw(struct dosbox *b, uint32_t a, uint32_t v) { mww(b, a, (uint16_t)v); mww(b, a + 2, (uint16_t)(v >> 16)); }

/* register shorthands for the HLE code */
#define AX(c) ((uint16_t)(c)->r[R_EAX])
#define BX(c) ((uint16_t)(c)->r[R_EBX])
#define CX(c) ((uint16_t)(c)->r[R_ECX])
#define DX(c) ((uint16_t)(c)->r[R_EDX])
#define SI(c) ((uint16_t)(c)->r[R_ESI])
#define DI(c) ((uint16_t)(c)->r[R_EDI])
#define BP(c) ((uint16_t)(c)->r[R_EBP])
#define AL(c) ((uint8_t)(c)->r[R_EAX])
#define AH(c) ((uint8_t)((c)->r[R_EAX] >> 8))
#define BL(c) ((uint8_t)(c)->r[R_EBX])
#define BH(c) ((uint8_t)((c)->r[R_EBX] >> 8))
#define CL(c) ((uint8_t)(c)->r[R_ECX])
#define CH(c) ((uint8_t)((c)->r[R_ECX] >> 8))
#define DL(c) ((uint8_t)(c)->r[R_EDX])
#define DH(c) ((uint8_t)((c)->r[R_EDX] >> 8))
static inline void set16(struct x86 *c, int r, uint16_t v) { c->r[r] = (c->r[r] & 0xFFFF0000) | v; }
static inline void setlo(struct x86 *c, int r, uint8_t v) { c->r[r] = (c->r[r] & 0xFFFFFF00) | v; }
static inline void sethi(struct x86 *c, int r, uint8_t v) { c->r[r] = (c->r[r] & 0xFFFF00FF) | (uint32_t)v << 8; }
#define SEG(c, i) ((c)->s[i].sel)

#endif
