/*
 * dosapi.c - DOS itself: INT 21h, the memory arena, program loading and
 * EXEC, file handles, FCBs, and the XMS driver.
 *
 * Files are bdos's own: a DOS program opening C:\GAMES\SAVE.DAT opens that
 * file, through bdosfs's 8.3 aliases where names are long. Memory is the
 * real DOS layout - MCBs from FIRST_MCB up to 640K, PSPs, environments - so
 * programs that walk it or shrink themselves and EXEC a child work.
 * COMMAND.COM isn't a DOS program here: EXEC'ing it (or COMSPEC) runs the
 * command in the bdos shell, so a DOS menu can launch anything bdos can.
 */
#include "dos.h"
#include "../gate.h"

#define PSP_SIZE_PARAS	0x10
#define JFT_SIZE	20
#define LOL_OFF		0x0026	/* in DOS_DATA_SEG: the List of Lists */
#define INDOS_OFF	0x0010	/* the InDOS flag */
#define SWAP_OFF	0x0100	/* scratch the DOS keeps for itself */
#define XMS_BASE	0x110000	/* extended memory past the HMA */

static void dos_err(struct dosbox *b, int code);

/* ---- small things ------------------------------------------------------------------ */

static void ok(struct x86 *c)
{
	bios_set_flag(c, FL_CF, 0);
}

static void fail(struct dosbox *b, uint16_t code)
{
	set16(&b->cpu, R_EAX, code);
	bios_set_flag(&b->cpu, FL_CF, 1);
	b->last_error = code;
}

static uint16_t dos_error_code(int e)
{
	switch (e) {
	case E_NOENT: return 2;
	case E_NOTDIR: return 3;
	case E_ACCES: case E_ISDIR: case E_EXIST: case E_NOTEMPTY: return 5;
	case E_NOSPC: return 0x27;
	case E_BADDRIVE: return 0x0F;
	case E_INVAL: return 0x0D;
	}
	return 0x1F;
}

static void dos_err(struct dosbox *b, int e)
{
	fail(b, dos_error_code(e));
}

static void get_str(struct dosbox *b, uint32_t a, char *out, size_t n)
{
	size_t i = 0;
	for (; i + 1 < n; i++) {
		char ch = (char)x86_rd8(&b->cpu, a + (uint32_t)i);
		if (!ch)
			break;
		out[i] = ch;
	}
	out[i] = 0;
}

static void put_str(struct dosbox *b, uint32_t a, const char *s)
{
	do
		x86_wr8(&b->cpu, a++, (uint8_t)*s);
	while (*s++);
}

static uint32_t ds_dx(struct x86 *c)
{
	return c->s[S_DS].base + DX(c);
}

/*
 * A character to standard output, the way DOS would: through the BIOS to
 * the screen -- or, when it's redirected to a file or a pipe, there and only
 * there, whatever the screen is doing (a program that turned the mouse on,
 * or went into graphics, still writes its > file).
 */
void dos_con_out(struct dosbox *b, uint8_t ch)
{
	if (!stdout_is_redirected()) {
		bios_tty(b, ch);
		return;
	}
	if (ch == '\r')
		return;			/* the console's newline does both */
	if (ch == '\n' || ch == '\b' || ch == '\t' || ch == 7 || (ch >= 0x20 && ch < 0x80 && ch != 0x7F))
		con_putc((char)ch);
	else
		con_rawchar(ch);
}

/* program text to the screen, the way DOS would (through the BIOS) */
void dos_print(struct dosbox *b, const char *s)
{
	while (*s)
		dos_con_out(b, (uint8_t)*s++);
}

/* ---- paths ----------------------------------------------------------------------- */

/* A DOS path, relative to the right drive's current directory, made absolute. */
int dos_resolve(struct dosbox *b, const char *in, char *out, size_t n)
{
	char tmp[VFS_PATH_MAX];
	while (*in == ' ')
		in++;
	int drive = b->drive;
	const char *p = in;
	if (p[0] && p[1] == ':') {
		drive = toupper(p[0]) - 'A';
		p += 2;
	}
	if (drive < 0 || drive >= 26)
		return E_BADDRIVE;
	if (!b->cwd[drive][0])
		snprintf(b->cwd[drive], sizeof b->cwd[drive], "%c:\\", 'A' + drive);
	if (*p == '\\' || *p == '/')
		snprintf(tmp, sizeof tmp, "%c:%s", 'A' + drive, p);
	else
		snprintf(tmp, sizeof tmp, "%s%s%s", b->cwd[drive],
		    b->cwd[drive][strlen(b->cwd[drive]) - 1] == '\\' ? "" : "\\", p);
	for (char *q = tmp; *q; q++)
		if (*q == '/')
			*q = '\\';
	/* "NUL", "CON", "PRN" and friends are devices wherever they are */
	if (resolve_path(tmp, out, n))
		return E_NOENT;
	char canon[VFS_PATH_MAX];
	if (!vfs_canon(out, canon, sizeof canon))
		strncpy0(out, canon, n);
	return 0;
}

static int dev_of(const char *path)
{
	const char *base = vfs_basename(path);
	char name[16];
	strncpy0(name, base, sizeof name);
	char *dot = strchr(name, '.');
	if (dot)
		*dot = 0;
	if (!strcasecmp(name, "CON")) return DEV_CON;
	if (!strcasecmp(name, "NUL")) return DEV_NUL;
	if (!strcasecmp(name, "AUX") || !strcasecmp(name, "COM1")) return DEV_AUX;
	if (!strcasecmp(name, "PRN") || !strcasecmp(name, "LPT1")) return DEV_PRN;
	if (!strcasecmp(name, "CLOCK$")) return DEV_CLOCK;
	return 0;
}

/* ---- memory: the MCB chain --------------------------------------------------------- */

#define MCB_SIG(s)	mb(b, sl((s), 0))
#define MCB_OWNER(s)	mw(b, sl((s), 1))
#define MCB_SIZE(s)	mw(b, sl((s), 3))

static void mcb_set(struct dosbox *b, uint16_t s, uint8_t sig, uint16_t owner, uint16_t size)
{
	mbw(b, sl(s, 0), sig);
	mww(b, sl(s, 1), owner);
	mww(b, sl(s, 3), size);
}

/* join neighbouring free blocks */
static void mcb_merge(struct dosbox *b)
{
	uint16_t s = FIRST_MCB;
	for (int guard = 0; guard < 4096; guard++) {
		if (MCB_SIG(s) == 'Z')
			return;
		uint16_t next = (uint16_t)(s + 1 + MCB_SIZE(s));
		if (MCB_OWNER(s) == 0 && MCB_OWNER(next) == 0 && (MCB_SIG(next) == 'M' || MCB_SIG(next) == 'Z')) {
			mcb_set(b, s, MCB_SIG(next), 0, (uint16_t)(MCB_SIZE(s) + 1 + MCB_SIZE(next)));
			continue;
		}
		if (MCB_SIG(next) != 'M' && MCB_SIG(next) != 'Z')
			return;		/* the chain is broken: leave it */
		s = next;
	}
}

uint16_t mcb_alloc(struct dosbox *b, uint16_t paras, uint16_t owner, uint16_t *largest)
{
	mcb_merge(b);
	uint16_t best = 0, big = 0;
	int strategy = b->alloc_strategy & 3;
	for (uint16_t s = FIRST_MCB;;) {
		uint8_t sig = MCB_SIG(s);
		if (sig != 'M' && sig != 'Z')
			break;
		uint16_t size = MCB_SIZE(s);
		if (MCB_OWNER(s) == 0) {
			if (size > big)
				big = size;
			if (size >= paras) {
				if (!best || (strategy == 1 && size < MCB_SIZE(best)) || strategy == 2)
					best = s;
				if (strategy == 0)
					break;
			}
		}
		if (sig == 'Z')
			break;
		s = (uint16_t)(s + 1 + size);
	}
	if (largest)
		*largest = big;
	if (!best)
		return 0;
	uint16_t size = MCB_SIZE(best);
	uint8_t sig = MCB_SIG(best);
	if (size > paras + 1) {
		if (strategy == 2) {	/* last fit: take the top end */
			uint16_t top = (uint16_t)(best + 1 + size - paras - 1);
			mcb_set(b, best, 'M', 0, (uint16_t)(size - paras - 1));
			mcb_set(b, top, sig, owner, paras);
			return (uint16_t)(top + 1);
		}
		uint16_t rest = (uint16_t)(best + 1 + paras);
		mcb_set(b, rest, sig, 0, (uint16_t)(size - paras - 1));
		mcb_set(b, best, 'M', owner, paras);
	} else {
		mww(b, sl(best, 1), owner);
	}
	return (uint16_t)(best + 1);
}

int mcb_free(struct dosbox *b, uint16_t seg)
{
	uint16_t s = (uint16_t)(seg - 1);
	if (MCB_SIG(s) != 'M' && MCB_SIG(s) != 'Z')
		return -1;
	mww(b, sl(s, 1), 0);
	mcb_merge(b);
	return 0;
}

int mcb_resize(struct dosbox *b, uint16_t seg, uint16_t paras, uint16_t *largest)
{
	uint16_t s = (uint16_t)(seg - 1);
	if (MCB_SIG(s) != 'M' && MCB_SIG(s) != 'Z')
		return 7;
	mcb_merge(b);
	uint16_t size = MCB_SIZE(s);
	uint8_t sig = MCB_SIG(s);
	if (paras <= size) {
		if (size > paras) {
			uint16_t rest = (uint16_t)(s + 1 + paras);
			mcb_set(b, rest, sig, 0, (uint16_t)(size - paras - 1));
			mcb_set(b, s, 'M', MCB_OWNER(s), paras);
			mcb_merge(b);
		}
		return 0;
	}
	/* growing: only into a free block right after */
	uint16_t avail = size;
	if (sig == 'M') {
		uint16_t next = (uint16_t)(s + 1 + size);
		if (MCB_OWNER(next) == 0)
			avail = (uint16_t)(size + 1 + MCB_SIZE(next));
	}
	if (avail < paras) {
		if (largest)
			*largest = avail;
		return 8;
	}
	uint16_t next = (uint16_t)(s + 1 + size);
	uint8_t nsig = MCB_SIG(next);
	if (avail > paras) {
		uint16_t rest = (uint16_t)(s + 1 + paras);
		mcb_set(b, rest, nsig, 0, (uint16_t)(avail - paras - 1));
		mcb_set(b, s, 'M', MCB_OWNER(s), paras);
	} else {
		mcb_set(b, s, nsig, MCB_OWNER(s), paras);
	}
	return 0;
}

static void mcb_free_owner(struct dosbox *b, uint16_t psp)
{
	for (uint16_t s = FIRST_MCB;;) {
		uint8_t sig = MCB_SIG(s);
		if (sig != 'M' && sig != 'Z')
			break;
		if (MCB_OWNER(s) == psp)
			mww(b, sl(s, 1), 0);
		if (sig == 'Z')
			break;
		s = (uint16_t)(s + 1 + MCB_SIZE(s));
	}
	mcb_merge(b);
}

static void mcb_name(struct dosbox *b, uint16_t seg, const char *path)
{
	const char *base = vfs_basename(path);
	uint32_t a = sl((uint16_t)(seg - 1), 8);
	int i = 0;
	for (; i < 8 && base[i] && base[i] != '.'; i++)
		mbw(b, a + (uint32_t)i, (uint8_t)toupper(base[i]));
	for (; i < 8; i++)
		mbw(b, a + (uint32_t)i, 0);
}

/* ---- extended memory (XMS and DPMI share it) ------------------------------------------ */

