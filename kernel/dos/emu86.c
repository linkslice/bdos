/*
 * emu86.c - run real-mode DOS programs (.COM and .EXE) in an 8086 emulator.
 *
 * The CPU is a complete 8086 plus the common 80186 additions. DOS (INT 21h),
 * video BIOS (INT 10h), keyboard (INT 16h) and clock (INT 1Ah) services are
 * provided by bdos itself: files come from the VFS (8.3 names resolve
 * through bdosfs's short aliases), the screen is whatever console the
 * program was started from - full screen, a desktop window, COM1 or a
 * remote session. Programs that write straight to text memory at B800:0000
 * get that mirrored onto the console too.
 *
 * Programs may hook interrupt vectors; unhooked vectors point at F000:n*4,
 * which is where the built-in handlers run.
 */
#include "../kernel.h"
#include "../vfs.h"

#define MEM_SIZE	(0x100000 + 0x10000)	/* 1 MiB plus the HMA wrap-around */
#define PSP_SEG		0x0800
#define ENV_SEG		0x0700
#define BIOS_SEG	0xF000
#define VRAM		0xB8000
#define COLS		80
#define ROWS		25
#define MAX_FILES	20

enum { AX, CX, DX, BX, SP, BP, SI, DI };
enum { ES, CS, SS, DS };

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

struct dosfile {
	struct vfile *f;
	int is_con;
};

struct cpu {
	uint16_t r[8];
	uint16_t s[4];
	uint16_t ip, fl;
	uint8_t *m;
	int running, exit_code;
	/* decoding */
	int seg_ovr, rep;
	int mod, reg, rm;
	uint32_t ea;			/* linear address of the memory operand */
	uint16_t ea_off;
	int ea_seg;
	/* DOS state */
	uint16_t dta_seg, dta_off;
	struct dosfile files[MAX_FILES];
	int retcode;
	/* screen */
	int screen_mode;		/* 1 once the program addresses the screen directly */
	int cx, cy;			/* text cursor */
	uint8_t shown[ROWS * COLS * 2];	/* what's on the console now */
	int teletype;			/* writes come from our own output, not the program */
	int keyq[16], kh, kt;
	uint64_t steps;
	char *errmsg;
	struct vstat *find;		/* FindFirst results */
	int nfind, ifind;
};

/* ---- memory ------------------------------------------------------------------- */

static inline uint32_t lin(uint16_t seg, uint16_t off)
{
	return (((uint32_t)seg << 4) + off) & 0xFFFFF;
}

static void enter_screen_mode(struct cpu *c);

static inline uint8_t rb(struct cpu *c, uint32_t a)
{
	return c->m[a & 0xFFFFF];
}

static inline void wb(struct cpu *c, uint32_t a, uint8_t v)
{
	a &= 0xFFFFF;
	if (a >= 0xF0000)
		return;		/* "ROM" */
	c->m[a] = v;
	if (a >= VRAM && a < VRAM + ROWS * COLS * 2 && !c->teletype && !c->screen_mode)
		enter_screen_mode(c);
}

static inline uint16_t rw(struct cpu *c, uint32_t a)
{
	return (uint16_t)(rb(c, a) | rb(c, a + 1) << 8);
}

static inline void ww(struct cpu *c, uint32_t a, uint16_t v)
{
	wb(c, a, (uint8_t)v);
	wb(c, a + 1, (uint8_t)(v >> 8));
}

static uint8_t fetch8(struct cpu *c)
{
	return rb(c, lin(c->s[CS], c->ip++));
}

static uint16_t fetch16(struct cpu *c)
{
	uint16_t v = rw(c, lin(c->s[CS], c->ip));
	c->ip += 2;
	return v;
}

static void push(struct cpu *c, uint16_t v)
{
	c->r[SP] -= 2;
	ww(c, lin(c->s[SS], c->r[SP]), v);
}

static uint16_t pop(struct cpu *c)
{
	uint16_t v = rw(c, lin(c->s[SS], c->r[SP]));
	c->r[SP] += 2;
	return v;
}

/* ---- registers ------------------------------------------------------------------- */

static inline uint8_t get8(struct cpu *c, int i)
{
	return i < 4 ? (uint8_t)c->r[i] : (uint8_t)(c->r[i - 4] >> 8);
}

static inline void set8(struct cpu *c, int i, uint8_t v)
{
	if (i < 4)
		c->r[i] = (uint16_t)((c->r[i] & 0xFF00) | v);
	else
		c->r[i - 4] = (uint16_t)((c->r[i - 4] & 0x00FF) | v << 8);
}

#define AL get8(c, 0)
#define AH get8(c, 4)
#define SET_AL(v) set8(c, 0, (uint8_t)(v))
#define SET_AH(v) set8(c, 4, (uint8_t)(v))
#define DL get8(c, 2)
#define DH get8(c, 6)
#define CL get8(c, 1)
#define CH get8(c, 5)
#define BL get8(c, 3)
#define BH get8(c, 7)

static inline void setf(struct cpu *c, uint16_t f, int on)
{
	if (on)
		c->fl |= f;
	else
		c->fl &= (uint16_t)~f;
}

static int parity(uint8_t v)
{
	v ^= v >> 4;
	v ^= v >> 2;
	v ^= v >> 1;
	return !(v & 1);
}

static void szp(struct cpu *c, uint32_t v, int w)
{
	uint32_t mask = w ? 0xFFFF : 0xFF;
	v &= mask;
	setf(c, F_ZF, v == 0);
	setf(c, F_SF, v & (w ? 0x8000 : 0x80));
	setf(c, F_PF, parity((uint8_t)v));
}

/* ---- ModRM ----------------------------------------------------------------------- */

static void modrm(struct cpu *c)
{
	uint8_t b = fetch8(c);
	c->mod = b >> 6;
	c->reg = (b >> 3) & 7;
	c->rm = b & 7;
	if (c->mod == 3)
		return;
	uint16_t off = 0;
	int seg = DS;
	switch (c->rm) {
	case 0: off = c->r[BX] + c->r[SI]; break;
	case 1: off = c->r[BX] + c->r[DI]; break;
	case 2: off = c->r[BP] + c->r[SI]; seg = SS; break;
	case 3: off = c->r[BP] + c->r[DI]; seg = SS; break;
	case 4: off = c->r[SI]; break;
	case 5: off = c->r[DI]; break;
	case 6:
		if (c->mod == 0)
			off = fetch16(c);
		else {
			off = c->r[BP];
			seg = SS;
		}
		break;
	case 7: off = c->r[BX]; break;
	}
	if (c->mod == 1)
		off += (uint16_t)(int16_t)(int8_t)fetch8(c);
	else if (c->mod == 2)
		off += fetch16(c);
	if (c->seg_ovr >= 0)
		seg = c->seg_ovr;
	c->ea_seg = seg;
	c->ea_off = off;
	c->ea = lin(c->s[seg], off);
}

static uint8_t rm8(struct cpu *c)
{
	return c->mod == 3 ? get8(c, c->rm) : rb(c, c->ea);
}

static void set_rm8(struct cpu *c, uint8_t v)
{
	if (c->mod == 3)
		set8(c, c->rm, v);
	else
		wb(c, c->ea, v);
}

static uint16_t rm16(struct cpu *c)
{
	return c->mod == 3 ? c->r[c->rm] : rw(c, c->ea);
}

static void set_rm16(struct cpu *c, uint16_t v)
{
	if (c->mod == 3)
		c->r[c->rm] = v;
	else
		ww(c, c->ea, v);
}

/* ---- arithmetic -------------------------------------------------------------------- */

/* op: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP */
static uint16_t alu(struct cpu *c, int op, uint16_t a, uint16_t b, int w)
{
	uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
	uint32_t res, carry = c->fl & F_CF;
	switch (op) {
	case 0:
	case 2:
		res = (uint32_t)a + b + (op == 2 ? carry : 0);
		setf(c, F_CF, res > mask);
		setf(c, F_OF, ((a ^ res) & (b ^ res) & sign) != 0);
		setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
		break;
	case 3:
	case 5:
	case 7:
		res = (uint32_t)a - b - (op == 3 ? carry : 0);
		setf(c, F_CF, (uint32_t)a < (uint32_t)b + (op == 3 ? carry : 0));
		setf(c, F_OF, ((a ^ b) & (a ^ res) & sign) != 0);
		setf(c, F_AF, ((a ^ b ^ res) & 0x10) != 0);
		break;
	case 1: res = a | b; goto logic;
	case 4: res = a & b; goto logic;
	default: res = a ^ b;
	logic:
		setf(c, F_CF, 0);
		setf(c, F_OF, 0);
		setf(c, F_AF, 0);
	}
	szp(c, res, w);
	return (uint16_t)(res & mask);
}

