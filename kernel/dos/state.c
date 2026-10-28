/*
 * state.c - saved states: a running DOS program frozen to a file, and
 * thawed again later, even after a reboot.
 *
 * A box is its registers, its memory, its video memory and the hardware
 * around it, so a state file is just those, with long runs of zeros
 * squeezed out. Open files are written down by name and position and
 * reopened on the way back in. Ctrl+Alt+F11 saves, Ctrl+Alt+F12 goes back
 * to the save; RESUME NAME at the prompt starts from one.
 */
#include "dos.h"
#include "../gate.h"

#define STATE_DIR "C:\\BDOS\\STATES"
#define MAGIC "BDOSBOX1"

struct state_hdr {
	char magic[8];
	uint32_t version;
	uint32_t box_size, mem_size, vram_size;
	char prog[VFS_PATH_MAX];
};

void dosbox_state_name(const char *prog, char *out, size_t n)
{
	char name[64];
	strncpy0(name, vfs_basename(prog), sizeof name);
	char *dot = strrchr(name, '.');
	if (dot)
		*dot = 0;
	for (char *p = name; *p; p++)
		*p = (char)toupper(*p);
	snprintf(out, n, "%s\\%s.BST", STATE_DIR, name);
}

/* zero runs: [u32 literal length][literal bytes][u32 zero length] ... */
static int put(struct vfile *f, const void *p, size_t n)
{
	return vfs_write(f, p, n) == (long)n ? 0 : -1;
}

static int pack(struct vfile *f, const uint8_t *p, uint32_t n)
{
	uint32_t i = 0;
	while (i < n) {
		uint32_t lit = i;
		while (lit < n) {
			/* a run of 64+ zeros ends the literal */
			uint32_t z = lit;
			while (z < n && !p[z] && z - lit < 64)
				z++;
			if (z - lit >= 64 || z == n)
				break;
			lit = z + 1;
		}
		uint32_t llen = lit - i;
		uint32_t z = lit;
		while (z < n && !p[z])
			z++;
		uint32_t zlen = z - lit;
		if (put(f, &llen, 4) || (llen && put(f, p + i, llen)) || put(f, &zlen, 4))
			return -1;
		i = z;
	}
	return 0;
}

static int unpack(const uint8_t **src, const uint8_t *end, uint8_t *dst, uint32_t n)
{
	uint32_t i = 0;
	while (i < n) {
		uint32_t llen, zlen;
		if (*src + 4 > end)
			return -1;
		memcpy(&llen, *src, 4);
		*src += 4;
		if (llen > n - i || *src + llen > end)
			return -1;
		memcpy(dst + i, *src, llen);
		*src += llen;
		i += llen;
		if (*src + 4 > end)
			return -1;
		memcpy(&zlen, *src, 4);
		*src += 4;
		if (zlen > n - i)
			return -1;
		memset(dst + i, 0, zlen);
		i += zlen;
	}
	return 0;
}

int dosbox_save_state(struct dosbox *b)
{
	char path[VFS_PATH_MAX];
	vfs_mkdir("C:\\BDOS");
	vfs_mkdir(STATE_DIR);
	dosbox_state_name(b->prog, path, sizeof path);
	int e;
	struct vfile *f = vfs_open(path, O_WRITE | O_CREATE | O_TRUNC, &e);
	if (!f)
		return e;
	/* the FPU goes into the box's image too */
	x87_leave(&b->cpu);
	x87_enter(&b->cpu);
	for (int i = 0; i < MAX_SFT; i++)
		if (b->sft[i].refs && b->sft[i].f)
			b->sft[i].pos = vfs_tell(b->sft[i].f);
	struct state_hdr h;
	memset(&h, 0, sizeof h);
	memcpy(h.magic, MAGIC, 8);
	h.version = 1;
	h.box_size = sizeof *b;
	h.mem_size = b->cpu.mem_size;
	h.vram_size = 4 * VGA_PLANE;
	strncpy0(h.prog, b->prog, sizeof h.prog);
	int r = put(f, &h, sizeof h) || put(f, b, sizeof *b) ||
	    pack(f, b->cpu.mem, b->cpu.mem_size) || pack(f, b->vga.vram, 4 * VGA_PLANE);
	vfs_close(f);
	return r ? E_IO : 0;
}