uint32_t ext_alloc(struct dosbox *b, uint32_t size, int *handle)
{
	size = (size + 1023) & ~1023u;
	uint32_t top = b->cpu.mem_size;
	/* first fit between the blocks we've handed out */
	uint32_t at = XMS_BASE;	/* page-aligned, as DPMI blocks should be */
	for (;;) {
		int moved = 0;
		for (int i = 0; i < MAX_EXT; i++) {
			if (!b->xms[i].used)
				continue;
			uint32_t lo = b->xms[i].base, hi = lo + b->xms[i].size;
			if (at < hi && at + size > lo) {
				at = (hi + 4095) & ~4095u;
				moved = 1;
			}
		}
		if (!moved)
			break;
	}
	if (at + size > top || at + size < at)
		return 0;
	for (int i = 0; i < MAX_EXT; i++)
		if (!b->xms[i].used) {
			b->xms[i].used = 1;
			b->xms[i].base = at;
			b->xms[i].size = size;
			b->xms[i].locks = 0;
			*handle = i + 1;
			return at;
		}
	return 0;
}

void ext_free(struct dosbox *b, int handle)
{
	if (handle >= 1 && handle <= MAX_EXT)
		b->xms[handle - 1].used = 0;
}

uint32_t ext_free_kb(struct dosbox *b, uint32_t *largest_kb)
{
	/* walk the gaps */
	uint32_t total = 0, big = 0, at = XMS_BASE;
	uint32_t top = b->cpu.mem_size;
	for (;;) {
		uint32_t next = top;
		int found = -1;
		for (int i = 0; i < MAX_EXT; i++)
			if (b->xms[i].used && b->xms[i].base >= at && b->xms[i].base < next) {
				next = b->xms[i].base;
				found = i;
			}
		uint32_t gap = next > at ? next - at : 0;
		total += gap;
		if (gap > big)
			big = gap;
		if (found < 0)
			break;
		at = b->xms[found].base + b->xms[found].size;
	}
	if (largest_kb)
		*largest_kb = big / 1024;
	return total / 1024;
}

/* ---- XMS ---------------------------------------------------------------------------- */

static uint32_t xms_addr(struct dosbox *b, uint16_t handle, uint32_t off)
{
	if (!handle)	/* conventional memory, as seg:off */
		return sl((uint16_t)(off >> 16), (uint16_t)off);
	if (handle > MAX_EXT || !b->xms[handle - 1].used)
		return 0xFFFFFFFF;
	return b->xms[handle - 1].base + off;
}

void xms_entry(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	uint8_t fn = AH(c);
	switch (fn) {
	case 0x00:
		set16(c, R_EAX, 0x0300);
		set16(c, R_EBX, 0x0300);
		set16(c, R_EDX, 1);		/* there's an HMA */
		return;
	case 0x01:
		if (b->hma_used) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0x91);
		} else {
			b->hma_used = 1;
			set16(c, R_EAX, 1);
		}
		return;
	case 0x02:
		b->hma_used = 0;
		set16(c, R_EAX, 1);
		return;
	case 0x03: case 0x05:
		c->a20 = 0xFFFFFFFF;
		set16(c, R_EAX, 1);
		return;
	case 0x04: case 0x06:
		set16(c, R_EAX, 1);	/* leave A20 on: more programs like it that way */
		return;
	case 0x07:
		set16(c, R_EAX, (c->a20 & 0x100000) ? 1 : 0);
		setlo(c, R_EBX, 0);
		return;
	case 0x08: case 0x88: {
		uint32_t big, total = ext_free_kb(b, &big);
		if (fn == 0x88) {
			c->r[R_EAX] = big;
			c->r[R_EDX] = total;
			c->r[R_ECX] = c->mem_size - 1;
		} else {
			set16(c, R_EAX, (uint16_t)MIN(big, 0xFFFFu));
			set16(c, R_EDX, (uint16_t)MIN(total, 0xFFFFu));
		}
		setlo(c, R_EBX, 0);
		return;
	}
	case 0x09: case 0x89: {
		uint32_t kb = fn == 0x89 ? c->r[R_EDX] : DX(c);
		int h;
		if (!ext_alloc(b, kb * 1024 ? kb * 1024 : 1024, &h)) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA0);
			return;
		}
		set16(c, R_EAX, 1);
		set16(c, R_EDX, (uint16_t)h);
		return;
	}
	case 0x0A:
		if (DX(c) < 1 || DX(c) > MAX_EXT || !b->xms[DX(c) - 1].used) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA2);
			return;
		}
		ext_free(b, DX(c));
		set16(c, R_EAX, 1);
		return;
	case 0x0B: {
		uint32_t p = c->s[S_DS].base + SI(c);
		uint32_t len = x86_rd32(c, p);
		uint32_t src = xms_addr(b, x86_rd16(c, p + 4), x86_rd32(c, p + 6));
		uint32_t dst = xms_addr(b, x86_rd16(c, p + 10), x86_rd32(c, p + 12));
		if (src == 0xFFFFFFFF || dst == 0xFFFFFFFF || src + len > c->mem_size || dst + len > c->mem_size) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA3);
			return;
		}
		memmove(c->mem + dst, c->mem + src, len);
		if ((dst < 0xB0000 && dst + len > 0xA0000))
			b->vga.dirty = 1;
		set16(c, R_EAX, 1);
		return;
	}
	case 0x0C: {
		if (DX(c) < 1 || DX(c) > MAX_EXT || !b->xms[DX(c) - 1].used) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA2);
			return;
		}
		uint32_t base = b->xms[DX(c) - 1].base;
		b->xms[DX(c) - 1].locks++;
		set16(c, R_EAX, 1);
		set16(c, R_EDX, (uint16_t)(base >> 16));
		set16(c, R_EBX, (uint16_t)base);
		return;
	}
	case 0x0D:
		if (DX(c) >= 1 && DX(c) <= MAX_EXT && b->xms[DX(c) - 1].locks)
			b->xms[DX(c) - 1].locks--;
		set16(c, R_EAX, 1);
		return;
	case 0x0E: case 0x8E: {
		if (DX(c) < 1 || DX(c) > MAX_EXT || !b->xms[DX(c) - 1].used) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA2);
			return;
		}
		int free_handles = 0;
		for (int i = 0; i < MAX_EXT; i++)
			free_handles += !b->xms[i].used;
		set16(c, R_EAX, 1);
		sethi(c, R_EBX, (uint8_t)b->xms[DX(c) - 1].locks);
		if (fn == 0x8E) {
			set16(c, R_ECX, (uint16_t)free_handles);
			c->r[R_EDX] = b->xms[DX(c) - 1].size / 1024;
		} else {
			setlo(c, R_EBX, (uint8_t)free_handles);
			set16(c, R_EDX, (uint16_t)(b->xms[DX(c) - 1].size / 1024));
		}
		return;
	}
	case 0x0F: case 0x8F: {
		int h = DX(c);
		uint32_t kb = fn == 0x8F ? c->r[R_EBX] : BX(c);
		if (h < 1 || h > MAX_EXT || !b->xms[h - 1].used) {
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA2);
			return;
		}
		uint32_t old_base = b->xms[h - 1].base, old_size = b->xms[h - 1].size;
		b->xms[h - 1].used = 0;
		int nh;
		uint32_t nb = ext_alloc(b, kb * 1024, &nh);
		if (!nb) {
			b->xms[h - 1].used = 1;
			set16(c, R_EAX, 0);
			setlo(c, R_EBX, 0xA0);
			return;
		}
		/* keep the handle number: move the new block into the old slot */
		b->xms[nh - 1].used = 0;
		b->xms[h - 1].used = 1;
		b->xms[h - 1].base = nb;
		b->xms[h - 1].size = (kb * 1024 + 1023) & ~1023u;
		memmove(c->mem + nb, c->mem + old_base, MIN(old_size, b->xms[h - 1].size));
		set16(c, R_EAX, 1);
		return;
	}
	case 0x10:
		set16(c, R_EAX, 0);
		setlo(c, R_EBX, 0xB1);		/* no UMBs */
		set16(c, R_EDX, 0);
		return;
	}
	set16(c, R_EAX, 0);
	setlo(c, R_EBX, 0x80);
}

/* ---- handles ------------------------------------------------------------------------ */

static uint32_t jft_addr(struct dosbox *b)
{
	uint16_t psp = b->psp;
	return sl(mw(b, sl(psp, 0x36)), mw(b, sl(psp, 0x34)));
}

static int jft_size(struct dosbox *b)
{
	return mw(b, sl(b->psp, 0x32));
}

static int sft_of(struct dosbox *b, int h)
{
	if (h < 0 || h >= jft_size(b))
		return -1;
	uint8_t i = mb(b, jft_addr(b) + (uint32_t)h);
	if (i == 0xFF || i >= MAX_SFT || !b->sft[i].refs)
		return -1;
	return i;
}

static int sft_new(struct dosbox *b)
{
	for (int i = 5; i < MAX_SFT; i++)
		if (!b->sft[i].refs)
			return i;
	return -1;
}

static int handle_new(struct dosbox *b, int sft)
{
	uint32_t jft = jft_addr(b);
	for (int h = 0; h < jft_size(b); h++)
		if (mb(b, jft + (uint32_t)h) == 0xFF) {
			mbw(b, jft + (uint32_t)h, (uint8_t)sft);
			b->sft[sft].refs++;
			return h;
		}
	return -1;
}

static void sft_release(struct dosbox *b, int i)
{
	if (i < 0 || !b->sft[i].refs)
		return;
	if (--b->sft[i].refs == 0) {
		if (b->sft[i].f)
			vfs_close(b->sft[i].f);
		b->sft[i].f = NULL;
	}
}

static int handle_close(struct dosbox *b, int h)
{
	int i = sft_of(b, h);
	if (i < 0)
		return -1;
	mbw(b, jft_addr(b) + (uint32_t)h, 0xFF);
	sft_release(b, i);
	return 0;
}

void dos_close_all(struct dosbox *b)
{
	for (int h = 0; h < jft_size(b); h++)
		handle_close(b, h);
}

static int open_file(struct dosbox *b, const char *dospath, int mode, int create, int excl)
{
	struct x86 *c = &b->cpu;
	char abs[VFS_PATH_MAX];
	int e = dos_resolve(b, dospath, abs, sizeof abs);
	if (e) {
		fail(b, 3);
		return -1;
	}
	int i = sft_new(b);
	if (i < 0) {
		fail(b, 4);
		return -1;
	}
	struct sft *s = &b->sft[i];
	memset(s, 0, sizeof *s);
	s->dev = dev_of(abs);
	s->mode = (uint8_t)mode;
	strncpy0(s->name, vfs_basename(abs), sizeof s->name);
	if (!s->dev) {
		struct vstat st;
		if (excl && !vfs_stat(abs, &st)) {
			fail(b, 0x50);
			return -1;
		}
		int flags = create ? (O_READ | O_WRITE | O_CREATE | O_TRUNC) :
		    (mode & 3) == 0 ? O_READ : (mode & 3) == 1 ? (O_WRITE) : (O_READ | O_WRITE);
		if ((mode & 3) == 1 && !create)
			flags |= O_READ;	/* bdos files are happy read+write */
		strncpy0(s->path, abs, sizeof s->path);
		s->f = vfs_open(abs, flags, &e);
		if (!s->f) {
			dos_err(b, e);
			return -1;
		}
		if (vfs_isdev(s->f))
			s->dev = DEV_CON;
		if (create && (CX(c) & 0x07))
			vfs_setattr(abs, (uint8_t)(CX(c) & 0x27));
	}
	s->refs = 0;
	int h = handle_new(b, i);
	if (h < 0) {
		if (s->f)
			vfs_close(s->f);
		s->f = NULL;
		fail(b, 4);
		return -1;
	}
	return h;
}

/* ---- console input --------------------------------------------------------------- */

/* the next key as (scan << 8 | ascii), or -1 */
/* a key as DOS reads it: INT 16h's old call, so the grey arrows are 0 and a
 * scan code like the keypad's, not E0h (which is a character to a program) */
static uint16_t dos_key_get(struct dosbox *b)
{
	uint16_t k = bios_key_get(b);
	if ((k & 0xFF) == 0xE0 && k >> 8)
		k &= 0xFF00;
	return k;
}