static uint16_t incdec(struct cpu *c, uint16_t v, int dec, int w)
{
	uint16_t cf = c->fl & F_CF;
	uint16_t r = alu(c, dec ? 5 : 0, v, 1, w);
	setf(c, F_CF, cf);	/* INC/DEC leave CF alone */
	return r;
}

static uint16_t shift(struct cpu *c, int op, uint16_t v, int count, int w)
{
	int bits = w ? 16 : 8;
	uint32_t mask = w ? 0xFFFF : 0xFF, sign = w ? 0x8000 : 0x80;
	count &= 0x1F;
	if (!count)
		return v;
	uint32_t x = v;
	for (int i = 0; i < count; i++) {
		uint32_t cf = c->fl & F_CF;
		switch (op) {
		case 0:	/* ROL */
			cf = (x & sign) != 0;
			x = ((x << 1) | cf) & mask;
			break;
		case 1:	/* ROR */
			cf = x & 1;
			x = (x >> 1) | (cf ? sign : 0);
			break;
		case 2:	/* RCL */
		{
			uint32_t out = (x & sign) != 0;
			x = ((x << 1) | cf) & mask;
			cf = out;
			break;
		}
		case 3:	/* RCR */
		{
			uint32_t out = x & 1;
			x = (x >> 1) | (cf ? sign : 0);
			cf = out;
			break;
		}
		case 4:
		case 6:	/* SHL/SAL */
			cf = (x & sign) != 0;
			x = (x << 1) & mask;
			break;
		case 5:	/* SHR */
			cf = x & 1;
			x >>= 1;
			break;
		case 7:	/* SAR */
			cf = x & 1;
			x = (x >> 1) | (x & sign);
			break;
		}
		setf(c, F_CF, cf);
	}
	/* OF is defined for single-bit shifts */
	switch (op) {
	case 0: case 2: case 4: case 6:
		setf(c, F_OF, ((x & sign) != 0) != ((c->fl & F_CF) != 0));
		break;
	case 1: case 3:
		setf(c, F_OF, ((x ^ (x << 1)) & sign) != 0);
		break;
	case 5:
		setf(c, F_OF, (v & sign) != 0);
		break;
	case 7:
		setf(c, F_OF, 0);
		break;
	}
	if (op >= 4)
		szp(c, x, w);
	(void)bits;
	return (uint16_t)x;
}

/* ---- the screen ---------------------------------------------------------------------- */

static void sync_screen(struct cpu *c)
{
	if (!c->screen_mode)
		return;
	for (int i = 0; i < ROWS * COLS; i++) {
		uint8_t ch = c->m[VRAM + i * 2], at = c->m[VRAM + i * 2 + 1];
		if (c->shown[i * 2] == ch && c->shown[i * 2 + 1] == at)
			continue;
		c->shown[i * 2] = ch;
		c->shown[i * 2 + 1] = at;
		con_gotoxy(i % COLS, i / COLS);
		con_attr(at & 0xF, (at >> 4) & 0x7);
		con_putc(ch < 0x20 ? ' ' : (char)ch);
	}
	con_gotoxy(c->cx, c->cy);
}

static void enter_screen_mode(struct cpu *c)
{
	if (c->screen_mode)
		return;
	c->screen_mode = 1;
	con_attr(7, 0);
	con_clear();
	memset(c->shown, 0, sizeof c->shown);	/* force a full paint */
}

static void scroll_up(struct cpu *c, int top, int left, int bottom, int right, int n, uint8_t attr)
{
	if (bottom >= ROWS) bottom = ROWS - 1;
	if (right >= COLS) right = COLS - 1;
	if (top > bottom || left > right)
		return;
	c->teletype = 1;
	int h = bottom - top + 1;
	if (n == 0 || n > h)
		n = h;
	for (int y = top; y <= bottom; y++)
		for (int x = left; x <= right; x++) {
			uint32_t d = VRAM + (uint32_t)(y * COLS + x) * 2;
			if (y + n <= bottom) {
				uint32_t s = VRAM + (uint32_t)((y + n) * COLS + x) * 2;
				c->m[d] = c->m[s];
				c->m[d + 1] = c->m[s + 1];
			} else {
				c->m[d] = ' ';
				c->m[d + 1] = attr;
			}
		}
	c->teletype = 0;
}

static void scroll_down(struct cpu *c, int top, int left, int bottom, int right, int n, uint8_t attr)
{
	if (bottom >= ROWS) bottom = ROWS - 1;
	if (right >= COLS) right = COLS - 1;
	int h = bottom - top + 1;
	if (n == 0 || n > h)
		n = h;
	for (int y = bottom; y >= top; y--)
		for (int x = left; x <= right; x++) {
			uint32_t d = VRAM + (uint32_t)(y * COLS + x) * 2;
			if (y - n >= top) {
				uint32_t s = VRAM + (uint32_t)((y - n) * COLS + x) * 2;
				c->m[d] = c->m[s];
				c->m[d + 1] = c->m[s + 1];
			} else {
				c->m[d] = ' ';
				c->m[d + 1] = attr;
			}
		}
}

/* Character output the way DOS/BIOS teletype does it. */
static void tty(struct cpu *c, uint8_t ch)
{
	if (ch == '\t') {
		do
			tty(c, ' ');
		while (c->cx % 8);
		return;
	}
	if (!c->screen_mode)
		con_putc((char)ch);
	switch (ch) {
	case '\r':
		c->cx = 0;
		return;
	case '\n':
		break;
	case '\b':
		if (c->cx)
			c->cx--;
		return;
	case 7:
		return;
	default:
		c->teletype = 1;
		c->m[VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2] = ch;
		if (!c->m[VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2 + 1])
			c->m[VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2 + 1] = 7;
		c->teletype = 0;
		if (++c->cx < COLS)
			return;
		c->cx = 0;
	}
	if (++c->cy >= ROWS) {
		c->cy = ROWS - 1;
		scroll_up(c, 0, 0, ROWS - 1, COLS - 1, 1, 7);
	}
}

static void out_str(struct cpu *c, const char *s)
{
	while (*s)
		tty(c, (uint8_t)*s++);
}

/* ---- the keyboard ------------------------------------------------------------------- */

static const uint8_t scancodes[128] = {
	['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9,
	['9'] = 10, ['0'] = 11, ['-'] = 12, ['='] = 13, ['q'] = 16, ['w'] = 17, ['e'] = 18,
	['r'] = 19, ['t'] = 20, ['y'] = 21, ['u'] = 22, ['i'] = 23, ['o'] = 24, ['p'] = 25,
	['['] = 26, [']'] = 27, ['a'] = 30, ['s'] = 31, ['d'] = 32, ['f'] = 33, ['g'] = 34,
	['h'] = 35, ['j'] = 36, ['k'] = 37, ['l'] = 38, [';'] = 39, ['\''] = 40, ['`'] = 41,
	['\\'] = 43, ['z'] = 44, ['x'] = 45, ['c'] = 46, ['v'] = 47, ['b'] = 48, ['n'] = 49,
	['m'] = 50, [','] = 51, ['.'] = 52, ['/'] = 53, [' '] = 57,
};

/* bdos key -> BIOS scan<<8 | ascii */
static uint16_t bios_key(int k)
{
	switch (k) {
	case '\n': return 0x1C0D;
	case 27: return 0x011B;
	case '\b': return 0x0E08;
	case '\t': return 0x0F09;
	case KEY_UP: return 0x4800;
	case KEY_DOWN: return 0x5000;
	case KEY_LEFT: return 0x4B00;
	case KEY_RIGHT: return 0x4D00;
	case KEY_HOME: return 0x4700;
	case KEY_END: return 0x4F00;
	case KEY_PGUP: return 0x4900;
	case KEY_PGDN: return 0x5100;
	case KEY_DEL: return 0x5300;
	case KEY_INS: return 0x5200;
	}
	if (k >= KEY_F1 && k < KEY_F1 + 10)
		return (uint16_t)((0x3B + k - KEY_F1) << 8);
	if (k == KEY_F1 + 10)
		return 0x8500;
	if (k == KEY_F1 + 11)
		return 0x8600;
	if (k >= 0 && k < 128)
		return (uint16_t)(scancodes[tolower(k)] << 8 | k);
	return 0;
}

static void poll_keys(struct cpu *c)
{
	int k;
	while ((k = con_pollkey()) >= 0) {
		if (k == 3 || k == KEY_EOF) {	/* Ctrl-C, or our window closed */
			c->running = 0;
			c->exit_code = 3;
			c->errmsg = k == 3 ? "^C" : NULL;
			return;
		}
		int next = (c->kh + 1) % 16;
		if (next != c->kt) {
			c->keyq[c->kh] = k;
			c->kh = next;
		}
	}
}

static int key_ready(struct cpu *c)
{
	poll_keys(c);
	return c->kh != c->kt;
}

static int get_key(struct cpu *c)
{
	sync_screen(c);
	while (c->running && !key_ready(c))
		task_idle();
	if (!c->running)
		return 0;
	int k = c->keyq[c->kt];
	c->kt = (c->kt + 1) % 16;
	return k;
}

/* ---- DOS services ---------------------------------------------------------------------- */

static void get_asciiz(struct cpu *c, uint16_t seg, uint16_t off, char *out, size_t n)
{
	size_t i = 0;
	for (; i + 1 < n; i++) {
		char ch = (char)rb(c, lin(seg, (uint16_t)(off + i)));
		if (!ch)
			break;
		out[i] = ch;
	}
	out[i] = 0;
}

static void ok(struct cpu *c)
{
	setf(c, F_CF, 0);
}

static void err(struct cpu *c, uint16_t code)
{
	setf(c, F_CF, 1);
	c->r[AX] = code;
}

static uint16_t dos_error(int e)
{
	switch (e) {
	case E_NOENT: return 2;
	case E_NOTDIR: return 3;
	case E_ACCES: case E_ISDIR: case E_EXIST: case E_NOTEMPTY: return 5;
	case E_NOSPC: return 0x27;
	case E_BADDRIVE: return 0x0F;
	}
	return 0x1F;	/* general failure */
}

static int dos_path(struct cpu *c, uint16_t seg, uint16_t off, char *abs, size_t n)
{
	char p[128];
	get_asciiz(c, seg, off, p, sizeof p);
	return resolve_path(p, abs, n);
}

static int new_handle(struct cpu *c)
{
	for (int i = 5; i < MAX_FILES; i++)
		if (!c->files[i].f && !c->files[i].is_con)
			return i;
	return -1;
}

static void dos_open(struct cpu *c, int create)
{
	char abs[VFS_PATH_MAX];
	int e = dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs);
	if (e) {
		err(c, 3);
		return;
	}
	int h = new_handle(c);
	if (h < 0) {
		err(c, 4);
		return;
	}
	int mode = AL & 3;
	int flags = create ? (O_READ | O_WRITE | O_CREATE | O_TRUNC) :
	    mode == 0 ? O_READ : (O_READ | O_WRITE);
	struct vfile *f = vfs_open(abs, flags, &e);
	if (!f) {
		err(c, dos_error(e));
		return;
	}
	c->files[h].f = f;
	c->files[h].is_con = vfs_isdev(f) && !strcasecmp(vfs_basename(abs), "CON");
	c->r[AX] = (uint16_t)h;
	ok(c);
}