/* thaw: everything that's a pointer stays as it is in the running box */
int dosbox_load_state(struct dosbox *b, const char *file)
{
	size_t n;
	int e;
	uint8_t *img = (uint8_t *)vfs_slurp(file, &n, &e);
	if (!img)
		return e;
	const uint8_t *p = img, *end = img + n;
	struct state_hdr h;
	if (n < sizeof h) {
		kfree(img);
		return E_CORRUPT;
	}
	memcpy(&h, p, sizeof h);
	p += sizeof h;
	if (memcmp(h.magic, MAGIC, 8) || h.version != 1 || h.box_size != sizeof *b ||
	    h.mem_size > b->cpu.mem_size || h.vram_size != 4 * VGA_PLANE || p + sizeof *b > end) {
		kfree(img);
		return E_CORRUPT;
	}
	struct dosbox *saved = kmalloc(sizeof *b);
	if (!saved) {
		kfree(img);
		return E_NOMEM;
	}
	memcpy(saved, p, sizeof *b);
	p += sizeof *b;
	if (unpack(&p, end, b->cpu.mem, h.mem_size) || unpack(&p, end, b->vga.vram, h.vram_size)) {
		kfree(saved);
		kfree(img);
		return E_CORRUPT;
	}
	kfree(img);
	/* files: close ours, reopen theirs */
	for (int i = 0; i < MAX_SFT; i++)
		if (b->sft[i].f)
			vfs_close(b->sft[i].f);
	for (int i = 0; i < MAX_SEARCH; i++)
		kfree(b->search[i].ents);
	/* keep the live things */
	uint8_t *mem = b->cpu.mem, *vram = b->vga.vram, *thunk = b->cpu.fpu.thunk;
	uint32_t mem_size = b->cpu.mem_size;
	const struct x86_env *env = b->cpu.env;
	struct task *task = b->task;
	struct console *con = b->con;
	struct display *disp = b->disp;
	int disp_failed = b->disp_failed, raw_seen = b->raw_seen;
	int sb_stream = b->sb.stream, opl_stream = b->opl.stream;	/* the mixer's, not the file's */
	memcpy(b, saved, sizeof *b);
	kfree(saved);
	b->cpu.mem = mem;
	b->cpu.mem_size = mem_size;
	b->vga.vram = vram;
	b->cpu.fpu.thunk = thunk;
	b->cpu.fpu.last = 0xFFFF;
	b->cpu.env = env;
	b->cpu.box = b;
	b->task = task;
	b->con = con;
	b->disp = disp;
	b->shadow_of = NULL;
	b->disp_failed = disp_failed;
	b->raw_seen = raw_seen;
	b->sb.stream = sb_stream;
	b->opl.stream = opl_stream;
	b->pal32 = NULL;
	b->errmsg = NULL;
	b->want_quit = 0;
	memset(b->search, 0, sizeof b->search);
	for (int i = 0; i < MAX_SFT; i++) {
		struct sft *s = &b->sft[i];
		s->f = NULL;
		if (s->refs && !s->dev && s->path[0]) {
			s->f = vfs_open(s->path, O_READ | O_WRITE, &e);
			if (!s->f)
				s->f = vfs_open(s->path, O_READ, &e);
			if (s->f)
				vfs_seek(s->f, s->pos);
		}
	}
	b->running = 1;
	b->vga.dirty = 1;
	memset(b->shown, 0xFF, sizeof b->shown);
	b->t0_us = dos_now(b);
	b->ran = 0;
	b->irq0_next_us = 0;
	for (int i = 0; i < 3; i++)
		b->pit[i].start_us = dos_now(b);
	x87_enter(&b->cpu);
	return 0;
}