static int con_key2(struct dosbox *b)
{
	if (stdin_is_pipe()) {
		int k = con_getkey();
		if (k == KEY_EOF)
			return 0x1A;
		return k == '\n' ? 0x1C0D : k & 0xFF;
	}
	if (!bios_key_avail(b)) {
		b->cpu.flags |= FL_IF;
		dosbox_input_wait(b);
		return -1;
	}
	return dos_key_get(b);
}

/* ^C: what INT 23h would do */
static int check_break(struct dosbox *b, int ascii)
{
	if (ascii != 3 && !b->ctrl_break)
		return 0;
	b->ctrl_break = 0;
	dos_print(b, "^C\r\n");
	uint16_t seg = mw(b, 0x23 * 4 + 2), off = mw(b, 0x23 * 4);
	if (seg == BIOS_SEG && off == 0x23 * 4) {
		dos_terminate(b, 0, 0);
		return 1;
	}
	return 0;	/* hooked: the program wants to deal with it */
}


static int read_char_fn(struct dosbox *b, int echo, int brk)
{
	struct x86 *c = &b->cpu;
	if (b->pending_scan >= 0) {
		setlo(c, R_EAX, (uint8_t)b->pending_scan);
		b->pending_scan = -1;
		return 0;
	}
	int k = con_key2(b);
	if (k < 0)
		return 1;
	uint8_t a = (uint8_t)k;
	if (brk && check_break(b, a))
		return 0;
	if (!a)
		b->pending_scan = (k >> 8) & 0xFF;
	setlo(c, R_EAX, a);
	if (echo && a)
		dos_con_out(b, a);
	return 0;
}

/* line input, as AH=0Ah and reads from CON: returns 1 while still typing */
static int read_line_fn(struct dosbox *b, int max)
{
	for (;;) {
		int k = con_key2(b);
		if (k < 0)
			return 1;
		uint8_t a = (uint8_t)k;
		if (check_break(b, a))
			return -1;
		if (a == '\r') {
			b->line[b->line_len] = 0;
			dos_con_out(b, '\r');
			return 0;
		}
		if (a == 8) {
			if (b->line_len) {
				b->line_len--;
				dos_print(b, "\b \b");
			}
			continue;
		}
		if (a == 0x1A && stdin_is_pipe()) {
			b->line[b->line_len] = 0;
			return 0;
		}
		if (a == 27) {		/* Esc: start the line again */
			while (b->line_len) {
				b->line_len--;
				dos_print(b, "\b \b");
			}
			continue;
		}
		if (a >= 0x20 && b->line_len < max - 1 && b->line_len < (int)sizeof b->line - 2) {
			b->line[b->line_len++] = (char)a;
			dos_con_out(b, a);
		}
	}
}

/* ---- reading and writing ---------------------------------------------------------- */

static long dev_write(struct dosbox *b, int dev, uint32_t buf, uint32_t n)
{
	struct x86 *c = &b->cpu;
	if (dev == DEV_CON) {
		for (uint32_t i = 0; i < n; i++)
			dos_con_out(b, x86_rd8(c, buf + i));
		return (long)n;
	}
	return (long)n;		/* NUL, AUX, PRN: swallowed */
}

static int do_read(struct dosbox *b, int h, uint32_t buf, uint32_t n, long *got)
{
	struct x86 *c = &b->cpu;
	int i = sft_of(b, h);
	if (i < 0)
		return 6;
	struct sft *s = &b->sft[i];
	if (s->dev == DEV_CON) {
		/* cooked console input: a whole line, then hand it out */
		if (!b->line_done) {
			int r = read_line_fn(b, 128);
			if (r > 0)
				return -1;
			if (r < 0) {
				*got = 0;
				return 0;
			}
			b->line[b->line_len++] = '\r';
			b->line[b->line_len++] = '\n';
			dos_con_out(b, '\n');
			b->line_done = 1;
			b->line_pos = 0;
		}
		uint32_t take = (uint32_t)MIN((long)n, (long)(b->line_len - b->line_pos));
		for (uint32_t k = 0; k < take; k++)
			x86_wr8(c, buf + k, (uint8_t)b->line[b->line_pos + (int)k]);
		b->line_pos += (int)take;
		if (b->line_pos >= b->line_len) {
			b->line_done = 0;
			b->line_len = 0;
		}
		*got = (long)take;
		return 0;
	}
	if (s->dev) {
		*got = 0;
		return 0;
	}
	uint8_t *tmp = kmalloc(n ? n : 1);
	if (!tmp)
		return 8;
	long r = vfs_read(s->f, tmp, n);
	if (r < 0) {
		kfree(tmp);
		return dos_error_code((int)r);
	}
	uint32_t a = buf & c->a20;
	if (a + (uint32_t)r <= c->mem_size && !(a < 0xB0000 && a + (uint32_t)r > 0xA0000))
		memcpy(c->mem + a, tmp, (size_t)r);
	else
		for (long k = 0; k < r; k++)
			x86_wr8(c, buf + (uint32_t)k, tmp[k]);
	kfree(tmp);
	*got = r;
	return 0;
}

static int do_write(struct dosbox *b, int h, uint32_t buf, uint32_t n, long *put)
{
	struct x86 *c = &b->cpu;
	int i = sft_of(b, h);
	if (i < 0)
		return 6;
	struct sft *s = &b->sft[i];
	if (s->dev) {
		*put = dev_write(b, s->dev, buf, n);
		return 0;
	}
	if (!n) {		/* writing nothing truncates there */
		*put = 0;
		return 0;
	}
	uint8_t *tmp = kmalloc(n);
	if (!tmp)
		return 8;
	for (uint32_t k = 0; k < n; k++)
		tmp[k] = x86_rd8(c, buf + k);
	long w = vfs_write(s->f, tmp, n);
	kfree(tmp);
	if (w < 0)
		return dos_error_code((int)w);
	*put = w;
	return 0;
}

/* ---- FindFirst / FindNext ------------------------------------------------------------ */

static void dta_fill(struct dosbox *b, struct vstat *st, int slot)
{
	uint32_t d = sl(b->dta_seg, b->dta_off);
	int y, mo, dd, h, mi, s;
	epoch_to_date(st->mtime, &y, &mo, &dd, &h, &mi, &s);
	if (y < 1980)
		y = 1980;
	mbw(b, d + 0, (uint8_t)(b->drive + 1));
	mbw(b, d + 13, (uint8_t)slot);		/* which search this is, for FindNext */
	mbw(b, d + 14, 0xB0);
	mbw(b, d + 21, st->type == VFS_DIR ? (uint8_t)(st->attr | A_DIR) : st->attr);
	mww(b, d + 22, (uint16_t)(h << 11 | mi << 5 | s / 2));
	mww(b, d + 24, (uint16_t)((y - 1980) << 9 | mo << 5 | dd));
	mdw(b, d + 26, (uint32_t)st->size);
	const char *name = st->short_name[0] ? st->short_name : st->name;
	int i = 0;
	for (; name[i] && i < 12; i++)
		mbw(b, d + 30 + (uint32_t)i, (uint8_t)toupper(name[i]));
	mbw(b, d + 30 + (uint32_t)i, 0);
}

struct find_ctx {
	struct search *s;
	const char *pat;
	uint8_t attr;
};

static int find_cb(void *arg, struct vstat *st)
{
	struct find_ctx *f = arg;
	uint8_t a = st->attr | (st->type == VFS_DIR ? A_DIR : 0);
	if ((a & (A_HIDDEN | A_SYSTEM | A_DIR)) & ~f->attr)
		return 0;
	const char *sn = st->short_name[0] ? st->short_name : st->name;
	if (!wildmatch(f->pat, sn) && !wildmatch(f->pat, st->name)) {
		/* "*.*" matches names without a dot too */
		if (!(strcmp(f->pat, "*.*") == 0))
			return 0;
	}
	struct vstat *n = krealloc(f->s->ents, sizeof(struct vstat) * (size_t)(f->s->n + 1));
	if (!n)
		return 1;
	f->s->ents = n;
	f->s->ents[f->s->n++] = *st;
	return 0;
}

static void find_first(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	char spec[128], abs[VFS_PATH_MAX], dir[VFS_PATH_MAX];
	get_str(b, ds_dx(c), spec, sizeof spec);
	if (dos_resolve(b, spec, abs, sizeof abs)) {
		fail(b, 3);
		return;
	}
	if (CX(c) & 0x08) {		/* the volume label */
		char label[32];
		uint64_t fr, tot;
		if (!vfs_info(abs[0], label, sizeof label, &fr, &tot) && label[0]) {
			struct vstat st;
			memset(&st, 0, sizeof st);
			strncpy0(st.name, label, sizeof st.name);
			st.attr = 0x08;
			dta_fill(b, &st, 0xFF);
			mbw(b, sl(b->dta_seg, b->dta_off) + 21, 0x08);
			ok(c);
			return;
		}
	}
	vfs_dirname(abs, dir, sizeof dir);
	/* reuse the slot this DTA had, else the oldest */
	int slot = -1;
	for (int i = 0; i < MAX_SEARCH; i++)
		if (!b->search[i].used) {
			slot = i;
			break;
		}
	if (slot < 0)
		slot = b->search_next++ % MAX_SEARCH;
	struct search *s = &b->search[slot];
	kfree(s->ents);
	memset(s, 0, sizeof *s);
	s->used = 1;
	struct find_ctx f = { s, vfs_basename(abs), (uint8_t)CX(c) };
	if (vfs_list(dir, find_cb, &f) || !s->n) {
		kfree(s->ents);
		memset(s, 0, sizeof *s);
		fail(b, 0x12);
		return;
	}
	dta_fill(b, &s->ents[s->i++], slot);
	if (s->i >= s->n) {
		kfree(s->ents);
		memset(s, 0, sizeof *s);
	}
	ok(c);
}

static void find_next(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	int slot = mb(b, sl(b->dta_seg, b->dta_off) + 13);
	if (slot >= MAX_SEARCH || !b->search[slot].used) {
		fail(b, 0x12);
		return;
	}
	struct search *s = &b->search[slot];
	dta_fill(b, &s->ents[s->i++], slot);
	if (s->i >= s->n) {
		kfree(s->ents);
		memset(s, 0, sizeof *s);
	}
	ok(c);
}

/* ---- FCBs ------------------------------------------------------------------------------ */

static uint32_t fcb_at(struct x86 *c)
{
	uint32_t a = ds_dx(c);
	if (x86_rd8(c, a) == 0xFF)	/* extended FCB */
		a += 7;
	return a;
}

static void fcb_name(struct dosbox *b, uint32_t fcb, char *out, size_t n)
{
	char name[16];
	int k = 0;
	int drive = mb(b, fcb);
	if (drive) {
		name[k++] = (char)('A' + drive - 1);
		name[k++] = ':';
	}
	for (int i = 0; i < 8; i++) {
		char ch = (char)mb(b, fcb + 1 + (uint32_t)i);
		if (ch != ' ')
			name[k++] = ch;
	}
	if (mb(b, fcb + 9) != ' ') {
		name[k++] = '.';
		for (int i = 0; i < 3; i++) {
			char ch = (char)mb(b, fcb + 9 + (uint32_t)i);
			if (ch != ' ')
				name[k++] = ch;
		}
	}
	name[k] = 0;
	strncpy0(out, name, n);
}

/* FCB files keep their SFT index in the reserved bytes */
static int fcb_sft(struct dosbox *b, uint32_t fcb)
{
	int i = mb(b, fcb + 0x18);
	if (i < 5 || i >= MAX_SFT || !b->sft[i].refs)
		return -1;
	return i;
}