static void dos_read(struct cpu *c)
{
	int h = c->r[BX];
	uint16_t n = c->r[CX];
	uint32_t buf = lin(c->s[DS], c->r[DX]);
	if (h == 0 || (h < MAX_FILES && c->files[h].is_con)) {
		/* cooked console input: one line, ending in CR LF */
		char line[256];
		sync_screen(c);
		c->teletype = 1;
		int len = 0;
		for (;;) {
			int k = get_key(c);
			if (!c->running || k == KEY_EOF)
				break;
			if (k == '\n')
				break;
			if (k == '\b') {
				if (len) {
					len--;
					out_str(c, "\b \b");
				}
				continue;
			}
			if (k >= 0x20 && k < 0x7F && len < (int)sizeof line - 3) {
				line[len++] = (char)k;
				tty(c, (uint8_t)k);
			}
		}
		c->teletype = 0;
		out_str(c, "\r\n");
		line[len++] = '\r';
		line[len++] = '\n';
		uint16_t take = (uint16_t)MIN(len, (int)n);
		for (int i = 0; i < take; i++)
			wb(c, buf + (uint32_t)i, (uint8_t)line[i]);
		c->r[AX] = take;
		ok(c);
		return;
	}
	if (h >= MAX_FILES || !c->files[h].f) {
		err(c, 6);
		return;
	}
	uint8_t *tmp = kmalloc(n ? n : 1);
	if (!tmp) {
		err(c, 8);
		return;
	}
	long got = vfs_read(c->files[h].f, tmp, n);
	if (got < 0) {
		kfree(tmp);
		err(c, dos_error((int)got));
		return;
	}
	for (long i = 0; i < got; i++)
		wb(c, buf + (uint32_t)i, tmp[i]);
	kfree(tmp);
	c->r[AX] = (uint16_t)got;
	ok(c);
}

static void dos_write(struct cpu *c)
{
	int h = c->r[BX];
	uint16_t n = c->r[CX];
	uint32_t buf = lin(c->s[DS], c->r[DX]);
	if (h == 1 || h == 2 || (h < MAX_FILES && c->files[h].is_con)) {
		c->teletype = 1;
		for (uint16_t i = 0; i < n; i++)
			tty(c, rb(c, buf + i));
		c->teletype = 0;
		c->r[AX] = n;
		ok(c);
		return;
	}
	if (h == 3 || h == 4) {		/* AUX, PRN */
		c->r[AX] = n;
		ok(c);
		return;
	}
	if (h >= MAX_FILES || !c->files[h].f) {
		err(c, 6);
		return;
	}
	if (!n) {	/* DOS: writing 0 bytes truncates at the current position */
		c->r[AX] = 0;
		ok(c);
		return;
	}
	uint8_t *tmp = kmalloc(n);
	if (!tmp) {
		err(c, 8);
		return;
	}
	for (uint16_t i = 0; i < n; i++)
		tmp[i] = rb(c, buf + i);
	long w = vfs_write(c->files[h].f, tmp, n);
	kfree(tmp);
	if (w < 0) {
		err(c, dos_error((int)w));
		return;
	}
	c->r[AX] = (uint16_t)w;
	ok(c);
}

/* DTA layout used by FindFirst/FindNext */
static void fill_dta(struct cpu *c, struct vstat *st)
{
	uint32_t d = lin(c->dta_seg, c->dta_off);
	int y, mo, dd, h, mi, s;
	epoch_to_date(st->mtime, &y, &mo, &dd, &h, &mi, &s);
	wb(c, d + 21, st->attr);
	ww(c, d + 22, (uint16_t)(h << 11 | mi << 5 | s / 2));
	ww(c, d + 24, (uint16_t)((y - 1980) << 9 | mo << 5 | dd));
	ww(c, d + 26, (uint16_t)st->size);
	ww(c, d + 28, (uint16_t)(st->size >> 16));
	const char *name = st->short_name[0] ? st->short_name : st->name;
	int i = 0;
	for (; name[i] && i < 12; i++)
		wb(c, d + 30 + (uint32_t)i, (uint8_t)toupper(name[i]));
	wb(c, d + 30 + (uint32_t)i, 0);
}

struct find_ctx {
	struct cpu *c;
	const char *pat;
	uint8_t attr;
};

static int find_cb(void *arg, struct vstat *st)
{
	struct find_ctx *f = arg;
	struct cpu *c = f->c;
	if ((st->attr & (A_HIDDEN | A_SYSTEM | A_DIR)) & ~f->attr)
		return 0;
	if (!wildmatch(f->pat, st->short_name) && !wildmatch(f->pat, st->name))
		return 0;
	struct vstat *n = krealloc(c->find, sizeof(struct vstat) * (size_t)(c->nfind + 1));
	if (!n)
		return 1;
	c->find = n;
	c->find[c->nfind++] = *st;
	return 0;
}

static void dos_findfirst(struct cpu *c)
{
	char abs[VFS_PATH_MAX], dir[VFS_PATH_MAX];
	if (dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs)) {
		err(c, 3);
		return;
	}
	vfs_dirname(abs, dir, sizeof dir);
	kfree(c->find);
	c->find = NULL;
	c->nfind = c->ifind = 0;
	struct find_ctx f = { c, vfs_basename(abs), (uint8_t)c->r[CX] };
	if (vfs_list(dir, find_cb, &f) || !c->nfind) {
		err(c, 0x12);
		return;
	}
	fill_dta(c, &c->find[c->ifind++]);
	ok(c);
}

static void dos_findnext(struct cpu *c)
{
	if (c->ifind >= c->nfind) {
		err(c, 0x12);
		return;
	}
	fill_dta(c, &c->find[c->ifind++]);
	ok(c);
}

static void int21(struct cpu *c)
{
	uint8_t fn = AH;
	char abs[VFS_PATH_MAX], abs2[VFS_PATH_MAX];
	int e;
	switch (fn) {
	case 0x00:
		c->running = 0;
		c->exit_code = 0;
		break;
	case 0x01: {
		int k = get_key(c);
		SET_AL(bios_key(k) & 0xFF);
		c->teletype = 1;
		tty(c, AL == '\r' ? '\r' : AL);
		c->teletype = 0;
		break;
	}
	case 0x02:
		c->teletype = 1;
		tty(c, DL);
		c->teletype = 0;
		break;
	case 0x06:
		if (DL == 0xFF) {
			if (key_ready(c)) {
				int k = get_key(c);
				uint16_t bk = bios_key(k);
				SET_AL(bk & 0xFF ? bk & 0xFF : 0);
				if (!(bk & 0xFF)) {	/* extended: return 0 now, scan code next */
					int next = (c->kt + 15) % 16;
					c->keyq[next] = (bk >> 8) | 0x10000;
					c->kt = next;
				}
				setf(c, F_ZF, 0);
			} else {
				SET_AL(0);
				setf(c, F_ZF, 1);
			}
		} else {
			c->teletype = 1;
			tty(c, DL);
			c->teletype = 0;
		}
		break;
	case 0x07:
	case 0x08: {
		int k = get_key(c);
		if (k & 0x10000)	/* second half of an extended key */
			SET_AL(k & 0xFF);
		else {
			uint16_t bk = bios_key(k);
			if (bk & 0xFF)
				SET_AL(bk & 0xFF);
			else {
				SET_AL(0);
				int next = (c->kt + 15) % 16;
				c->keyq[next] = (bk >> 8) | 0x10000;
				c->kt = next;
			}
		}
		break;
	}
	case 0x09: {
		uint16_t off = c->r[DX];
		c->teletype = 1;
		for (int i = 0; i < 65535; i++) {
			uint8_t ch = rb(c, lin(c->s[DS], (uint16_t)(off + i)));
			if (ch == '$')
				break;
			tty(c, ch);
		}
		c->teletype = 0;
		SET_AL('$');
		break;
	}
	case 0x0A: {	/* buffered input: [max][len][chars...] */
		uint32_t b = lin(c->s[DS], c->r[DX]);
		int max = rb(c, b);
		int len = 0;
		c->teletype = 1;
		for (;;) {
			int k = get_key(c);
			if (!c->running || k == '\n')
				break;
			if (k == '\b') {
				if (len) {
					len--;
					out_str(c, "\b \b");
				}
			} else if (k >= 0x20 && k < 0x7F && len < max - 1) {
				wb(c, b + 2 + (uint32_t)len++, (uint8_t)k);
				tty(c, (uint8_t)k);
			}
		}
		wb(c, b + 2 + (uint32_t)len, '\r');
		wb(c, b + 1, (uint8_t)len);
		tty(c, '\r');
		c->teletype = 0;
		break;
	}
	case 0x0B:
		SET_AL(key_ready(c) ? 0xFF : 0);
		break;
	case 0x0C:
		c->kh = c->kt = 0;
		if (AL == 1 || AL == 6 || AL == 7 || AL == 8 || AL == 0x0A) {
			c->r[AX] = (uint16_t)(AL << 8 | (c->r[AX] & 0xFF));
			int21(c);
		}
		break;
	case 0x0D:
		break;
	case 0x0E:
		if (vfs_drive((char)('A' + DL)))
			snprintf(task_current()->cwd, sizeof task_current()->cwd, "%c:\\", 'A' + DL);
		SET_AL(26);
		break;
	case 0x19:
		SET_AL(task_current()->cwd[0] - 'A');
		break;
	case 0x1A:
		c->dta_seg = c->s[DS];
		c->dta_off = c->r[DX];
		break;
	case 0x25:
		ww(c, (uint32_t)AL * 4, c->r[DX]);
		ww(c, (uint32_t)AL * 4 + 2, c->s[DS]);
		break;
	case 0x2A: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		c->r[CX] = (uint16_t)y;
		set8(c, 6, (uint8_t)mo);
		set8(c, 2, (uint8_t)d);
		SET_AL((rtc_epoch() / 86400 + 4) % 7);
		break;
	}
	case 0x2C: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		set8(c, 5, (uint8_t)h);
		set8(c, 1, (uint8_t)mi);
		set8(c, 6, (uint8_t)s);
		set8(c, 2, (uint8_t)((uptime_ms() / 10) % 100));
		break;
	}
	case 0x2F:
		c->s[ES] = c->dta_seg;
		c->r[BX] = c->dta_off;
		break;
	case 0x30:
		c->r[AX] = 0x0005;	/* "DOS 5.0" */
		c->r[BX] = 0;
		c->r[CX] = 0;
		break;
	case 0x33:
		set8(c, 2, 0);
		break;
	case 0x35:
		c->r[BX] = rw(c, (uint32_t)AL * 4);
		c->s[ES] = rw(c, (uint32_t)AL * 4 + 2);
		break;
	case 0x36: {
		char label[32];
		uint64_t free = 0, total = 0;
		if (vfs_info(DL ? (char)('A' + DL - 1) : task_current()->cwd[0], label, sizeof label,
		    &free, &total)) {
			c->r[AX] = 0xFFFF;
			break;
		}
		c->r[AX] = 64;	/* sectors per cluster */
		c->r[CX] = 512;
		c->r[BX] = (uint16_t)MIN(free / 32768, 65535);
		c->r[DX] = (uint16_t)MIN(total / 32768, 65535);
		break;
	}
	case 0x39:
	case 0x3A:
	case 0x41:
		if ((e = dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs))) {
			err(c, 3);
			break;
		}
		e = fn == 0x39 ? vfs_mkdir(abs) : fn == 0x3A ? vfs_rmdir(abs) : vfs_delete(abs);
		if (e)
			err(c, dos_error(e));
		else
			ok(c);
		break;
	case 0x3B: {
		struct vstat st;
		if (dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs) || vfs_stat(abs, &st) ||
		    st.type != VFS_DIR) {
			err(c, 3);
			break;
		}
		vfs_canon(abs, task_current()->cwd, sizeof task_current()->cwd);
		ok(c);
		break;
	}
	case 0x3C:
		dos_open(c, 1);
		break;
	case 0x3D:
		dos_open(c, 0);
		break;
	case 0x3E: {
		int h = c->r[BX];
		if (h < 5) {
			ok(c);
			break;
		}
		if (h >= MAX_FILES || !c->files[h].f) {
			err(c, 6);
			break;
		}
		vfs_close(c->files[h].f);
		c->files[h].f = NULL;
		c->files[h].is_con = 0;
		ok(c);
		break;
	}
	case 0x3F:
		dos_read(c);
		break;
	case 0x40:
		dos_write(c);
		break;
	case 0x42: {
		int h = c->r[BX];
		if (h < 5 || h >= MAX_FILES || !c->files[h].f) {
			if (h < 5) {
				c->r[AX] = c->r[DX] = 0;
				ok(c);
			} else {
				err(c, 6);
			}
			break;
		}
		int32_t off = (int32_t)((uint32_t)c->r[CX] << 16 | c->r[DX]);
		struct vfile *f = c->files[h].f;
		int64_t base = AL == 0 ? 0 : AL == 1 ? (int64_t)vfs_tell(f) : (int64_t)vfs_size(f);
		int64_t pos = base + off;
		if (pos < 0) {
			err(c, 0x19);
			break;
		}
		vfs_seek(f, (uint64_t)pos);
		c->r[AX] = (uint16_t)pos;
		c->r[DX] = (uint16_t)(pos >> 16);
		ok(c);
		break;
	}
	case 0x43: {
		struct vstat st;
		if (dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs) || (e = vfs_stat(abs, &st))) {
			err(c, 2);
			break;
		}
		if (AL == 0) {
			c->r[CX] = st.attr;
			ok(c);
		} else {
			e = vfs_setattr(abs, (uint8_t)c->r[CX]);
			if (e)
				err(c, dos_error(e));
			else
				ok(c);
		}
		break;
	}
	case 0x44:
		if (AL == 0) {	/* device information */
			int h = c->r[BX];
			int dev = h < 3 || (h < MAX_FILES && c->files[h].is_con);
			c->r[DX] = dev ? 0x80D3 : 0x0000;
			ok(c);
		} else if (AL == 8) {
			c->r[AX] = 1;	/* not removable */
			ok(c);
		} else {
			err(c, 1);
		}
		break;
	case 0x45:
	case 0x46:
		err(c, 4);	/* handle duplication isn't supported */
		break;
	case 0x47: {	/* current directory, without "C:\" */
		const char *cwd = task_current()->cwd + 3;
		uint32_t d = lin(c->s[DS], c->r[SI]);
		int i = 0;
		for (; cwd[i] && i < 63; i++)
			wb(c, d + (uint32_t)i, (uint8_t)toupper(cwd[i]));
		wb(c, d + (uint32_t)i, 0);
		ok(c);
		break;
	}
	case 0x48:
		c->r[BX] = 0;	/* .COM programs already own all memory */
		err(c, 8);
		break;
	case 0x49:
	case 0x4A:
		ok(c);
		break;
	case 0x4C:
		c->running = 0;
		c->exit_code = AL;
		break;
	case 0x4D:
		c->r[AX] = (uint16_t)c->retcode;
		break;
	case 0x4E:
		dos_findfirst(c);
		break;
	case 0x4F:
		dos_findnext(c);
		break;
	case 0x56:
		if (dos_path(c, c->s[DS], c->r[DX], abs, sizeof abs) ||
		    dos_path(c, c->s[ES], c->r[DI], abs2, sizeof abs2)) {
			err(c, 3);
			break;
		}
		e = vfs_rename(abs, abs2);
		if (e)
			err(c, dos_error(e));
		else
			ok(c);
		break;
	case 0x57:
		if (AL == 0) {
			c->r[CX] = 0;
			c->r[DX] = (1 << 5) | 1;
		}
		ok(c);
		break;
	case 0x62:
		c->r[BX] = PSP_SEG;
		break;
	default:
		err(c, 1);	/* invalid function */
	}
}