static void fcb_open(struct dosbox *b, int create)
{
	struct x86 *c = &b->cpu;
	uint32_t fcb = fcb_at(c);
	char name[16], abs[VFS_PATH_MAX];
	fcb_name(b, fcb, name, sizeof name);
	if (dos_resolve(b, name, abs, sizeof abs)) {
		setlo(c, R_EAX, 0xFF);
		return;
	}
	int i = sft_new(b);
	if (i < 0) {
		setlo(c, R_EAX, 0xFF);
		return;
	}
	int e;
	struct vfile *f = vfs_open(abs, create ? (O_READ | O_WRITE | O_CREATE | O_TRUNC) : (O_READ | O_WRITE), &e);
	if (!f && !create)
		f = vfs_open(abs, O_READ, &e);
	if (!f) {
		setlo(c, R_EAX, 0xFF);
		return;
	}
	memset(&b->sft[i], 0, sizeof b->sft[i]);
	strncpy0(b->sft[i].path, abs, sizeof b->sft[i].path);
	b->sft[i].f = f;
	b->sft[i].refs = 1;
	strncpy0(b->sft[i].name, name, sizeof b->sft[i].name);
	if (!mb(b, fcb))
		mbw(b, fcb, (uint8_t)(b->drive + 1));
	mww(b, fcb + 0x0C, 0);			/* current block */
	mww(b, fcb + 0x0E, 128);		/* record size */
	mdw(b, fcb + 0x10, (uint32_t)vfs_size(f));
	mww(b, fcb + 0x14, 0x5821);
	mww(b, fcb + 0x16, 0);
	mbw(b, fcb + 0x18, (uint8_t)i);
	mbw(b, fcb + 0x20, 0);			/* current record */
	setlo(c, R_EAX, 0);
}

static void fcb_close(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	uint32_t fcb = fcb_at(c);
	int i = fcb_sft(b, fcb);
	if (i >= 0)
		sft_release(b, i);
	mbw(b, fcb + 0x18, 0);
	setlo(c, R_EAX, 0);
}

/* sequential or random record I/O: returns the AL result */
static uint8_t fcb_rw(struct dosbox *b, uint32_t fcb, uint32_t rec, int write, uint32_t count, uint32_t *done)
{
	int i = fcb_sft(b, fcb);
	if (i < 0)
		return 1;
	uint32_t rsz = mw(b, fcb + 0x0E);
	if (!rsz)
		rsz = 128;
	struct vfile *f = b->sft[i].f;
	uint32_t dta = sl(b->dta_seg, b->dta_off);
	*done = 0;
	for (uint32_t r = 0; r < count; r++) {
		vfs_seek(f, (uint64_t)(rec + r) * rsz);
		uint8_t buf[512];
		uint32_t n = MIN(rsz, (uint32_t)sizeof buf);
		if (write) {
			for (uint32_t k = 0; k < n; k++)
				buf[k] = mb(b, dta + r * rsz + k);
			if (vfs_write(f, buf, n) != (long)n)
				return 1;
			uint64_t end = (uint64_t)(rec + r + 1) * rsz;
			if (end > md(b, fcb + 0x10))
				mdw(b, fcb + 0x10, (uint32_t)end);
		} else {
			long got = vfs_read(f, buf, n);
			if (got <= 0)
				return 1;
			for (uint32_t k = 0; k < n; k++)
				mbw(b, dta + r * rsz + k, k < (uint32_t)got ? buf[k] : 0);
			(*done)++;
			if ((uint32_t)got < n)
				return 3;	/* a partial record: end of file */
			continue;
		}
		(*done)++;
	}
	return 0;
}

static void fcb_seq(struct dosbox *b, int write)
{
	struct x86 *c = &b->cpu;
	uint32_t fcb = fcb_at(c);
	uint32_t rec = (uint32_t)mw(b, fcb + 0x0C) * 128 + mb(b, fcb + 0x20);
	uint32_t done;
	uint8_t r = fcb_rw(b, fcb, rec, write, 1, &done);
	if (done) {
		rec++;
		mww(b, fcb + 0x0C, (uint16_t)(rec / 128));
		mbw(b, fcb + 0x20, (uint8_t)(rec % 128));
	}
	setlo(c, R_EAX, r);
}

static void fcb_random(struct dosbox *b, int write, int block)
{
	struct x86 *c = &b->cpu;
	uint32_t fcb = fcb_at(c);
	uint32_t rec = md(b, fcb + 0x21);
	if (mw(b, fcb + 0x0E) < 64)
		rec &= 0xFFFFFF;
	uint32_t n = block ? CX(c) : 1, done;
	uint8_t r = fcb_rw(b, fcb, rec, write, n, &done);
	if (block) {
		set16(c, R_ECX, (uint16_t)done);
		rec += done;
		mdw(b, fcb + 0x21, rec);
	}
	mww(b, fcb + 0x0C, (uint16_t)(rec / 128));
	mbw(b, fcb + 0x20, (uint8_t)(rec % 128));
	setlo(c, R_EAX, r);
}

/* parse a filename into an FCB (AH=29h) */
static void fcb_parse(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	uint32_t src = c->s[S_DS].base + SI(c), dst = c->s[S_ES].base + DI(c);
	uint8_t flags = AL(c);
	uint32_t p = src;
	if (flags & 1)
		while (mb(b, p) == ' ' || mb(b, p) == '\t' || mb(b, p) == ':' || mb(b, p) == ';' ||
		    mb(b, p) == ',' || mb(b, p) == '=' || mb(b, p) == '+')
			p++;
	int wild = 0;
	if (!(flags & 2))
		mbw(b, dst, 0);
	if (mb(b, p) && mb(b, p + 1) == ':') {
		mbw(b, dst, (uint8_t)(toupper(mb(b, p)) - 'A' + 1));
		p += 2;
	}
	char name[8], ext[3];
	memset(name, ' ', 8);
	memset(ext, ' ', 3);
	int i = 0;
	static const char stops[] = " \t.\"/\\[]:|<>+=;,\r";
	while (mb(b, p) && !strchr(stops, mb(b, p))) {
		char ch = (char)toupper(mb(b, p++));
		if (ch == '*') {
			while (i < 8)
				name[i++] = '?';
			wild = 1;
			continue;
		}
		if (ch == '?')
			wild = 1;
		if (i < 8)
			name[i++] = ch;
	}
	if (mb(b, p) == '.') {
		p++;
		i = 0;
		while (mb(b, p) && !strchr(stops, mb(b, p))) {
			char ch = (char)toupper(mb(b, p++));
			if (ch == '*') {
				while (i < 3)
					ext[i++] = '?';
				wild = 1;
				continue;
			}
			if (ch == '?')
				wild = 1;
			if (i < 3)
				ext[i++] = ch;
		}
	}
	int has_name = name[0] != ' ';
	if (has_name || !(flags & 4))
		for (int k = 0; k < 8; k++)
			mbw(b, dst + 1 + (uint32_t)k, (uint8_t)name[k]);
	if (ext[0] != ' ' || !(flags & 8))
		for (int k = 0; k < 3; k++)
			mbw(b, dst + 9 + (uint32_t)k, (uint8_t)ext[k]);
	set16(c, R_ESI, (uint16_t)(SI(c) + (p - src)));
	setlo(c, R_EAX, wild ? 1 : 0);
}

static void fcb_find(struct dosbox *b, int first)
{
	struct x86 *c = &b->cpu;
	uint32_t fcb = fcb_at(c);
	struct search *s = &b->search[MAX_SEARCH - 1];
	if (first) {
		char name[16], abs[VFS_PATH_MAX], dir[VFS_PATH_MAX];
		fcb_name(b, fcb, name, sizeof name);
		if (dos_resolve(b, name, abs, sizeof abs)) {
			setlo(c, R_EAX, 0xFF);
			return;
		}
		vfs_dirname(abs, dir, sizeof dir);
		kfree(s->ents);
		memset(s, 0, sizeof *s);
		s->used = 1;
		struct find_ctx f = { s, vfs_basename(abs), 0x16 };
		if (vfs_list(dir, find_cb, &f) || !s->n) {
			setlo(c, R_EAX, 0xFF);
			return;
		}
	}
	if (!s->used || s->i >= s->n) {
		setlo(c, R_EAX, 0xFF);
		return;
	}
	struct vstat *st = &s->ents[s->i++];
	uint32_t d = sl(b->dta_seg, b->dta_off);
	mbw(b, d, (uint8_t)(b->drive + 1));
	const char *n = st->short_name[0] ? st->short_name : st->name;
	for (int k = 0; k < 11; k++)
		mbw(b, d + 1 + (uint32_t)k, ' ');
	int k = 0;
	for (; *n && *n != '.' && k < 8; n++, k++)
		mbw(b, d + 1 + (uint32_t)k, (uint8_t)toupper(*n));
	if (*n == '.')
		for (n++, k = 0; *n && k < 3; n++, k++)
			mbw(b, d + 9 + (uint32_t)k, (uint8_t)toupper(*n));
	mdw(b, d + 0x1D, (uint32_t)st->size);
	setlo(c, R_EAX, 0);
}

/* ---- programs: PSPs, loading, EXEC and exit ------------------------------------------ */

static void make_psp(struct dosbox *b, uint16_t psp, uint16_t mem_top, uint16_t parent, uint16_t env)
{
	uint32_t p = sl(psp, 0);
	for (int i = 0; i < 0x100; i++)
		mbw(b, p + (uint32_t)i, 0);
	mbw(b, p + 0x00, 0xCD);		/* INT 20h */
	mbw(b, p + 0x01, 0x20);
	mww(b, p + 0x02, mem_top);
	mbw(b, p + 0x05, 0x9A);		/* CALL FAR to the CP/M-style entry */
	mww(b, p + 0x06, 0xFEF0);
	mww(b, p + 0x08, 0xF01D);
	mdw(b, p + 0x0A, md(b, 0x22 * 4));
	mdw(b, p + 0x0E, md(b, 0x23 * 4));
	mdw(b, p + 0x12, md(b, 0x24 * 4));
	mww(b, p + 0x16, parent ? parent : psp);
	/* the job file table: inherited handles, or the standard five */
	uint32_t jft = p + 0x18;
	for (int i = 0; i < JFT_SIZE; i++)
		mbw(b, jft + (uint32_t)i, 0xFF);
	if (parent && parent != psp) {
		uint32_t pj = sl(mw(b, sl(parent, 0x36)), mw(b, sl(parent, 0x34)));
		int psize = mw(b, sl(parent, 0x32));
		for (int i = 0; i < JFT_SIZE && i < psize; i++) {
			uint8_t s = mb(b, pj + (uint32_t)i);
			if (s != 0xFF && s < MAX_SFT && b->sft[s].refs) {
				mbw(b, jft + (uint32_t)i, s);
				b->sft[s].refs++;
			}
		}
	} else {
		for (int i = 0; i < 5; i++) {
			mbw(b, jft + (uint32_t)i, (uint8_t)(i < 3 ? 0 : i - 2));
			b->sft[i < 3 ? 0 : i - 2].refs++;
		}
	}
	mww(b, p + 0x2C, env);
	mww(b, p + 0x32, JFT_SIZE);
	mww(b, p + 0x34, 0x18);
	mww(b, p + 0x36, psp);
	mdw(b, p + 0x38, 0xFFFFFFFF);
	mww(b, p + 0x40, 0x0005);	/* DOS version 5.0 */
	mbw(b, p + 0x50, 0xCD);		/* INT 21h; RETF */
	mbw(b, p + 0x51, 0x21);
	mbw(b, p + 0x52, 0xCB);
	for (int i = 0; i < 11; i++) {
		mbw(b, p + 0x5D + (uint32_t)i, ' ');
		mbw(b, p + 0x6D + (uint32_t)i, ' ');
	}
	mbw(b, p + 0x81, '\r');
}