static void int10(struct cpu *c)
{
	switch (AH) {
	case 0x00:	/* set mode: clear the screen */
		enter_screen_mode(c);
		scroll_up(c, 0, 0, ROWS - 1, COLS - 1, 0, 7);
		c->cx = c->cy = 0;
		break;
	case 0x01:
	case 0x05:
	case 0x11:
	case 0x12:
		break;
	case 0x02:
		if (DH != c->cy && !c->screen_mode)
			enter_screen_mode(c);
		c->cx = MIN(DL, COLS - 1);
		c->cy = MIN(DH, ROWS - 1);
		if (!c->screen_mode)
			con_gotoxy(c->cx, c->cy);
		break;
	case 0x03:
		set8(c, 6, (uint8_t)c->cy);
		set8(c, 2, (uint8_t)c->cx);
		c->r[CX] = 0x0607;
		break;
	case 0x06:
	case 0x07:
		enter_screen_mode(c);
		if (AH == 6)
			scroll_up(c, CH, CL, DH, DL, AL, BH);
		else
			scroll_down(c, CH, CL, DH, DL, AL, BH);
		break;
	case 0x08: {
		uint32_t a = VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2;
		c->r[AX] = (uint16_t)(c->m[a] | c->m[a + 1] << 8);
		break;
	}
	case 0x09:
	case 0x0A: {
		enter_screen_mode(c);
		uint32_t a = VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2;
		for (int i = 0; i < c->r[CX] && a < VRAM + ROWS * COLS * 2; i++, a += 2) {
			c->m[a] = AL;
			if (AH == 9)
				c->m[a + 1] = BL;
		}
		break;
	}
	case 0x0E:
		c->teletype = 1;
		tty(c, AL);
		c->teletype = 0;
		break;
	case 0x0F:
		c->r[AX] = 0x5003;	/* 80 columns, mode 3 */
		set8(c, 7, 0);
		break;
	case 0x13: {	/* write string */
		enter_screen_mode(c);
		int save_x = c->cx, save_y = c->cy;
		c->cx = DL;
		c->cy = DH;
		uint32_t s = lin(c->s[ES], c->r[BP]);
		c->teletype = 1;
		for (int i = 0; i < c->r[CX]; i++) {
			uint8_t ch = rb(c, s + (uint32_t)(AL & 2 ? i * 2 : i));
			uint8_t at = AL & 2 ? rb(c, s + (uint32_t)i * 2 + 1) : BL;
			uint32_t a = VRAM + (uint32_t)(c->cy * COLS + c->cx) * 2;
			if (ch >= 0x20) {
				c->m[a] = ch;
				c->m[a + 1] = at;
				if (++c->cx >= COLS) {
					c->cx = 0;
					c->cy = MIN(c->cy + 1, ROWS - 1);
				}
			} else {
				tty(c, ch);
			}
		}
		c->teletype = 0;
		if (!(AL & 1)) {
			c->cx = save_x;
			c->cy = save_y;
		}
		break;
	}
	case 0x1A:
		SET_AL(0x1A);
		set8(c, 3, 8);	/* VGA with colour */
		break;
	}
}

static void int16(struct cpu *c)
{
	switch (AH) {
	case 0x00:
	case 0x10: {
		int k = get_key(c);
		c->r[AX] = bios_key(k);
		break;
	}
	case 0x01:
	case 0x11:
		sync_screen(c);
		if (key_ready(c)) {
			c->r[AX] = bios_key(c->keyq[c->kt]);
			setf(c, F_ZF, 0);
		} else {
			setf(c, F_ZF, 1);
			task_idle();	/* polling loops shouldn't hog the machine */
		}
		break;
	case 0x02:
		SET_AL(0);
		break;
	}
}

static uint8_t to_bcd(int v)
{
	return (uint8_t)((v / 10) << 4 | (v % 10));
}

static void int1a(struct cpu *c)
{
	int y, mo, d, h, mi, s;
	switch (AH) {
	case 0x00: {
		uint32_t ticks = (uint32_t)(uptime_ms() * 182 / 10000);
		c->r[CX] = (uint16_t)(ticks >> 16);
		c->r[DX] = (uint16_t)ticks;
		SET_AL(0);
		break;
	}
	case 0x02:
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		set8(c, 5, to_bcd(h));
		set8(c, 1, to_bcd(mi));
		set8(c, 6, to_bcd(s));
		setf(c, F_CF, 0);
		break;
	case 0x04:
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		set8(c, 5, to_bcd(y / 100));
		set8(c, 1, to_bcd(y % 100));
		set8(c, 6, to_bcd(mo));
		set8(c, 2, to_bcd(d));
		setf(c, F_CF, 0);
		break;
	}
}

/* A built-in interrupt handler. */
static void hle_int(struct cpu *c, int n)
{
	switch (n) {
	case 0x00:
		c->errmsg = "Divide overflow";
		c->running = 0;
		c->exit_code = 255;
		break;
	case 0x10: int10(c); break;
	case 0x16: int16(c); break;
	case 0x1A: int1a(c); break;
	case 0x20:
		c->running = 0;
		c->exit_code = 0;
		break;
	case 0x21: int21(c); break;
	case 0x29:
		c->teletype = 1;
		tty(c, AL);
		c->teletype = 0;
		break;
	case 0x33:
		c->r[AX] = 0;	/* no mouse driver */
		break;
	case 0x2F:
		SET_AL(0);
		break;
	default:
		break;		/* timer, Ctrl-Break and friends: nothing to do */
	}
}

static void do_int(struct cpu *c, int n)
{
	uint16_t off = rw(c, (uint32_t)n * 4), seg = rw(c, (uint32_t)n * 4 + 2);
	if (seg == BIOS_SEG && off == (uint16_t)(n * 4)) {
		/* the vector is ours: run the built-in handler directly */
		hle_int(c, n);
		return;
	}
	push(c, c->fl);
	push(c, c->s[CS]);
	push(c, c->ip);
	setf(c, F_IF, 0);
	setf(c, F_TF, 0);
	c->s[CS] = seg;
	c->ip = off;
}

/* ---- the CPU ----------------------------------------------------------------------------- */

static int cond(struct cpu *c, int cc)
{
	int cf = c->fl & F_CF, zf = c->fl & F_ZF, sf = !!(c->fl & F_SF), of = !!(c->fl & F_OF);
	int r;
	switch (cc >> 1) {
	case 0: r = of; break;
	case 1: r = cf; break;
	case 2: r = zf; break;
	case 3: r = cf || zf; break;
	case 4: r = sf; break;
	case 5: r = !!(c->fl & F_PF); break;
	case 6: r = sf != of; break;
	default: r = zf || (sf != of); break;
	}
	return (cc & 1) ? !r : r;
}

static void string_op(struct cpu *c, uint8_t op)
{
	int w = op & 1;
	int d = (c->fl & F_DF) ? -(1 + w) : (1 + w);
	int seg = c->seg_ovr >= 0 ? c->seg_ovr : DS;
	int rep = c->rep;
	if (rep && !c->r[CX])
		return;
	for (;;) {
		switch (op & ~1) {
		case 0xA4:	/* MOVS */
			if (w)
				ww(c, lin(c->s[ES], c->r[DI]), rw(c, lin(c->s[seg], c->r[SI])));
			else
				wb(c, lin(c->s[ES], c->r[DI]), rb(c, lin(c->s[seg], c->r[SI])));
			c->r[SI] += (uint16_t)d;
			c->r[DI] += (uint16_t)d;
			break;
		case 0xA6: {	/* CMPS */
			uint16_t a = w ? rw(c, lin(c->s[seg], c->r[SI])) : rb(c, lin(c->s[seg], c->r[SI]));
			uint16_t b = w ? rw(c, lin(c->s[ES], c->r[DI])) : rb(c, lin(c->s[ES], c->r[DI]));
			alu(c, 7, a, b, w);
			c->r[SI] += (uint16_t)d;
			c->r[DI] += (uint16_t)d;
			break;
		}
		case 0xAA:	/* STOS */
			if (w)
				ww(c, lin(c->s[ES], c->r[DI]), c->r[AX]);
			else
				wb(c, lin(c->s[ES], c->r[DI]), AL);
			c->r[DI] += (uint16_t)d;
			break;
		case 0xAC:	/* LODS */
			if (w)
				c->r[AX] = rw(c, lin(c->s[seg], c->r[SI]));
			else
				SET_AL(rb(c, lin(c->s[seg], c->r[SI])));
			c->r[SI] += (uint16_t)d;
			break;
		case 0xAE: {	/* SCAS */
			uint16_t b = w ? rw(c, lin(c->s[ES], c->r[DI])) : rb(c, lin(c->s[ES], c->r[DI]));
			alu(c, 7, w ? c->r[AX] : AL, b, w);
			c->r[DI] += (uint16_t)d;
			break;
		}
		}
		if (!rep)
			return;
		if (--c->r[CX] == 0)
			return;
		if ((op & ~1) == 0xA6 || (op & ~1) == 0xAE) {
			int zf = !!(c->fl & F_ZF);
			if ((rep == 1 && !zf) || (rep == 2 && zf))
				return;
		}
	}
}

static void step(struct cpu *c)
{
	c->seg_ovr = -1;
	c->rep = 0;
	uint8_t op;
	for (;;) {	/* prefixes */
		op = fetch8(c);
		if (op == 0x26) c->seg_ovr = ES;
		else if (op == 0x2E) c->seg_ovr = CS;
		else if (op == 0x36) c->seg_ovr = SS;
		else if (op == 0x3E) c->seg_ovr = DS;
		else if (op == 0xF3) c->rep = 1;
		else if (op == 0xF2) c->rep = 2;
		else if (op == 0xF0) ;	/* LOCK */
		else break;
	}

	if (op < 0x40 && (op & 7) < 6) {	/* the ALU block */
		int aop = op >> 3, w = op & 1;
		switch (op & 7) {
		case 0: modrm(c); { uint8_t r = (uint8_t)alu(c, aop, rm8(c), get8(c, c->reg), 0); if (aop != 7) set_rm8(c, r); } return;
		case 1: modrm(c); { uint16_t r = alu(c, aop, rm16(c), c->r[c->reg], 1); if (aop != 7) set_rm16(c, r); } return;
		case 2: modrm(c); { uint8_t r = (uint8_t)alu(c, aop, get8(c, c->reg), rm8(c), 0); if (aop != 7) set8(c, c->reg, r); } return;
		case 3: modrm(c); { uint16_t r = alu(c, aop, c->r[c->reg], rm16(c), 1); if (aop != 7) c->r[c->reg] = r; } return;
		case 4: { uint8_t r = (uint8_t)alu(c, aop, AL, fetch8(c), 0); if (aop != 7) SET_AL(r); } return;
		case 5: { uint16_t r = alu(c, aop, c->r[AX], fetch16(c), 1); if (aop != 7) c->r[AX] = r; } return;
		}
		(void)w;
	}

	switch (op) {
	case 0x06: push(c, c->s[ES]); break;
	case 0x07: c->s[ES] = pop(c); break;
	case 0x0E: push(c, c->s[CS]); break;
	case 0x16: push(c, c->s[SS]); break;
	case 0x17: c->s[SS] = pop(c); break;
	case 0x1E: push(c, c->s[DS]); break;
	case 0x1F: c->s[DS] = pop(c); break;
	case 0x27: case 0x2F: {	/* DAA, DAS */
		uint8_t al = AL, oal = al;
		int cf = c->fl & F_CF;
		if ((al & 0xF) > 9 || (c->fl & F_AF)) {
			al = (uint8_t)(op == 0x27 ? al + 6 : al - 6);
			setf(c, F_AF, 1);
		} else
			setf(c, F_AF, 0);
		if (oal > 0x99 || cf) {
			al = (uint8_t)(op == 0x27 ? al + 0x60 : al - 0x60);
			setf(c, F_CF, 1);
		} else
			setf(c, F_CF, 0);
		SET_AL(al);
		szp(c, al, 0);
		break;
	}
	case 0x37: case 0x3F: {	/* AAA, AAS */
		if ((AL & 0xF) > 9 || (c->fl & F_AF)) {
			if (op == 0x37) {
				c->r[AX] += 0x106;
			} else {
				SET_AL(AL - 6);
				SET_AH(AH - 1);
			}
			setf(c, F_AF | F_CF, 1);
		} else
			setf(c, F_AF | F_CF, 0);
		SET_AL(AL & 0xF);
		break;
	}
	case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
		c->r[op & 7] = incdec(c, c->r[op & 7], 0, 1);
		break;
	case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
		c->r[op & 7] = incdec(c, c->r[op & 7], 1, 1);
		break;
	case 0x50: case 0x51: case 0x52: case 0x53: case 0x55: case 0x56: case 0x57:
		push(c, c->r[op & 7]);
		break;
	case 0x54: {	/* PUSH SP pushes the new value on 8086 */
		uint16_t v = (uint16_t)(c->r[SP] - 2);
		push(c, v);
		break;
	}
	case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
		c->r[op & 7] = pop(c);
		break;
	case 0x60: {	/* PUSHA */
		uint16_t sp = c->r[SP];
		for (int i = 0; i < 8; i++)
			push(c, i == SP ? sp : c->r[i]);
		break;
	}
	case 0x61:	/* POPA */
		for (int i = 7; i >= 0; i--) {
			uint16_t v = pop(c);
			if (i != SP)
				c->r[i] = v;
		}
		break;
	case 0x62:	/* BOUND: check and carry on */
		modrm(c);
		break;
	case 0x68: push(c, fetch16(c)); break;
	case 0x6A: push(c, (uint16_t)(int16_t)(int8_t)fetch8(c)); break;
	case 0x69: case 0x6B: {	/* IMUL r16, r/m16, imm */
		modrm(c);
		int32_t a = (int16_t)rm16(c);
		int32_t b = op == 0x69 ? (int16_t)fetch16(c) : (int8_t)fetch8(c);
		int32_t r = a * b;
		c->r[c->reg] = (uint16_t)r;
		setf(c, F_CF | F_OF, r != (int16_t)r);
		break;
	}
	case 0x6C: case 0x6D: case 0x6E: case 0x6F:	/* INS/OUTS: no ports here */
		break;
	case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
	case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
		int8_t d = (int8_t)fetch8(c);
		if (cond(c, op & 0xF))
			c->ip += (uint16_t)d;
		break;
	}
	case 0x80: case 0x82: {
		modrm(c);
		uint8_t imm = fetch8(c);
		uint8_t r = (uint8_t)alu(c, c->reg, rm8(c), imm, 0);
		if (c->reg != 7)
			set_rm8(c, r);
		break;
	}
	case 0x81: case 0x83: {
		modrm(c);
		uint16_t imm = op == 0x81 ? fetch16(c) : (uint16_t)(int16_t)(int8_t)fetch8(c);
		uint16_t r = alu(c, c->reg, rm16(c), imm, 1);
		if (c->reg != 7)
			set_rm16(c, r);
		break;
	}
	case 0x84: modrm(c); alu(c, 4, rm8(c), get8(c, c->reg), 0); break;
	case 0x85: modrm(c); alu(c, 4, rm16(c), c->r[c->reg], 1); break;
	case 0x86: {
		modrm(c);
		uint8_t a = rm8(c), b = get8(c, c->reg);
		set_rm8(c, b);
		set8(c, c->reg, a);
		break;
	}
	case 0x87: {
		modrm(c);
		uint16_t a = rm16(c), b = c->r[c->reg];
		set_rm16(c, b);
		c->r[c->reg] = a;
		break;
	}
	case 0x88: modrm(c); set_rm8(c, get8(c, c->reg)); break;
	case 0x89: modrm(c); set_rm16(c, c->r[c->reg]); break;
	case 0x8A: modrm(c); set8(c, c->reg, rm8(c)); break;
	case 0x8B: modrm(c); c->r[c->reg] = rm16(c); break;
	case 0x8C: modrm(c); set_rm16(c, c->s[c->reg & 3]); break;
	case 0x8D: modrm(c); c->r[c->reg] = c->ea_off; break;
	case 0x8E: modrm(c); c->s[c->reg & 3] = rm16(c); break;
	case 0x8F: modrm(c); set_rm16(c, pop(c)); break;
	case 0x90: break;
	case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
		uint16_t t = c->r[AX];
		c->r[AX] = c->r[op & 7];
		c->r[op & 7] = t;
		break;
	}
	case 0x98: c->r[AX] = (uint16_t)(int16_t)(int8_t)AL; break;
	case 0x99: c->r[DX] = (c->r[AX] & 0x8000) ? 0xFFFF : 0; break;
	case 0x9A: {
		uint16_t off = fetch16(c), seg = fetch16(c);
		push(c, c->s[CS]);
		push(c, c->ip);
		c->s[CS] = seg;
		c->ip = off;
		break;
	}
	case 0x9B: break;
	case 0x9C: push(c, c->fl | 0xF002); break;
	case 0x9D: c->fl = (uint16_t)((pop(c) & 0x0FD5) | 0x0002); break;
	case 0x9E: c->fl = (uint16_t)((c->fl & 0xFF00) | (AH & 0xD5) | 2); break;
	case 0x9F: SET_AH(c->fl & 0xFF); break;
	case 0xA0: case 0xA1: case 0xA2: case 0xA3: {
		uint16_t off = fetch16(c);
		uint32_t a = lin(c->s[c->seg_ovr >= 0 ? c->seg_ovr : DS], off);
		if (op == 0xA0) SET_AL(rb(c, a));
		else if (op == 0xA1) c->r[AX] = rw(c, a);
		else if (op == 0xA2) wb(c, a, AL);
		else ww(c, a, c->r[AX]);
		break;
	}
	case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
	case 0xAC: case 0xAD: case 0xAE: case 0xAF:
		string_op(c, op);
		break;
	case 0xA8: alu(c, 4, AL, fetch8(c), 0); break;
	case 0xA9: alu(c, 4, c->r[AX], fetch16(c), 1); break;
	case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
		set8(c, op & 7, fetch8(c));
		break;
	case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
		c->r[op & 7] = fetch16(c);
		break;
	case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
		modrm(c);
		int w = op & 1;
		int count = (op == 0xC0 || op == 0xC1) ? fetch8(c) : (op >= 0xD2 ? CL : 1);
		if (w)
			set_rm16(c, shift(c, c->reg, rm16(c), count, 1));
		else
			set_rm8(c, (uint8_t)shift(c, c->reg, rm8(c), count, 0));
		break;
	}
	case 0xC2: {
		uint16_t n = fetch16(c);
		c->ip = pop(c);
		c->r[SP] += n;
		break;
	}
	case 0xC3: c->ip = pop(c); break;
	case 0xC4: case 0xC5:
		modrm(c);
		c->r[c->reg] = rw(c, c->ea);
		c->s[op == 0xC4 ? ES : DS] = rw(c, c->ea + 2);
		break;
	case 0xC6: modrm(c); set_rm8(c, fetch8(c)); break;
	case 0xC7: modrm(c); set_rm16(c, fetch16(c)); break;
	case 0xC8: {	/* ENTER */
		uint16_t size = fetch16(c);
		uint8_t level = fetch8(c) & 31;
		push(c, c->r[BP]);
		uint16_t frame = c->r[SP];
		for (int i = 1; i < level; i++) {
			c->r[BP] -= 2;
			push(c, rw(c, lin(c->s[SS], c->r[BP])));
		}
		if (level)
			push(c, frame);
		c->r[BP] = frame;
		c->r[SP] -= size;
		break;
	}
	case 0xC9:	/* LEAVE */
		c->r[SP] = c->r[BP];
		c->r[BP] = pop(c);
		break;
	case 0xCA: {
		uint16_t n = fetch16(c);
		c->ip = pop(c);
		c->s[CS] = pop(c);
		c->r[SP] += n;
		break;
	}
	case 0xCB:
		c->ip = pop(c);
		c->s[CS] = pop(c);
		break;
	case 0xCC: do_int(c, 3); break;
	case 0xCD: do_int(c, fetch8(c)); break;
	case 0xCE: if (c->fl & F_OF) do_int(c, 4); break;
	case 0xCF:
		c->ip = pop(c);
		c->s[CS] = pop(c);
		c->fl = (uint16_t)((pop(c) & 0x0FD5) | 2);
		break;
	case 0xD4: {	/* AAM */
		uint8_t base = fetch8(c);
		if (!base) {
			do_int(c, 0);
			break;
		}
		uint8_t al = AL;
		SET_AH(al / base);
		SET_AL(al % base);
		szp(c, AL, 0);
		break;
	}
	case 0xD5: {	/* AAD */
		uint8_t base = fetch8(c);
		SET_AL(AL + AH * base);
		SET_AH(0);
		szp(c, AL, 0);
		break;
	}
	case 0xD6: SET_AL((c->fl & F_CF) ? 0xFF : 0); break;	/* SALC */
	case 0xD7:
		SET_AL(rb(c, lin(c->s[c->seg_ovr >= 0 ? c->seg_ovr : DS], (uint16_t)(c->r[BX] + AL))));
		break;
	case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
		modrm(c);	/* no 8087: ignore */
		break;
	case 0xE0: case 0xE1: case 0xE2: {
		int8_t d = (int8_t)fetch8(c);
		c->r[CX]--;
		int zf = !!(c->fl & F_ZF);
		int go = c->r[CX] != 0 && (op == 0xE2 || (op == 0xE1 ? zf : !zf));
		if (go)
			c->ip += (uint16_t)d;
		break;
	}
	case 0xE3: {
		int8_t d = (int8_t)fetch8(c);
		if (!c->r[CX])
			c->ip += (uint16_t)d;
		break;
	}
	case 0xE4: fetch8(c); SET_AL(0xFF); break;
	case 0xE5: fetch8(c); c->r[AX] = 0xFFFF; break;
	case 0xE6: case 0xE7: fetch8(c); break;
	case 0xE8: {
		uint16_t d = fetch16(c);
		push(c, c->ip);
		c->ip += d;
		break;
	}
	case 0xE9: { uint16_t d = fetch16(c); c->ip += d; break; }
	case 0xEA: {
		uint16_t off = fetch16(c), seg = fetch16(c);
		c->ip = off;
		c->s[CS] = seg;
		break;
	}
	case 0xEB: { int8_t d = (int8_t)fetch8(c); c->ip += (uint16_t)d; break; }
	case 0xEC: SET_AL(0xFF); break;
	case 0xED: c->r[AX] = 0xFFFF; break;
	case 0xEE: case 0xEF: break;
	case 0xF4:	/* HLT: wait for "an interrupt" */
		sync_screen(c);
		task_idle();
		break;
	case 0xF5: c->fl ^= F_CF; break;
	case 0xF6: case 0xF7: {
		modrm(c);
		int w = op & 1;
		switch (c->reg) {
		case 0: case 1:
			alu(c, 4, w ? rm16(c) : rm8(c), w ? fetch16(c) : fetch8(c), w);
			break;
		case 2:
			if (w) set_rm16(c, (uint16_t)~rm16(c));
			else set_rm8(c, (uint8_t)~rm8(c));
			break;
		case 3:
			if (w) set_rm16(c, alu(c, 5, 0, rm16(c), 1));
			else set_rm8(c, (uint8_t)alu(c, 5, 0, rm8(c), 0));
			break;
		case 4:	/* MUL */
			if (w) {
				uint32_t r = (uint32_t)c->r[AX] * rm16(c);
				c->r[AX] = (uint16_t)r;
				c->r[DX] = (uint16_t)(r >> 16);
				setf(c, F_CF | F_OF, c->r[DX] != 0);
			} else {
				uint16_t r = (uint16_t)(AL * rm8(c));
				c->r[AX] = r;
				setf(c, F_CF | F_OF, AH != 0);
			}
			break;
		case 5:	/* IMUL */
			if (w) {
				int32_t r = (int16_t)c->r[AX] * (int16_t)rm16(c);
				c->r[AX] = (uint16_t)r;
				c->r[DX] = (uint16_t)((uint32_t)r >> 16);
				setf(c, F_CF | F_OF, r != (int16_t)r);
			} else {
				int16_t r = (int16_t)((int8_t)AL * (int8_t)rm8(c));
				c->r[AX] = (uint16_t)r;
				setf(c, F_CF | F_OF, r != (int8_t)r);
			}
			break;
		case 6:	/* DIV */
			if (w) {
				uint32_t n = (uint32_t)c->r[DX] << 16 | c->r[AX];
				uint16_t d = rm16(c);
				if (!d || n / d > 0xFFFF) {
					do_int(c, 0);
					break;
				}
				c->r[AX] = (uint16_t)(n / d);
				c->r[DX] = (uint16_t)(n % d);
			} else {
				uint16_t n = c->r[AX];
				uint8_t d = rm8(c);
				if (!d || n / d > 0xFF) {
					do_int(c, 0);
					break;
				}
				SET_AL(n / d);
				SET_AH(n % d);
			}
			break;
		case 7:	/* IDIV */
			if (w) {
				int32_t n = (int32_t)((uint32_t)c->r[DX] << 16 | c->r[AX]);
				int16_t d = (int16_t)rm16(c);
				if (!d || n / d > 32767 || n / d < -32768) {
					do_int(c, 0);
					break;
				}
				c->r[AX] = (uint16_t)(n / d);
				c->r[DX] = (uint16_t)(n % d);
			} else {
				int16_t n = (int16_t)c->r[AX];
				int8_t d = (int8_t)rm8(c);
				if (!d || n / d > 127 || n / d < -128) {
					do_int(c, 0);
					break;
				}
				SET_AL(n / d);
				SET_AH(n % d);
			}
			break;
		}
		break;
	}
	case 0xF8: setf(c, F_CF, 0); break;
	case 0xF9: setf(c, F_CF, 1); break;
	case 0xFA: setf(c, F_IF, 0); break;
	case 0xFB: setf(c, F_IF, 1); break;
	case 0xFC: setf(c, F_DF, 0); break;
	case 0xFD: setf(c, F_DF, 1); break;
	case 0xFE:
		modrm(c);
		if (c->reg < 2)
			set_rm8(c, (uint8_t)incdec(c, rm8(c), c->reg, 0));
		break;
	case 0xFF:
		modrm(c);
		switch (c->reg) {
		case 0: set_rm16(c, incdec(c, rm16(c), 0, 1)); break;
		case 1: set_rm16(c, incdec(c, rm16(c), 1, 1)); break;
		case 2: {
			uint16_t t = rm16(c);
			push(c, c->ip);
			c->ip = t;
			break;
		}
		case 3: {
			uint16_t off = rw(c, c->ea), seg = rw(c, c->ea + 2);
			push(c, c->s[CS]);
			push(c, c->ip);
			c->ip = off;
			c->s[CS] = seg;
			break;
		}
		case 4: c->ip = rm16(c); break;
		case 5:
			c->ip = rw(c, c->ea);
			c->s[CS] = rw(c, c->ea + 2);
			break;
		case 6: push(c, rm16(c)); break;
		}
		break;
	default:
		/* unknown opcode: report like a real CPU would trap */
		c->running = 0;
		c->exit_code = 255;
		{
			static char msg[64];
			snprintf(msg, sizeof msg, "Invalid opcode %02X at %04X:%04X", op, c->s[CS],
			    (uint16_t)(c->ip - 1));
			c->errmsg = msg;
		}
	}
}