static void set_tail(struct dosbox *b, uint16_t psp, const char *tail)
{
	uint32_t p = sl(psp, 0);
	size_t n = MIN(strlen(tail), (size_t)126);
	mbw(b, p + 0x80, (uint8_t)n);
	for (size_t i = 0; i < n; i++)
		mbw(b, p + 0x81 + (uint32_t)i, (uint8_t)tail[i]);
	mbw(b, p + 0x81 + (uint32_t)n, '\r');
	/* the default FCBs get the first two arguments */
	struct x86 *c = &b->cpu;
	uint32_t save_si = c->r[R_ESI], save_di = c->r[R_EDI], save_ax = c->r[R_EAX];
	struct xseg sds = c->s[S_DS], ses = c->s[S_ES];
	x86_load_seg_real(c, S_DS, psp);
	x86_load_seg_real(c, S_ES, psp);
	set16(c, R_ESI, 0x81);
	set16(c, R_EDI, 0x5C);
	setlo(c, R_EAX, 1);
	fcb_parse(b);
	set16(c, R_EDI, 0x6C);
	setlo(c, R_EAX, 1);
	fcb_parse(b);
	c->r[R_ESI] = save_si;
	c->r[R_EDI] = save_di;
	c->r[R_EAX] = save_ax;
	c->s[S_DS] = sds;
	c->s[S_ES] = ses;
}

/* a copy of the parent's environment (or ours), plus the program's name */
static uint16_t make_env(struct dosbox *b, uint16_t from, const char *path, uint16_t owner)
{
	char buf[2048];
	int n = 0;
	if (from) {
		uint32_t a = sl(from, 0);
		while (n < (int)sizeof buf - 200) {
			uint8_t ch = mb(b, a + (uint32_t)n);
			buf[n++] = (char)ch;
			if (!ch && !mb(b, a + (uint32_t)n))
				break;
		}
		buf[n++] = 0;
	} else {
		/* the bdos environment, so PATH and friends carry over */
		static const char *const defs[] = { "COMSPEC=C:\\COMMAND.COM", "PATH=C:\\BDOS;C:\\DOSAPPS",
		    "PROMPT=$P$G", "BLASTER=A220 I7 D1 H5 T6", "TEMP=C:\\", "TMP=C:\\" };
		for (size_t i = 0; i < NELEM(defs); i++) {
			size_t l = strlen(defs[i]) + 1;
			memcpy(buf + n, defs[i], l);
			n += (int)l;
		}
		buf[n++] = 0;
	}
	buf[n++] = 1;
	buf[n++] = 0;
	size_t pl = strlen(path) + 1;
	for (size_t i = 0; i < pl; i++)		/* in capitals, as DOS gives it */
		buf[n + (int)i] = (char)toupper((unsigned char)path[i]);
	n += (int)pl;
	uint16_t paras = (uint16_t)((n + 15) / 16);
	uint16_t seg = mcb_alloc(b, paras, owner, NULL);
	if (!seg)
		return 0;
	for (int i = 0; i < n; i++)
		mbw(b, sl(seg, 0) + (uint32_t)i, (uint8_t)buf[i]);
	return seg;
}

static int is_command_com(const char *path)
{
	const char *base = vfs_basename(path);
	return !strcasecmp(base, "COMMAND.COM") || !strcasecmp(base, "COMMAND");
}

/* COMMAND.COM: the bdos shell stands in */
static int run_bdos_command(struct dosbox *b, const char *tail)
{
	while (*tail == ' ')
		tail++;
	char line[256];
	if (!strncasecmp(tail, "/C", 2) || !strncasecmp(tail, "/K", 2)) {
		strncpy0(line, tail + 2, sizeof line);
		char *e = strchr(line, '\r');
		if (e)
			*e = 0;
		dosbox_set_graphics(b, 0);
		return shell_exec(line);
	}
	/* an interactive shell, until EXIT */
	dosbox_set_graphics(b, 0);
	kprintf("\r\nbdos shell (type EXIT to return to the program)\r\n");
	for (;;) {
		char cwd[VFS_PATH_MAX];
		strncpy0(cwd, task_current()->cwd, sizeof cwd);
		kprintf("%s>", cwd);
		if (read_line(line, sizeof line) < 0)
			break;
		const char *p = line;
		while (*p == ' ')
			p++;
		if (!strcasecmp(p, "exit"))
			break;
		shell_exec(p);
	}
	return 0;
}

/*
 * Load a program into a new PSP. For EXEC, the caller's state is kept on
 * the box's EXEC stack and comes back when the child ends. Returns 0, or a
 * DOS error code.
 */
int dos_load(struct dosbox *b, const char *path, const char *tail, int is_exec, uint16_t env_from)
{
	struct x86 *c = &b->cpu;
	int e;
	size_t n;
	char *img = vfs_slurp(path, &n, &e);
	if (!img)
		return dos_error_code(e);
	int exe = n >= 2 && ((img[0] == 'M' && img[1] == 'Z') || (img[0] == 'Z' && img[1] == 'M'));
	uint16_t h[16] = { 0 };
	uint32_t body = (uint32_t)n, hdr = 0;
	if (exe) {
		if (n < 28) {
			kfree(img);
			return 11;
		}
		for (int i = 0; i < 16 && (size_t)(i * 2 + 1) < n; i++)
			h[i] = (uint16_t)((uint8_t)img[i * 2] | (uint8_t)img[i * 2 + 1] << 8);
		uint32_t size = h[2] * 512u - (h[1] ? 512 - h[1] : 0);
		if (size > n || !h[2])
			size = (uint32_t)n;
		hdr = h[4] * 16u;
		if (hdr >= size) {
			kfree(img);
			return 11;
		}
		body = size - hdr;
	} else if (n > 0xFF00) {
		kfree(img);
		return 8;
	}
	uint16_t parent = is_exec ? b->psp : 0;
	/* the environment first, then the program gets memory. The block is
	 * DOS's (owner 8) until the program's PSP exists: owner 0 would mean
	 * free, and the program's own block would be laid right over it */
	uint16_t env = make_env(b, env_from, path, 0x0008);
	if (!env) {
		kfree(img);
		return 8;
	}
	uint16_t largest;
	uint16_t need = (uint16_t)(PSP_SIZE_PARAS + (body + 15) / 16);
	uint16_t want;
	if (exe) {
		uint32_t w = need + (uint32_t)h[5];		/* minimum extra */
		uint32_t mx = h[6] ? need + (uint32_t)h[6] : 0xFFFF;
		if (w > 0xFFFF)
			w = 0xFFFF;
		mcb_alloc(b, 0xFFFF, 0, &largest);
		if (largest < w) {
			mcb_free(b, env);
			kfree(img);
			return 8;
		}
		want = (uint16_t)MIN(mx > 0xFFFF ? 0xFFFF : mx, (uint32_t)largest);
		if (!h[5] && !h[6])
			want = largest;		/* "load high": the whole block */
	} else {
		mcb_alloc(b, 0xFFFF, 0, &largest);
		if (largest < need + 0x10) {
			mcb_free(b, env);
			kfree(img);
			return 8;
		}
		want = largest;
	}
	uint16_t psp = mcb_alloc(b, want, 0, NULL);
	if (!psp) {
		mcb_free(b, env);
		kfree(img);
		return 8;
	}
	mww(b, sl((uint16_t)(psp - 1), 1), psp);
	mww(b, sl((uint16_t)(env - 1), 1), psp);
	mcb_name(b, psp, path);
	uint16_t top = (uint16_t)(psp + want);

	if (is_exec) {
		/* remember the parent, to come back to */
		if (b->exec_depth >= (int)NELEM(b->exec)) {
			mcb_free(b, psp);
			mcb_free(b, env);
			kfree(img);
			return 8;
		}
		struct exec_frame *f = &b->exec[b->exec_depth++];
		f->psp = b->psp;
		f->ss = c->s[S_SS].sel;
		f->esp = c->r[R_ESP];
		for (int i = 0; i < 8; i++)
			f->regs[i] = c->r[i];
		f->ds = c->s[S_DS].sel;
		f->es = c->s[S_ES].sel;
		f->dta = (uint32_t)b->dta_seg << 16 | b->dta_off;
		mww(b, sl(b->psp, 0x2E), (uint16_t)c->r[R_ESP]);
		mww(b, sl(b->psp, 0x30), c->s[S_SS].sel);
	}
	make_psp(b, psp, top, parent, env);
	b->psp = psp;
	set_tail(b, psp, tail);
	b->dta_seg = psp;
	b->dta_off = 0x80;

	uint16_t load = (uint16_t)(psp + PSP_SIZE_PARAS);
	if (exe) {
		uint32_t at = sl(load, 0);
		for (uint32_t i = 0; i < body && at + i < c->mem_size; i++)
			c->mem[at + i] = (uint8_t)img[hdr + i];
		for (int i = 0; i < h[3]; i++) {
			uint32_t r = h[12] + (uint32_t)i * 4;
			if (r + 4 > n)
				break;
			uint16_t off = (uint16_t)((uint8_t)img[r] | (uint8_t)img[r + 1] << 8);
			uint16_t seg = (uint16_t)((uint8_t)img[r + 2] | (uint8_t)img[r + 3] << 8);
			uint32_t a = sl((uint16_t)(load + seg), off);
			mww(b, a, (uint16_t)(mw(b, a) + load));
		}
		x86_load_seg_real(c, S_CS, (uint16_t)(load + h[11]));
		c->eip = h[10];
		x86_load_seg_real(c, S_SS, (uint16_t)(load + h[7]));
		c->r[R_ESP] = h[8];
	} else {
		uint32_t at = sl(psp, 0x100);
		for (uint32_t i = 0; i < n; i++)
			c->mem[at + i] = (uint8_t)img[i];
		x86_load_seg_real(c, S_CS, psp);
		c->eip = 0x100;
		x86_load_seg_real(c, S_SS, psp);
		uint32_t sp = (uint32_t)(top - psp) * 16;
		c->r[R_ESP] = sp > 0xFFFE || sp == 0 ? 0xFFFE : (sp - 2) & 0xFFFE;
		mww(b, sl(psp, (uint16_t)c->r[R_ESP]), 0);	/* RET goes to PSP:0 - INT 20h */
	}
	kfree(img);
	x86_load_seg_real(c, S_DS, psp);
	x86_load_seg_real(c, S_ES, psp);
	c->r[R_EAX] = 0;
	c->r[R_EBX] = 0;
	c->r[R_ECX] = 0x00FF;
	c->r[R_EDX] = psp;
	c->r[R_ESI] = c->eip;
	c->r[R_EDI] = c->r[R_ESP];
	c->r[R_EBP] = 0x091C;
	x86_set_flags(c, FL_IF | 2, 0xFFFF);
	/* the child returns to INT 22h: the instruction after the parent's INT 21h */
	if (is_exec) {
		struct exec_frame *f = &b->exec[b->exec_depth - 1];
		uint32_t sp = f->ss * 16 + (f->esp & 0xFFFF);
		mww(b, sl(psp, 0x0A), mw(b, sp));
		mww(b, sl(psp, 0x0C), mw(b, sp + 2));
	}
	return 0;
}

/* a program ends: free it, and go back to whoever EXEC'd it */
void dos_terminate(struct dosbox *b, int code, int tsr_paras)
{
	struct x86 *c = &b->cpu;
	uint16_t psp = b->psp;
	dpmi_terminating(b, psp);
	/* the vectors it was given back */
	mdw(b, 0x22 * 4, md(b, sl(psp, 0x0A)));
	mdw(b, 0x23 * 4, md(b, sl(psp, 0x0E)));
	mdw(b, 0x24 * 4, md(b, sl(psp, 0x12)));
	b->retcode = (tsr_paras ? 0x300 : 0) | (code & 0xFF);
	if (b->exec_depth == 0) {
		b->exit_code = code & 0xFF;
		b->running = 0;
		c->state = XS_STOP;	/* not one more instruction */
		dos_close_all(b);
		return;
	}
	if (tsr_paras) {
		mcb_resize(b, psp, (uint16_t)tsr_paras, NULL);
	} else {
		dos_close_all(b);
		mcb_free_owner(b, psp);
	}
	struct exec_frame *f = &b->exec[--b->exec_depth];
	b->psp = f->psp;
	for (int i = 0; i < 8; i++)
		c->r[i] = f->regs[i];
	x86_load_seg_real(c, S_SS, (uint16_t)f->ss);
	c->r[R_ESP] = f->esp;
	x86_load_seg_real(c, S_DS, f->ds);
	x86_load_seg_real(c, S_ES, f->es);
	b->dta_seg = (uint16_t)(f->dta >> 16);
	b->dta_off = (uint16_t)f->dta;
	/* back out through the INT 21h stub's IRET, with CF clear */
	if (c->cr0 & CR0_PE)
		c->cr0 &= ~CR0_PE;
	x86_load_seg_real(c, S_CS, BIOS_SEG);
	c->eip = (uint32_t)(ROM_VECTORS + 0x21 * 4 + 2);
	bios_set_flag(c, FL_CF, 0);
	c->state = XS_WAIT;	/* stop this x86_run: the world just changed */
}

static void do_exec(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	char name[128], abs[VFS_PATH_MAX];
	get_str(b, ds_dx(c), name, sizeof name);
	uint32_t blk = c->s[S_ES].base + BX(c);
	uint16_t env = x86_rd16(c, blk);
	uint32_t tail_a = sl(x86_rd16(c, blk + 4), x86_rd16(c, blk + 2));
	char tail[130];
	int tl = x86_rd8(c, tail_a);
	if (tl > 126)
		tl = 126;
	for (int i = 0; i < tl; i++)
		tail[i] = (char)x86_rd8(c, tail_a + 1 + (uint32_t)i);
	tail[tl] = 0;
	if (dos_resolve(b, name, abs, sizeof abs)) {
		fail(b, 2);
		return;
	}
	if (AL(c) == 0x03) {		/* load an overlay */
		uint32_t ov = c->s[S_ES].base + BX(c);
		uint16_t seg = x86_rd16(c, ov), reloc = x86_rd16(c, ov + 2);
		int e;
		size_t n;
		char *img = vfs_slurp(abs, &n, &e);
		if (!img) {
			dos_err(b, e);
			return;
		}
		uint32_t hdr = 0, body = (uint32_t)n;
		if (n > 28 && img[0] == 'M' && img[1] == 'Z') {
			hdr = (uint32_t)((uint8_t)img[8] | (uint8_t)img[9] << 8) * 16;
			uint16_t nrel = (uint16_t)((uint8_t)img[6] | (uint8_t)img[7] << 8);
			uint16_t rofs = (uint16_t)((uint8_t)img[24] | (uint8_t)img[25] << 8);
			uint32_t size = ((uint32_t)((uint8_t)img[4] | (uint8_t)img[5] << 8)) * 512 -
			    (((uint8_t)img[2] | (uint8_t)img[3] << 8) ? 512 - ((uint8_t)img[2] | (uint8_t)img[3] << 8) : 0);
			if (size < n)
				body = size;
			body -= hdr;
			for (uint32_t i = 0; i < body; i++)
				mbw(b, sl(seg, 0) + i, (uint8_t)img[hdr + i]);
			for (int i = 0; i < nrel; i++) {
				uint32_t r = rofs + (uint32_t)i * 4;
				if (r + 4 > n)
					break;
				uint16_t off = (uint16_t)((uint8_t)img[r] | (uint8_t)img[r + 1] << 8);
				uint16_t sg = (uint16_t)((uint8_t)img[r + 2] | (uint8_t)img[r + 3] << 8);
				uint32_t a = sl((uint16_t)(seg + sg), off);
				mww(b, a, (uint16_t)(mw(b, a) + reloc));
			}
		} else {
			for (uint32_t i = 0; i < body; i++)
				mbw(b, sl(seg, 0) + i, (uint8_t)img[i]);
		}
		kfree(img);
		ok(c);
		return;
	}
	const char *dot = strrchr(vfs_basename(abs), '.');
	int dosprog = dot && (!strcasecmp(dot, ".COM") || !strcasecmp(dot, ".EXE"));
	if (is_command_com(abs) || !strcasecmp(name, "COMMAND.COM")) {
		b->retcode = run_bdos_command(b, tail) & 0xFF;
		ok(c);
		return;
	}
	if (!dosprog) {
		/* a BASIC or C program, a batch file: bdos runs it */
		char line[300];
		snprintf(line, sizeof line, "\"%s\"%s", abs, tail);
		dosbox_set_graphics(b, 0);
		b->retcode = shell_exec(line) & 0xFF;
		ok(c);
		return;
	}
	if (AL(c) != 0x00) {
		fail(b, 1);
		return;
	}
	int r = dos_load(b, abs, tail, 1, env ? env : mw(b, sl(b->psp, 0x2C)));
	if (r) {
		fail(b, (uint16_t)r);
		return;
	}
	c->state = XS_WAIT;	/* the child starts on the next run */
}

/* ---- INT 21h ------------------------------------------------------------------------------ */

/* dos.trace=3: every DOS call, on COM1 only (the screen is the program's) */
static void log_call(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	char line[200];
	uint32_t sp = c->s[S_SS].base + (c->r[R_ESP] & 0xFFFF);
	int n = snprintf(line, sizeof line, "[dos %04X:%04X] AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X psp %04X env %04X",
	    x86_rd16(c, sp + 2), x86_rd16(c, sp), AX(c), BX(c), CX(c), DX(c), c->s[S_DS].sel, c->s[S_ES].sel,
	    b->psp, mw(b, sl(b->psp, 0x2C)));
	uint8_t fn = AH(c);
	if (fn == 0x3C || fn == 0x3D || fn == 0x41 || fn == 0x43 || fn == 0x4B || fn == 0x4E || fn == 0x5B ||
	    fn == 0x39 || fn == 0x3A || fn == 0x3B || fn == 0x6C || fn == 0x56) {
		char p[80];
		get_str(b, fn == 0x6C ? c->s[S_DS].base + SI(c) : ds_dx(c), p, sizeof p);
		n += snprintf(line + n, sizeof line - (size_t)n, " \"%s\"", p);
	}
	for (int i = 0; i < n && line[i]; i++)
		serial_putc(line[i]);
	serial_putc('\r');
	serial_putc('\n');
}