/* ---- loading and running ----------------------------------------------------------------- */

static void setup_ivt(struct cpu *c)
{
	for (int n = 0; n < 256; n++) {
		ww(c, (uint32_t)n * 4, (uint16_t)(n * 4));
		ww(c, (uint32_t)n * 4 + 2, BIOS_SEG);
		c->m[(BIOS_SEG << 4) + n * 4] = 0xCF;	/* IRET, for anyone who chains */
	}
	/* BIOS data area: 80 columns, 25 rows, video mode 3 */
	ww(c, 0x44A, COLS);
	wb(c, 0x449, 3);
	wb(c, 0x484, ROWS - 1);
}

static void setup_psp(struct cpu *c, int argc, char **argv)
{
	uint32_t p = (uint32_t)PSP_SEG << 4;
	c->m[p] = 0xCD;		/* INT 20h */
	c->m[p + 1] = 0x20;
	ww(c, p + 2, 0xA000);	/* top of memory */
	ww(c, p + 0x2C, ENV_SEG);
	/* command tail */
	char tail[128] = "";
	for (int i = 1; i < argc; i++) {
		strcat0(tail, " ", sizeof tail);
		strcat0(tail, argv[i], sizeof tail);
	}
	size_t tl = MIN(strlen(tail), 126);
	c->m[p + 0x80] = (uint8_t)tl;
	memcpy(c->m + p + 0x81, tail, tl);
	c->m[p + 0x81 + tl] = '\r';
	/* environment: PATH, COMSPEC, then the program's own name */
	uint32_t e = (uint32_t)ENV_SEG << 4;
	const char *env[] = { "PATH=C:\\BDOS", "COMSPEC=C:\\COMMAND.COM", "PROMPT=$P$G" };
	for (size_t i = 0; i < NELEM(env); i++) {
		size_t l = strlen(env[i]) + 1;
		memcpy(c->m + e, env[i], l);
		e += (uint32_t)l;
	}
	c->m[e++] = 0;
	ww(c, e, 1);
	e += 2;
	const char *name = argv[0];
	memcpy(c->m + e, name, strlen(name) + 1);
	c->dta_seg = PSP_SEG;
	c->dta_off = 0x80;
}