static int int21(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	char path[128], abs[VFS_PATH_MAX], abs2[VFS_PATH_MAX];
	int e;
	uint8_t fn = AH(c);
	if (b->log_dos)
		log_call(b);
	switch (fn) {
	case 0x00:
		dos_terminate(b, 0, 0);
		break;
	case 0x01:
		return read_char_fn(b, 1, 1);
	case 0x02: {
		int i = sft_of(b, 1);
		uint8_t ch = DL(c);
		if (i >= 0 && !b->sft[i].dev) {
			long put;
			uint32_t save = c->r[R_EDX];
			(void)save;
			uint8_t tmp = ch;
			vfs_write(b->sft[i].f, &tmp, 1);
			(void)put;
		} else {
			dos_con_out(b, ch);
		}
		setlo(c, R_EAX, ch);
		break;
	}
	case 0x03: case 0x04: case 0x05:
		setlo(c, R_EAX, 0);
		break;
	case 0x06:
		if (DL(c) == 0xFF) {
			if (b->pending_scan >= 0) {
				setlo(c, R_EAX, (uint8_t)b->pending_scan);
				b->pending_scan = -1;
				bios_set_flag(c, FL_ZF, 0);
			} else if (bios_key_avail(b)) {
				uint16_t k = dos_key_get(b);
				setlo(c, R_EAX, (uint8_t)k);
				if (!(k & 0xFF))
					b->pending_scan = k >> 8;
				bios_set_flag(c, FL_ZF, 0);
			} else {
				setlo(c, R_EAX, 0);
				bios_set_flag(c, FL_ZF, 1);
				b->idle_polls++;
			}
		} else {
			dos_con_out(b, DL(c));
		}
		break;
	case 0x07: case 0x08:
		return read_char_fn(b, 0, fn == 0x08);
	case 0x09: {
		uint32_t a = ds_dx(c);
		int i = sft_of(b, 1);
		for (int k = 0; k < 65535; k++) {
			uint8_t ch = x86_rd8(c, a + (uint32_t)k);
			if (ch == '$')
				break;
			if (i >= 0 && !b->sft[i].dev)
				vfs_write(b->sft[i].f, &ch, 1);
			else
				dos_con_out(b, ch);
		}
		setlo(c, R_EAX, '$');
		break;
	}
	case 0x0A: {
		uint32_t a = ds_dx(c);
		int max = x86_rd8(c, a);
		if (!b->line_active) {
			b->line_active = 1;
			b->line_len = 0;
		}
		int r = read_line_fn(b, max);
		if (r > 0)
			return 1;
		b->line_active = 0;
		if (r < 0)
			break;
		int len = MIN(b->line_len, max - 1);
		for (int k = 0; k < len; k++)
			x86_wr8(c, a + 2 + (uint32_t)k, (uint8_t)b->line[k]);
		x86_wr8(c, a + 2 + (uint32_t)len, '\r');
		x86_wr8(c, a + 1, (uint8_t)len);
		b->line_len = 0;
		break;
	}
	case 0x0B:
		setlo(c, R_EAX, bios_key_avail(b) || b->pending_scan >= 0 ? 0xFF : 0);
		if (!bios_key_avail(b))
			b->idle_polls++;
		break;
	case 0x0C: {
		/* flush the buffer, then one of the input functions */
		if (!b->flushed) {
			while (bios_key_avail(b))
				bios_key_get(b);
			b->pending_scan = -1;
			b->flushed = 1;
		}
		uint8_t sub = AL(c);
		if (sub == 1 || sub == 6 || sub == 7 || sub == 8 || sub == 0x0A) {
			sethi(c, R_EAX, sub);
			int r = int21(b);
			sethi(c, R_EAX, 0x0C);
			if (r)
				return 1;
		}
		b->flushed = 0;
		break;
	}
	case 0x0D:
		break;
	case 0x0E:
		if (DL(c) < 26 && vfs_drive((char)('A' + DL(c))))
			b->drive = DL(c);
		setlo(c, R_EAX, 26);
		break;
	case 0x0F: fcb_open(b, 0); break;
	case 0x10: fcb_close(b); break;
	case 0x11: fcb_find(b, 1); break;
	case 0x12: fcb_find(b, 0); break;
	case 0x13: {
		uint32_t fcb = fcb_at(c);
		char name[16];
		fcb_name(b, fcb, name, sizeof name);
		setlo(c, R_EAX, (dos_resolve(b, name, abs, sizeof abs) || vfs_delete(abs)) ? 0xFF : 0);
		break;
	}
	case 0x14: fcb_seq(b, 0); break;
	case 0x15: fcb_seq(b, 1); break;
	case 0x16: fcb_open(b, 1); break;
	case 0x17: {
		uint32_t fcb = fcb_at(c);
		char from[16], to[16];
		fcb_name(b, fcb, from, sizeof from);
		/* the new name is at offset 16 in the same FCB, same layout */
		char nm[16];
		int k = 0;
		for (int i = 0; i < 8; i++) {
			char ch = (char)mb(b, fcb + 17 + (uint32_t)i);
			if (ch != ' ')
				nm[k++] = ch;
		}
		if (mb(b, fcb + 25) != ' ') {
			nm[k++] = '.';
			for (int i = 0; i < 3; i++) {
				char ch = (char)mb(b, fcb + 25 + (uint32_t)i);
				if (ch != ' ')
					nm[k++] = ch;
			}
		}
		nm[k] = 0;
		strncpy0(to, nm, sizeof to);
		setlo(c, R_EAX, (dos_resolve(b, from, abs, sizeof abs) || dos_resolve(b, to, abs2, sizeof abs2) ||
		    vfs_rename(abs, abs2)) ? 0xFF : 0);
		break;
	}
	case 0x19:
		setlo(c, R_EAX, (uint8_t)b->drive);
		break;
	case 0x1A:
		b->dta_seg = c->s[S_DS].sel;
		b->dta_off = DX(c);
		break;
	case 0x1B: case 0x1C: case 0x36: {
		char label[32];
		uint64_t fr = 0, tot = 0;
		int drive = fn == 0x1B ? b->drive : (DL(c) ? DL(c) - 1 : b->drive);
		if (vfs_info((char)('A' + drive), label, sizeof label, &fr, &tot)) {
			set16(c, R_EAX, 0xFFFF);
			break;
		}
		if (fn == 0x36) {
			set16(c, R_EAX, 64);		/* sectors per cluster */
			set16(c, R_ECX, 512);
			set16(c, R_EBX, (uint16_t)MIN(fr / 32768, 65535));
			set16(c, R_EDX, (uint16_t)MIN(tot / 32768, 65535));
		} else {
			setlo(c, R_EAX, 64);
			set16(c, R_ECX, 512);
			set16(c, R_EDX, (uint16_t)MIN(tot / 32768, 65535));
			x86_load_seg_real(c, S_DS, DOS_DATA_SEG);
			set16(c, R_EBX, 0x0000);
			mbw(b, sl(DOS_DATA_SEG, 0), 0xF8);
		}
		break;
	}
	case 0x21: fcb_random(b, 0, 0); break;
	case 0x22: fcb_random(b, 1, 0); break;
	case 0x23: {
		uint32_t fcb = fcb_at(c);
		char name[16];
		struct vstat st;
		fcb_name(b, fcb, name, sizeof name);
		if (dos_resolve(b, name, abs, sizeof abs) || vfs_stat(abs, &st)) {
			setlo(c, R_EAX, 0xFF);
			break;
		}
		uint32_t rsz = mw(b, fcb + 0x0E) ? mw(b, fcb + 0x0E) : 128;
		mdw(b, fcb + 0x21, (uint32_t)((st.size + rsz - 1) / rsz));
		setlo(c, R_EAX, 0);
		break;
	}
	case 0x24: {
		uint32_t fcb = fcb_at(c);
		mdw(b, fcb + 0x21, (uint32_t)mw(b, fcb + 0x0C) * 128 + mb(b, fcb + 0x20));
		break;
	}
	case 0x25:
		mww(b, (uint32_t)AL(c) * 4, DX(c));
		mww(b, (uint32_t)AL(c) * 4 + 2, c->s[S_DS].sel);
		break;
	case 0x26: {
		uint32_t to = sl(DX(c), 0), from = sl(b->psp, 0);
		for (int i = 0; i < 0x100; i++)
			mbw(b, to + (uint32_t)i, mb(b, from + (uint32_t)i));
		break;
	}
	case 0x27: fcb_random(b, 0, 1); break;
	case 0x28: fcb_random(b, 1, 1); break;
	case 0x29: fcb_parse(b); break;
	case 0x2A: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		set16(c, R_ECX, (uint16_t)y);
		sethi(c, R_EDX, (uint8_t)mo);
		setlo(c, R_EDX, (uint8_t)d);
		setlo(c, R_EAX, (uint8_t)((rtc_epoch() / 86400 + 4) % 7));
		break;
	}
	case 0x2B: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		setlo(c, R_EAX, clock_set(CX(c), DH(c), DL(c), h, mi, s) ? 0xFF : 0);
		break;
	}
	case 0x2C: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		sethi(c, R_ECX, (uint8_t)h);
		setlo(c, R_ECX, (uint8_t)mi);
		sethi(c, R_EDX, (uint8_t)s);
		setlo(c, R_EDX, (uint8_t)((uptime_ms() / 10) % 100));
		break;
	}
	case 0x2D: {
		int y, mo, d, h, mi, s;
		arch_rtc(&y, &mo, &d, &h, &mi, &s);
		setlo(c, R_EAX, clock_set(y, mo, d, CH(c), CL(c), DH(c)) ? 0xFF : 0);
		break;
	}
	case 0x2E:
		b->verify = AL(c) & 1;
		break;
	case 0x2F:
		x86_load_seg_real(c, S_ES, b->dta_seg);
		set16(c, R_EBX, b->dta_off);
		break;
	case 0x30:
		set16(c, R_EAX, 0x0005);	/* DOS 5.00 */
		set16(c, R_EBX, AL(c) == 1 ? 0x0800 : 0xFF00);
		set16(c, R_ECX, 0);
		break;
	case 0x31:
		dos_terminate(b, AL(c), MAX(DX(c), (uint16_t)PSP_SIZE_PARAS));
		break;
	case 0x33:
		switch (AL(c)) {
		case 0x00: setlo(c, R_EDX, b->break_check); break;
		case 0x01: b->break_check = DL(c) & 1; break;
		case 0x05: setlo(c, R_EDX, 3); break;		/* booted from C: */
		case 0x06:
			set16(c, R_EBX, 0x0005);
			set16(c, R_EDX, 0x1000);		/* in the HMA */
			break;
		}
		break;
	case 0x34:
		x86_load_seg_real(c, S_ES, DOS_DATA_SEG);
		set16(c, R_EBX, INDOS_OFF);
		break;
	case 0x35:
		set16(c, R_EBX, mw(b, (uint32_t)AL(c) * 4));
		x86_load_seg_real(c, S_ES, mw(b, (uint32_t)AL(c) * 4 + 2));
		break;
	case 0x37:
		if (AL(c) == 0)
			setlo(c, R_EDX, '/');
		setlo(c, R_EAX, 0);
		break;
	case 0x38: {			/* country: the US */
		uint32_t a = ds_dx(c);
		for (int i = 0; i < 34; i++)
			x86_wr8(c, a + (uint32_t)i, 0);
		x86_wr16(c, a, 0);		/* date format: m d y */
		x86_wr8(c, a + 2, '$');
		x86_wr8(c, a + 7, ',');
		x86_wr8(c, a + 9, '.');
		x86_wr8(c, a + 11, '-');
		x86_wr8(c, a + 13, ':');
		x86_wr8(c, a + 16, 2);
		x86_wr8(c, a + 22, ',');
		set16(c, R_EBX, 1);
		ok(c);
		break;
	}
	case 0x39: case 0x3A: case 0x41:
		get_str(b, ds_dx(c), path, sizeof path);
		if (dos_resolve(b, path, abs, sizeof abs)) {
			fail(b, 3);
			break;
		}
		e = fn == 0x39 ? vfs_mkdir(abs) : fn == 0x3A ? vfs_rmdir(abs) : vfs_delete(abs);
		if (e)
			dos_err(b, fn == 0x41 && e == E_NOENT ? E_NOENT : e);
		else
			ok(c);
		break;
	case 0x3B: {
		struct vstat st;
		get_str(b, ds_dx(c), path, sizeof path);
		if (dos_resolve(b, path, abs, sizeof abs) || vfs_stat(abs, &st) || st.type != VFS_DIR) {
			fail(b, 3);
			break;
		}
		int d = toupper(abs[0]) - 'A';
		strncpy0(b->cwd[d], abs, sizeof b->cwd[d]);
		if (d == b->drive)
			strncpy0(task_current()->cwd, abs, sizeof task_current()->cwd);
		ok(c);
		break;
	}
	case 0x3C: case 0x5B: case 0x5A: {
		get_str(b, ds_dx(c), path, sizeof path);
		if (fn == 0x5A) {	/* a unique name in that directory */
			size_t l = strlen(path);
			if (l && path[l - 1] != '\\')
				strcat0(path, "\\", sizeof path);
			char name[16];
			snprintf(name, sizeof name, "B%07X.TMP", (unsigned)(uptime_us() & 0xFFFFFFF));
			strcat0(path, name, sizeof path);
			uint32_t a = ds_dx(c) + (uint32_t)strlen(path) - (uint32_t)strlen(name);
			put_str(b, a, name);
		}
		int h = open_file(b, path, 2, 1, fn == 0x5B);
		if (h < 0)
			break;
		set16(c, R_EAX, (uint16_t)h);
		ok(c);
		break;
	}
	case 0x3D: {
		get_str(b, ds_dx(c), path, sizeof path);
		int h = open_file(b, path, AL(c), 0, 0);
		if (h < 0)
			break;
		set16(c, R_EAX, (uint16_t)h);
		ok(c);
		break;
	}
	case 0x3E:
		if (handle_close(b, BX(c)))
			fail(b, 6);
		else
			ok(c);
		break;
	case 0x3F: {
		long got = 0;
		int r = do_read(b, BX(c), ds_dx(c), CX(c), &got);
		if (r < 0)
			return 1;
		if (r)
			fail(b, (uint16_t)r);
		else {
			set16(c, R_EAX, (uint16_t)got);
			ok(c);
		}
		break;
	}
	case 0x40: {
		long put = 0;
		int r = do_write(b, BX(c), ds_dx(c), CX(c), &put);
		if (r)
			fail(b, (uint16_t)r);
		else {
			set16(c, R_EAX, (uint16_t)put);
			ok(c);
		}
		break;
	}
	case 0x42: {
		int i = sft_of(b, BX(c));
		if (i < 0) {
			fail(b, 6);
			break;
		}
		struct sft *s = &b->sft[i];
		if (s->dev) {
			set16(c, R_EAX, 0);
			set16(c, R_EDX, 0);
			ok(c);
			break;
		}
		int32_t off = (int32_t)((uint32_t)CX(c) << 16 | DX(c));
		int64_t base = AL(c) == 0 ? 0 : AL(c) == 1 ? (int64_t)vfs_tell(s->f) : (int64_t)vfs_size(s->f);
		int64_t pos = base + off;
		if (pos < 0) {
			fail(b, 0x19);
			break;
		}
		vfs_seek(s->f, (uint64_t)pos);
		set16(c, R_EAX, (uint16_t)pos);
		set16(c, R_EDX, (uint16_t)(pos >> 16));
		ok(c);
		break;
	}
	case 0x43: {
		struct vstat st;
		get_str(b, ds_dx(c), path, sizeof path);
		if (dos_resolve(b, path, abs, sizeof abs) || vfs_stat(abs, &st)) {
			fail(b, 2);
			break;
		}
		if (AL(c) == 0) {
			set16(c, R_ECX, (uint16_t)(st.attr | (st.type == VFS_DIR ? A_DIR : 0)));
			ok(c);
		} else {
			e = vfs_setattr(abs, (uint8_t)(CX(c) & 0x27));
			if (e)
				dos_err(b, e);
			else
				ok(c);
		}
		break;
	}
	case 0x44: {
		int i = sft_of(b, BX(c));
		switch (AL(c)) {
		case 0x00:
			if (i < 0) {
				fail(b, 6);
				return 0;
			}
			if (b->sft[i].dev == DEV_CON)
				set16(c, R_EDX, 0x80D3);
			else if (b->sft[i].dev == DEV_NUL)
				set16(c, R_EDX, 0x8084);
			else if (b->sft[i].dev)
				set16(c, R_EDX, 0x80C0);
			else
				set16(c, R_EDX, (uint16_t)(b->drive & 0x1F));
			ok(c);
			break;
		case 0x01:
			ok(c);
			break;
		case 0x06:
			if (i < 0) {
				fail(b, 6);
				return 0;
			}
			if (b->sft[i].dev == DEV_CON)
				setlo(c, R_EAX, bios_key_avail(b) ? 0xFF : 0);
			else if (b->sft[i].dev)
				setlo(c, R_EAX, 0);
			else
				setlo(c, R_EAX, vfs_tell(b->sft[i].f) < vfs_size(b->sft[i].f) ? 0xFF : 0);
			ok(c);
			break;
		case 0x07:
			setlo(c, R_EAX, 0xFF);
			ok(c);
			break;
		case 0x08:
		{
			int d = BL(c) ? BL(c) - 1 : b->drive;
			set16(c, R_EAX, d <= 1 ? 0 : 1);	/* A: and B: are floppies */
		}
			ok(c);
			break;
		case 0x09:
			set16(c, R_EDX, 0);
			ok(c);
			break;
		case 0x0A:
			set16(c, R_EDX, 0);
			ok(c);
			break;
		case 0x0E:
			setlo(c, R_EAX, 0);
			ok(c);
			break;
		default:
			fail(b, 1);
		}
		break;
	}
	case 0x45: {
		int i = sft_of(b, BX(c));
		if (i < 0) {
			fail(b, 6);
			break;
		}
		int h = handle_new(b, i);
		if (h < 0) {
			fail(b, 4);
			break;
		}
		set16(c, R_EAX, (uint16_t)h);
		ok(c);
		break;
	}
	case 0x46: {
		int i = sft_of(b, BX(c));
		if (i < 0 || CX(c) >= jft_size(b)) {
			fail(b, 6);
			break;
		}
		handle_close(b, CX(c));
		mbw(b, jft_addr(b) + CX(c), (uint8_t)i);
		b->sft[i].refs++;
		ok(c);
		break;
	}
	case 0x47: {
		int d = DL(c) ? DL(c) - 1 : b->drive;
		if (d < 0 || d >= 26 || !vfs_drive((char)('A' + d))) {
			fail(b, 0x0F);
			break;
		}
		if (!b->cwd[d][0])
			snprintf(b->cwd[d], sizeof b->cwd[d], "%c:\\", 'A' + d);
		const char *cwd = b->cwd[d] + 3;
		uint32_t a = c->s[S_DS].base + SI(c);
		int k = 0;
		for (; cwd[k] && k < 63; k++)
			x86_wr8(c, a + (uint32_t)k, (uint8_t)toupper(cwd[k]));
		x86_wr8(c, a + (uint32_t)k, 0);
		ok(c);
		break;
	}
	case 0x48: {
		uint16_t largest;
		uint16_t seg = mcb_alloc(b, BX(c), b->psp, &largest);
		if (!seg) {
			set16(c, R_EBX, largest);
			fail(b, 8);
			break;
		}
		set16(c, R_EAX, seg);
		ok(c);
		break;
	}
	case 0x49:
		if (mcb_free(b, c->s[S_ES].sel))
			fail(b, 9);
		else
			ok(c);
		break;
	case 0x4A: {
		uint16_t largest = 0;
		int r = mcb_resize(b, c->s[S_ES].sel, BX(c), &largest);
		if (r) {
			set16(c, R_EBX, largest);
			fail(b, (uint16_t)r);
		} else {
			if (c->s[S_ES].sel == b->psp)
				mww(b, sl(b->psp, 2), (uint16_t)(b->psp + BX(c)));
			ok(c);
		}
		break;
	}
	case 0x4B:
		do_exec(b);
		break;
	case 0x4C:
		dos_terminate(b, AL(c), 0);
		break;
	case 0x4D:
		set16(c, R_EAX, (uint16_t)b->retcode);
		b->retcode = 0;
		break;
	case 0x4E: find_first(b); break;
	case 0x4F: find_next(b); break;
	case 0x50:
		b->psp = BX(c);
		break;
	case 0x51: case 0x62:
		set16(c, R_EBX, b->psp);
		break;
	case 0x52:
		x86_load_seg_real(c, S_ES, DOS_DATA_SEG);
		set16(c, R_EBX, LOL_OFF);
		break;
	case 0x54:
		setlo(c, R_EAX, (uint8_t)b->verify);
		break;
	case 0x56: {
		get_str(b, ds_dx(c), path, sizeof path);
		char to[128];
		get_str(b, c->s[S_ES].base + DI(c), to, sizeof to);
		if (dos_resolve(b, path, abs, sizeof abs) || dos_resolve(b, to, abs2, sizeof abs2)) {
			fail(b, 3);
			break;
		}
		e = vfs_rename(abs, abs2);
		if (e)
			dos_err(b, e);
		else
			ok(c);
		break;
	}
	case 0x57: {
		int i = sft_of(b, BX(c));
		if (i < 0) {
			fail(b, 6);
			break;
		}
		if (AL(c) == 0) {
			int y, mo, d, h, mi, s;
			epoch_to_date(rtc_epoch(), &y, &mo, &d, &h, &mi, &s);
			set16(c, R_ECX, (uint16_t)(h << 11 | mi << 5 | s / 2));
			set16(c, R_EDX, (uint16_t)((y - 1980) << 9 | mo << 5 | d));
		}
		ok(c);
		break;
	}
	case 0x58:
		if (AL(c) == 0)
			set16(c, R_EAX, b->alloc_strategy);
		else if (AL(c) == 1)
			b->alloc_strategy = BL(c);
		else if (AL(c) == 2)
			setlo(c, R_EAX, 0);
		ok(c);
		break;
	case 0x59:
		set16(c, R_EAX, (uint16_t)b->last_error);
		set16(c, R_EBX, 0x0D01);
		sethi(c, R_ECX, 1);
		break;
	case 0x5C:
		ok(c);
		break;
	case 0x60: {
		get_str(b, c->s[S_DS].base + SI(c), path, sizeof path);
		if (dos_resolve(b, path, abs, sizeof abs)) {
			fail(b, 3);
			break;
		}
		for (char *p = abs; *p; p++)
			*p = (char)toupper(*p);
		put_str(b, c->s[S_ES].base + DI(c), abs);
		ok(c);
		break;
	}
	case 0x63:
		fail(b, 1);
		break;
	case 0x65:
		if (AL(c) == 0x20 || AL(c) == 0x21 || AL(c) == 0x22) {
			/* uppercase a character or a string */
			if (AL(c) == 0x20)
				setlo(c, R_EDX, (uint8_t)toupper(DL(c)));
			else {
				uint32_t a = ds_dx(c);
				int n = AL(c) == 0x21 ? CX(c) : 65535;
				for (int k = 0; k < n; k++) {
					uint8_t ch = x86_rd8(c, a + (uint32_t)k);
					if (AL(c) == 0x22 && !ch)
						break;
					x86_wr8(c, a + (uint32_t)k, (uint8_t)toupper(ch));
				}
			}
			ok(c);
		} else if (AL(c) == 0x01) {
			uint32_t a = c->s[S_ES].base + DI(c);
			x86_wr8(c, a, 1);
			x86_wr16(c, a + 1, 38);
			x86_wr16(c, a + 3, 1);
			x86_wr16(c, a + 5, 437);
			set16(c, R_ECX, 41);
			ok(c);
		} else {
			fail(b, 1);
		}
		break;
	case 0x66:
		set16(c, R_EBX, 437);
		set16(c, R_EDX, 437);
		ok(c);
		break;
	case 0x67: case 0x68: case 0x6A:
		ok(c);
		break;
	case 0x6C: {
		get_str(b, c->s[S_DS].base + SI(c), path, sizeof path);
		uint8_t action = DL(c);
		struct vstat st;
		int exists = !dos_resolve(b, path, abs, sizeof abs) && !vfs_stat(abs, &st);
		int h = -1, how = 0;
		if (exists) {
			if ((action & 0x0F) == 0) {
				fail(b, 0x50);
				break;
			}
			if ((action & 0x0F) == 2) {
				h = open_file(b, path, BL(c) & 0x7F, 1, 0);
				how = 3;
			} else {
				h = open_file(b, path, BL(c) & 0x7F, 0, 0);
				how = 1;
			}
		} else {
			if (!(action & 0xF0)) {
				fail(b, 2);
				break;
			}
			h = open_file(b, path, BL(c) & 0x7F, 1, 0);
			how = 2;
		}
		if (h < 0)
			break;
		set16(c, R_EAX, (uint16_t)h);
		set16(c, R_ECX, (uint16_t)how);
		ok(c);
		break;
	}
	case 0x71:
		set16(c, R_EAX, 0x7100);	/* no long file name API */
		bios_set_flag(c, FL_CF, 1);
		break;
	default:
		fail(b, 1);
	}
	return 0;
}

void dos_int2f(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	switch (AX(c)) {
	case 0x1600:		/* Windows: not here */
		setlo(c, R_EAX, 0);
		return;
	case 0x1680:		/* release the time slice: a hint we're idle */
		b->idle_polls += 64;
		setlo(c, R_EAX, 0);
		return;
	case 0x1687:
		dpmi_detect(b);
		return;
	case 0x4300:		/* XMS installed */
		setlo(c, R_EAX, 0x80);
		return;
	case 0x4310:
		x86_load_seg_real(c, S_ES, BIOS_SEG);
		set16(c, R_EBX, ROM_XMS);
		return;
	case 0x4A01:		/* free HMA space */
		set16(c, R_EBX, 0);
		x86_load_seg_real(c, S_ES, 0xFFFF);
		set16(c, R_EDI, 0xFFFF);
		return;
	}
	/* nothing else is installed: leave AL alone, which says so */
}

int dos_hle(struct dosbox *b, int n)
{
	struct x86 *c = &b->cpu;
	switch (n) {
	case 0x20:
		dos_terminate(b, 0, 0);
		return 0;
	case 0x21: {
		mbw(b, sl(DOS_DATA_SEG, INDOS_OFF), 1);
		int r = int21(b);
		mbw(b, sl(DOS_DATA_SEG, INDOS_OFF), 0);
		return r;
	}
	case 0x22:
		dos_terminate(b, 0, 0);
		return 0;
	case 0x23:
		dos_terminate(b, 0, 0);
		return 0;
	case 0x24:
		setlo(c, R_EAX, 3);	/* critical error: fail the call */
		return 0;
	case 0x25: case 0x26:		/* absolute disk I/O: no */
		set16(c, R_EAX, 0x0201);
		bios_set_flag(c, FL_CF, 1);
		return 0;
	case 0x27: {
		uint16_t paras = (uint16_t)((DX(c) + 15) / 16);
		x86_load_seg_real(c, S_DS, c->s[S_CS].sel);
		dos_terminate(b, 0, MAX(paras, (uint16_t)PSP_SIZE_PARAS));
		return 0;
	}
	case 0x28:
		b->idle_polls += 16;
		return 0;
	case 0x29:
		bios_tty(b, AL(c));
		return 0;
	case 0x2A: case 0x2E:
		return 0;
	case 0x2F:
		dos_int2f(b);
		return 0;
	}
	return 0;
}

void dos_init(struct dosbox *b)
{
	/* the arena: one free block from FIRST_MCB to the top of conventional memory */
	mcb_set(b, FIRST_MCB, 'Z', 0, (uint16_t)(CONV_TOP_SEG - FIRST_MCB - 1));
	/* the List of Lists: what's useful in it, the first MCB */
	mww(b, sl(DOS_DATA_SEG, LOL_OFF - 2), FIRST_MCB);
	mbw(b, sl(DOS_DATA_SEG, LOL_OFF + 0x20), 26);	/* drives */
	mbw(b, sl(DOS_DATA_SEG, INDOS_OFF), 0);
	/* the standard devices */
	memset(b->sft, 0, sizeof b->sft);
	b->sft[0].dev = DEV_CON;
	strcpy(b->sft[0].name, "CON");
	b->sft[1].dev = DEV_AUX;
	strcpy(b->sft[1].name, "AUX");
	b->sft[2].dev = DEV_PRN;
	strcpy(b->sft[2].name, "PRN");
	/* the drives' directories: where bdos is now, roots elsewhere */
	const char *cwd = task_current()->cwd;
	b->drive = toupper(cwd[0]) - 'A';
	for (int d = 0; d < 26; d++)
		snprintf(b->cwd[d], sizeof b->cwd[d], "%c:\\", 'A' + d);
	strncpy0(b->cwd[b->drive], cwd, sizeof b->cwd[b->drive]);
	b->pending_scan = -1;
	b->alloc_strategy = 0;
	b->break_check = 1;
}