static int load_com(struct cpu *c, const uint8_t *img, size_t n)
{
	if (n > 0xFF00)
		return -1;
	memcpy(c->m + ((uint32_t)PSP_SEG << 4) + 0x100, img, n);
	c->s[CS] = c->s[DS] = c->s[ES] = c->s[SS] = PSP_SEG;
	c->ip = 0x100;
	c->r[SP] = 0xFFFE;
	ww(c, lin(PSP_SEG, 0xFFFE), 0);	/* RET goes to PSP:0000, INT 20h */
	return 0;
}

static int load_exe(struct cpu *c, const uint8_t *img, size_t n)
{
	if (n < 28)
		return -1;
	uint16_t h[14];
	for (int i = 0; i < 14; i++)
		h[i] = (uint16_t)(img[i * 2] | img[i * 2 + 1] << 8);
	uint32_t size = h[2] * 512u - (h[1] ? 512 - h[1] : 0);
	uint32_t hdr = h[4] * 16u;
	if (size > n)
		size = (uint32_t)n;
	if (hdr >= size)
		return -1;
	uint32_t body = size - hdr;
	uint16_t load = PSP_SEG + 0x10;
	if (((uint32_t)load << 4) + body > 0x9F000)
		return -2;	/* too big for conventional memory */
	memcpy(c->m + ((uint32_t)load << 4), img + hdr, body);
	for (int i = 0; i < h[3]; i++) {
		uint32_t at = h[12] + (uint32_t)i * 4;
		if (at + 4 > n)
			return -1;
		uint16_t off = (uint16_t)(img[at] | img[at + 1] << 8);
		uint16_t seg = (uint16_t)(img[at + 2] | img[at + 3] << 8);
		uint32_t a = lin((uint16_t)(load + seg), off);
		ww(c, a, (uint16_t)(rw(c, a) + load));
	}
	c->s[CS] = (uint16_t)(load + h[11]);
	c->ip = h[10];
	c->s[SS] = (uint16_t)(load + h[7]);
	c->r[SP] = h[8];
	c->s[DS] = c->s[ES] = PSP_SEG;
	return 0;
}

static int last_exit;

int com_run(const char *path, int argc, char **argv)
{
	int e;
	size_t n;
	char *img = vfs_slurp(path, &n, &e);
	if (!img) {
		kprintf("%s - %s\n", vfs_strerror(e), path);
		return 1;
	}
	struct cpu *c = kzalloc(sizeof *c);
	if (c)
		c->m = kzalloc(MEM_SIZE);
	if (!c || !c->m) {
		kfree(c);
		kfree(img);
		kprintf("Not enough memory\n");
		return 1;
	}
	setup_ivt(c);
	char *args0[1] = { (char *)path };
	if (!argc) {
		argc = 1;
		argv = args0;
	}
	setup_psp(c, argc, argv);
	int r = (n >= 2 && img[0] == 'M' && img[1] == 'Z') ? load_exe(c, (uint8_t *)img, n) :
	    load_com(c, (uint8_t *)img, n);
	kfree(img);
	if (r) {
		kprintf(r == -2 ? "Program too big to fit in memory\n" : "Bad or damaged program file\n");
		kfree(c->m);
		kfree(c);
		return 1;
	}
	c->fl = 0x0202;
	c->r[AX] = 0;
	c->r[BX] = 0;
	c->r[CX] = 0x00FF;
	c->r[DX] = PSP_SEG;
	c->retcode = last_exit;
	for (int i = 0; i < ROWS * COLS; i++)
		c->m[VRAM + i * 2 + 1] = 7;
	c->running = 1;

	while (c->running) {
		if (c->s[CS] == BIOS_SEG && c->ip < 0x400 && !(c->ip & 3)) {
			/* a hooked interrupt chained to the original handler */
			hle_int(c, c->ip / 4);
			c->ip = pop(c);
			c->s[CS] = pop(c);
			uint16_t fl = pop(c);
			/* the handler's result flags (CF, ZF) win over the saved ones */
			c->fl = (uint16_t)(((fl & ~(F_CF | F_ZF)) | (c->fl & (F_CF | F_ZF)) | 2));
			continue;
		}
		step(c);
		if (!(++c->steps & 0x3FFF)) {
			poll_keys(c);
			sync_screen(c);
			task_yield();
		}
	}
	sync_screen(c);
	if (c->screen_mode) {
		con_attr(7, 0);
		con_gotoxy(0, MIN(c->cy + 1, ROWS - 1));
	}
	con_attr(7, 0);
	for (int i = 5; i < MAX_FILES; i++)
		if (c->files[i].f)
			vfs_close(c->files[i].f);
	if (c->errmsg)
		kprintf("\n%s\n", c->errmsg);
	int code = c->exit_code;
	last_exit = code;
	kfree(c->find);
	kfree(c->m);
	kfree(c);
	return code;
}
