/*
 * proc.c - native 64-bit programs.
 *
 * A native program is an ELF64 executable that runs in ring 3 in an
 * address space of its own, above USER_BASE. It talks to bdos through the
 * SYSCALL instruction (the numbers are in include/bdos_sys.h, which the
 * program's libc uses too). Its threads are ordinary bdos tasks that share
 * the address space, so they spread over the cores like everything else
 * and are preempted like everything else.
 *
 * The kernel never touches a program's memory directly: copies in and out
 * go page by page through the program's page tables, so a bad pointer is
 * an error code, not a crash.
 */
#include "kernel.h"
#include "vfs.h"
#include "sysapi.h"
#include "settings.h"
#include "../include/bdos_sys.h"
#include "net/net.h"
#include "audio.h"

#define STACK_SIZE	(1u << 20)
#define THREAD_STACK	(1u << 20)
#define STACK_TOP	(USER_TOP & ~0xFFFFFULL)
#define MMAP_BASE	0x0000100000000000ULL
#define MAX_FD		32
#define MAX_THREADS	64

struct thread {
	int used, tid;
	struct task *task;
	volatile int done;
	int code;
	uint64_t entry, arg, arg2, stack;
	struct proc *proc;
};

struct proc {
	void *as;
	struct spinlock lock;
	volatile int alive;		/* threads still running */
	volatile int exiting;
	int exit_code;
	uint64_t brk_start, brk;
	uint64_t mmap_next;
	uint64_t stack_next;
	struct vfile *fd[MAX_FD];
	struct thread th[MAX_THREADS];
	int next_tid;
	char path[VFS_PATH_MAX];
	/* the console: while no thread is reading it, the shell's task watches
	 * for Ctrl+C and keeps other keys here for the program */
	volatile int reading;
	int keys[16], nkeys;
	char line[512];			/* what's left of a line read from fd 0 */
	struct tcp *sock[16];		/* TCP sockets: handle i+1 */
	int audio[4];			/* sound streams + 1 (0: none) */
	size_t line_pos, line_len;
};

static int key_get(struct proc *p, int wait)
{
	if (p->nkeys) {
		int k = p->keys[0];
		memmove(p->keys, p->keys + 1, sizeof p->keys[0] * (size_t)--p->nkeys);
		return k;
	}
	return wait ? con_getkey() : con_pollkey();
}

/* ---- user memory ------------------------------------------------------------------- */

static int copy_in(struct proc *p, void *dst, uint64_t src, size_t n)
{
	uint8_t *d = dst;
	while (n) {
		uint64_t pa = arch_as_lookup(p->as, src);
		if (!pa)
			return E_INVAL;
		size_t chunk = MIN(n, (size_t)(PAGE_SIZE - (src & 0xFFF)));
		memcpy(d, (void *)pa, chunk);
		d += chunk;
		src += chunk;
		n -= chunk;
	}
	return 0;
}

static int copy_out(struct proc *p, uint64_t dst, const void *src, size_t n)
{
	const uint8_t *s = src;
	while (n) {
		uint64_t pa = arch_as_lookup(p->as, dst);
		if (!pa)
			return E_INVAL;
		size_t chunk = MIN(n, (size_t)(PAGE_SIZE - (dst & 0xFFF)));
		memcpy((void *)pa, s, chunk);
		s += chunk;
		dst += chunk;
		n -= chunk;
	}
	return 0;
}

static int copy_str(struct proc *p, char *dst, uint64_t src, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		uint64_t pa = arch_as_lookup(p->as, src + i);
		if (!pa)
			return E_INVAL;
		dst[i] = *(char *)pa;
		if (!dst[i])
			return 0;
	}
	dst[n - 1] = 0;
	return 0;
}

/* fresh zeroed pages for [va, va+len) */
static int map_range(struct proc *p, uint64_t va, uint64_t len, int flags)
{
	uint64_t a = va & ~0xFFFULL, end = (va + len + 0xFFF) & ~0xFFFULL;
	for (; a < end; a += PAGE_SIZE) {
		if (arch_as_lookup(p->as, a))
			continue;
		void *pg = pmm_alloc(1);
		if (!pg)
			return E_NOMEM;
		if (arch_as_map(p->as, a, (uint64_t)pg, flags)) {
			pmm_free(pg, 1);
			return E_NOMEM;
		}
	}
	return 0;
}

/* ---- loading ELF ---------------------------------------------------------------------- */

struct ehdr {
	uint8_t ident[16];
	uint16_t type, machine;
	uint32_t version;
	uint64_t entry, phoff, shoff;
	uint32_t flags;
	uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct phdr {
	uint32_t type, flags;
	uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

int elf_is_native(const void *img, size_t n)
{
	const uint8_t *b = img;
	return n >= 64 && b[0] == 0x7F && b[1] == 'E' && b[2] == 'L' && b[3] == 'F' && b[4] == 2 &&
	    *(const uint16_t *)(b + 18) == 0x3E;
}

static int load_elf(struct proc *p, const uint8_t *img, size_t n, uint64_t *entry)
{
	if (!elf_is_native(img, n))
		return E_INVAL;
	const struct ehdr *h = (const void *)img;
	if (h->type != 2 || h->phoff + (uint64_t)h->phnum * sizeof(struct phdr) > n)
		return E_INVAL;		/* static executables only */
	uint64_t top = 0;
	for (int i = 0; i < h->phnum; i++) {
		const struct phdr *ph = (const void *)(img + h->phoff + (uint64_t)i * h->phentsize);
		if (ph->type != 1)	/* PT_LOAD */
			continue;
		if (ph->vaddr < USER_BASE || ph->vaddr + ph->memsz > MMAP_BASE || ph->offset + ph->filesz > n)
			return E_INVAL;
		int r = map_range(p, ph->vaddr, ph->memsz, VM_WRITE | VM_EXEC);
		if (r)
			return r;
		r = copy_out(p, ph->vaddr, img + ph->offset, ph->filesz);
		if (r)
			return r;
		if (ph->vaddr + ph->memsz > top)
			top = ph->vaddr + ph->memsz;
	}
	if (!top)
		return E_INVAL;
	p->brk_start = p->brk = (top + 0xFFF) & ~0xFFFULL;
	*entry = h->entry;
	return 0;
}

/* argc, argv[], NULL, envp NULL, auxv AT_NULL: the System V start, top down */
static int build_stack(struct proc *p, int argc, char **argv, uint64_t *sp_out, uint64_t *argv_out)
{
	uint64_t top = STACK_TOP;
	int r = map_range(p, top - STACK_SIZE, STACK_SIZE, VM_WRITE);
	if (r)
		return r;
	uint64_t sp = top;
	uint64_t ptrs[64];
	if (argc > 60)
		argc = 60;
	for (int i = argc - 1; i >= 0; i--) {
		size_t l = strlen(argv[i]) + 1;
		sp -= l;
		copy_out(p, sp, argv[i], l);
		ptrs[i] = sp;
	}
	sp &= ~15ULL;
	int words = 1 + argc + 1 + 1 + 2;
	if (words & 1)
		sp -= 8;
	uint64_t frame[70];
	int k = 0;
	frame[k++] = (uint64_t)argc;
	for (int i = 0; i < argc; i++)
		frame[k++] = ptrs[i];
	frame[k++] = 0;		/* argv end */
	frame[k++] = 0;		/* no environment */
	frame[k++] = 0;		/* AT_NULL */
	frame[k++] = 0;
	sp -= (uint64_t)k * 8;
	copy_out(p, sp, frame, (size_t)k * 8);
	*sp_out = sp;
	*argv_out = sp + 8;
	p->stack_next = top - STACK_SIZE - PAGE_SIZE;
	return 0;
}

/* ---- threads ----------------------------------------------------------------------------- */

static void thread_start(void *arg)
{
	struct thread *th = arg;
	struct task *t = task_current();
	struct proc *p = th->proc;
	th->task = t;
	t->proc = p;
	bkl_release_all();		/* user code never holds the lock */
	arch_as_switch(p->as);
	t->fs_base = 0;
	arch_set_fs_base(0);
	arch_enter_user(th->entry, th->stack, th->arg, th->arg2);
}

static struct thread *thread_new(struct proc *p, uint64_t entry, uint64_t a0, uint64_t a1, uint64_t sp)
{
	uint64_t f = spin_lock_irqsave(&p->lock);
	struct thread *th = NULL;
	for (int i = 0; i < MAX_THREADS; i++)
		if (!p->th[i].used) {
			th = &p->th[i];
			memset(th, 0, sizeof *th);
			th->used = 1;
			th->tid = ++p->next_tid;
			break;
		}
	spin_unlock_irqrestore(&p->lock, f);
	if (!th)
		return NULL;
	th->entry = entry;
	th->arg = a0;
	th->arg2 = a1;
	th->stack = sp;
	th->proc = p;
	__atomic_fetch_add(&p->alive, 1, __ATOMIC_SEQ_CST);
	char name[TASK_NAME_MAX];
	snprintf(name, sizeof name, "%s.%d", vfs_basename(p->path), th->tid);
	struct task *t = task_spawn(name, thread_start, th, NULL);
	if (!t) {
		__atomic_fetch_sub(&p->alive, 1, __ATOMIC_SEQ_CST);
		th->used = 0;
		return NULL;
	}
	return th;
}

static struct thread *thread_self(struct proc *p)
{
	struct task *t = task_current();
	for (int i = 0; i < MAX_THREADS; i++)
		if (p->th[i].used && p->th[i].task == t)
			return &p->th[i];
	return NULL;
}

_Noreturn static void thread_exit(struct proc *p, int code)
{
	struct thread *th = thread_self(p);
	if (th) {
		th->code = code;
		__atomic_store_n(&th->done, 1, __ATOMIC_RELEASE);	/* the code first, for a joiner on another core */
	}
	__atomic_fetch_sub(&p->alive, 1, __ATOMIC_SEQ_CST);
	sched_kick();
	arch_as_switch(NULL);
	task_current()->proc = NULL;
	task_exit();
}

/* a native program faulted: say so and end it */
static void user_fault(void *tf_)
{
	struct task *t = task_current();
	struct proc *p = t->proc;
	uint64_t *tf = tf_;
	/* the trapframe: 15 registers, vector, error, rip */
	uint64_t vec = tf[15], rip = tf[17];
	if (!p) {
		panic("user-mode fault with no program");
	}
	static const char *const names[32] = {
		[0] = "Divide error", [6] = "Invalid instruction", [13] = "Protection fault",
		[14] = "Page fault", [16] = "Floating point error", [19] = "SIMD floating point error",
	};
	uint64_t cr2 = 0;
#ifdef __x86_64__
	__asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
#endif
	bkl_enter();
	if (vec == 14)
		kprintf("\n%s: segmentation fault at %p (address %p)\n", vfs_basename(p->path), (void *)rip,
		    (void *)cr2);
	else
		kprintf("\n%s: %s at %p\n", vfs_basename(p->path), vec < 32 && names[vec] ? names[vec] : "exception",
		    (void *)rip);
	bkl_leave();
	p->exit_code = 139;
	p->exiting = 1;
	thread_exit(p, 139);
}

/* every return to user mode: a program that's ending takes its threads with it */
void proc_check(void)
{
	struct task *t = task_current();
	struct proc *p = t ? t->proc : NULL;
	if (p && p->exiting)
		thread_exit(p, p->exit_code);
}

/* ---- system calls --------------------------------------------------------------------- */

static int fd_new(struct proc *p, struct vfile *f)
{
	for (int i = 3; i < MAX_FD; i++)
		if (!p->fd[i]) {
			p->fd[i] = f;
			return i;
		}
	return E_NFILE;
}

static int64_t do_write(struct proc *p, int fd, uint64_t buf, uint64_t n)
{
	char tmp[512];
	uint64_t done = 0;
	while (done < n) {
		size_t c = (size_t)MIN(n - done, sizeof tmp);
		if (copy_in(p, tmp, buf + done, c))
			return done ? (int64_t)done : E_INVAL;
		if (fd == 1 || fd == 2) {
			bkl_enter();
			struct console *con = task_current()->con;
			if (con)
				con->write(con, tmp, c);
			bkl_leave();
		} else {
			if (fd < 0 || fd >= MAX_FD || !p->fd[fd])
				return E_BADF;
			bkl_enter();
			long w = vfs_write(p->fd[fd], tmp, c);
			bkl_leave();
			if (w < 0)
				return w;
			if ((size_t)w < c)
				return (int64_t)(done + (uint64_t)w);
		}
		done += c;
	}
	return (int64_t)done;
}

static int64_t do_read(struct proc *p, int fd, uint64_t buf, uint64_t n)
{
	if (fd == 0) {
		/* the console: a line at a time, newline included; 0 at end of input */
		if (p->line_pos >= p->line_len) {
			bkl_enter();
			p->reading++;
			/* keys typed ahead go back to the console's line editor first */
			int r = read_line(p->line, sizeof p->line - 1);
			p->reading--;
			bkl_leave();
			if (r < 0)
				return 0;
			p->line_len = strlen(p->line);
			p->line[p->line_len++] = '\n';
			p->line_pos = 0;
		}
		size_t l = MIN((size_t)n, p->line_len - p->line_pos);
		if (copy_out(p, buf, p->line + p->line_pos, l))
			return E_INVAL;
		p->line_pos += l;
		return (int64_t)l;
	}
	if (fd < 0 || fd >= MAX_FD || !p->fd[fd])
		return E_BADF;
	char tmp[4096];
	uint64_t done = 0;
	while (done < n) {
		size_t c = (size_t)MIN(n - done, sizeof tmp);
		bkl_enter();
		long r = vfs_read(p->fd[fd], tmp, c);
		bkl_leave();
		if (r < 0)
			return done ? (int64_t)done : r;
		if (r == 0)
			break;
		if (copy_out(p, buf + done, tmp, (size_t)r))
			return E_INVAL;
		done += (uint64_t)r;
		if ((size_t)r < c)
			break;
	}
	return (int64_t)done;
}

static int64_t do_open(struct proc *p, uint64_t upath, int flags)
{
	char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
	if (copy_str(p, path, upath, sizeof path))
		return E_INVAL;
	bkl_enter();
	int64_t r;
	if (resolve_path(path, abs, sizeof abs)) {
		r = E_NOENT;
	} else {
		int vf = 0, e = 0;
		if (flags & BS_O_READ) vf |= O_READ;
		if (flags & BS_O_WRITE) vf |= O_WRITE;
		if (flags & BS_O_CREATE) vf |= O_CREATE;
		if (flags & BS_O_TRUNC) vf |= O_TRUNC;
		if (!vf)
			vf = O_READ;
		struct vfile *f = vfs_open(abs, vf, &e);
		if (!f)
			r = e;
		else {
			if (flags & BS_O_APPEND)
				vfs_seek(f, vfs_size(f));
			r = fd_new(p, f);
			if (r < 0)
				vfs_close(f);
		}
	}
	bkl_leave();
	return r;
}

/* a struct sysval as programs lay it out: the same as the kernel's */
static int64_t do_sys(struct proc *p, uint64_t uname, uint64_t uargs, int argc, uint64_t uret)
{
	char name[64];
	if (copy_str(p, name, uname, sizeof name) || argc < 0 || argc > SYS_MAX_ARGS)
		return E_INVAL;
	struct sysval *args = kzalloc(sizeof(struct sysval) * (SYS_MAX_ARGS + 1));
	if (!args)
		return E_NOMEM;
	if (argc && copy_in(p, args, uargs, sizeof(struct sysval) * (size_t)argc)) {
		kfree(args);
		return E_INVAL;
	}
	for (int i = 0; i < argc; i++)
		args[i].s[sizeof args[i].s - 1] = 0;
	struct sysval *ret = &args[SYS_MAX_ARGS];
	bkl_enter();
	int r = sys_call(name, argc, args, ret);
	bkl_leave();
	if (uret)
		copy_out(p, uret, ret, sizeof *ret);
	kfree(args);
	return r;
}

static int64_t do_list(struct proc *p, uint64_t upath, uint64_t buf, uint64_t n);

void syscall_dispatch(struct sysframe *f)
{
	struct task *t = task_current();
	struct proc *p = t->proc;
	uint64_t a0 = f->rdi, a1 = f->rsi, a2 = f->rdx, a3 = f->r10;
	int64_t r = 0;
	if (!p) {
		f->rax = (uint64_t)(int64_t)E_INVAL;
		return;
	}
	switch (f->rax) {
	case BS_EXIT:
		p->exit_code = (int)a0;
		p->exiting = 1;
		thread_exit(p, (int)a0);
	case BS_WRITE:
		r = do_write(p, (int)a0, a1, a2);
		break;
	case BS_READ:
		r = do_read(p, (int)a0, a1, a2);
		break;
	case BS_OPEN:
		r = do_open(p, a0, (int)a1);
		break;
	case BS_CLOSE:
		if ((int)a0 < 3 || (int)a0 >= MAX_FD || !p->fd[a0]) {
			r = (int)a0 < 3 ? 0 : E_BADF;
			break;
		}
		bkl_enter();
		vfs_close(p->fd[a0]);
		bkl_leave();
		p->fd[a0] = NULL;
		break;
	case BS_SEEK: {
		if ((int)a0 < 3 || (int)a0 >= MAX_FD || !p->fd[a0]) {
			r = E_BADF;
			break;
		}
		struct vfile *vf = p->fd[a0];
		bkl_enter();
		int64_t base = a2 == 0 ? 0 : a2 == 1 ? (int64_t)vfs_tell(vf) : (int64_t)vfs_size(vf);
		int64_t pos = base + (int64_t)a1;
		if (pos < 0)
			r = E_INVAL;
		else
			r = vfs_seek(vf, (uint64_t)pos) ? E_INVAL : pos;
		bkl_leave();
		break;
	}
	case BS_SBRK: {
		uint64_t f2 = spin_lock_irqsave(&p->lock);
		uint64_t old = p->brk;
		int64_t inc = (int64_t)a0;
		if (inc > 0) {
			if (p->brk + (uint64_t)inc >= MMAP_BASE || map_range(p, p->brk, (uint64_t)inc, VM_WRITE)) {
				spin_unlock_irqrestore(&p->lock, f2);
				r = E_NOMEM;
				break;
			}
			p->brk += (uint64_t)inc;
		} else if (inc < 0 && p->brk + (uint64_t)inc >= p->brk_start) {
			p->brk += (uint64_t)inc;	/* the pages stay until the program ends */
		}
		spin_unlock_irqrestore(&p->lock, f2);
		r = (int64_t)old;
		break;
	}
	case BS_MMAP: {
		uint64_t len = (a0 + 0xFFF) & ~0xFFFULL;
		uint64_t f2 = spin_lock_irqsave(&p->lock);
		uint64_t at = p->mmap_next;
		p->mmap_next += len + PAGE_SIZE;
		spin_unlock_irqrestore(&p->lock, f2);
		r = map_range(p, at, len, VM_WRITE | VM_EXEC) ? E_NOMEM : (int64_t)at;
		break;
	}
	case BS_MUNMAP:
		r = 0;		/* freed when the program ends */
		break;
	case BS_GETKEY: {
		bkl_enter();
		p->reading++;
		int k = key_get(p, (int)a0);
		p->reading--;
		bkl_leave();
		if (k == 3) {		/* Ctrl+C */
			p->exit_code = 130;
			p->exiting = 1;
		}
		r = k;
		break;
	}
	case BS_UPTIME_MS: r = (int64_t)uptime_ms(); break;
	case BS_UPTIME_US: r = (int64_t)uptime_us(); break;
	case BS_SLEEP: {
		/* in short steps, so Ctrl+C doesn't wait for a long sleep to end */
		uint64_t until = uptime_ms() + a0;
		while (!p->exiting) {
			uint64_t now = uptime_ms();
			if (now >= until)
				break;
			sleep_ms(MIN(until - now, 50));
		}
		break;
	}
	case BS_YIELD: task_yield(); break;
	case BS_THREAD: {
		/* a1 = arg, a2 = stack size: the stack comes from below the main one */
		uint64_t size = a2 ? (a2 + 0xFFFF) & ~0xFFFFULL : THREAD_STACK;
		uint64_t f2 = spin_lock_irqsave(&p->lock);
		uint64_t top = p->stack_next;
		p->stack_next -= size + PAGE_SIZE;	/* and a guard page */
		spin_unlock_irqrestore(&p->lock, f2);
		if (map_range(p, top - size, size, VM_WRITE)) {
			r = E_NOMEM;
			break;
		}
		uint64_t sp = (top - 16) & ~15ULL;
		uint64_t zero = 0;
		copy_out(p, sp, &zero, 8);	/* a return address of 0: returning from the thread faults */
		struct thread *th = thread_new(p, a0, a1, 0, sp);
		r = th ? th->tid : E_NOMEM;
		break;
	}
	case BS_THREAD_EXIT:
		thread_exit(p, (int)a0);
	case BS_JOIN: {
		struct thread *th = NULL;
		for (int i = 0; i < MAX_THREADS; i++)
			if (p->th[i].used && p->th[i].tid == (int)a0)
				th = &p->th[i];
		if (!th) {
			r = E_INVAL;
			break;
		}
		while (!__atomic_load_n(&th->done, __ATOMIC_ACQUIRE) && !p->exiting)
			task_idle();
		while (th->task && task_alive(th->task) && !p->exiting)
			task_idle();
		r = th->code;
		th->used = 0;
		break;
	}
	case BS_SYS:
		r = do_sys(p, a0, a1, (int)a2, a3);
		break;
	case BS_GETCWD: {
		char cwd[256];
		strncpy0(cwd, t->cwd, sizeof cwd);
		size_t l = strlen(cwd) + 1;
		r = l > a1 ? E_NAMETOOLONG : copy_out(p, a0, cwd, l) ? E_INVAL : 0;
		break;
	}
	case BS_CHDIR: {
		char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
		struct vstat st;
		if (copy_str(p, path, a0, sizeof path)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		if (resolve_path(path, abs, sizeof abs) || vfs_stat(abs, &st) || st.type != VFS_DIR)
			r = E_NOENT;
		else
			vfs_canon(abs, t->cwd, sizeof t->cwd);
		bkl_leave();
		break;
	}
	case BS_STAT: {
		char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
		struct vstat st;
		if (copy_str(p, path, a0, sizeof path)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		r = resolve_path(path, abs, sizeof abs) ? E_NOENT : vfs_stat(abs, &st);
		bkl_leave();
		if (!r) {
			struct bs_stat us = { st.size, st.mtime, st.type == VFS_DIR, st.attr };
			r = copy_out(p, a1, &us, sizeof us) ? E_INVAL : 0;
		}
		break;
	}
	case BS_LIST:
		r = do_list(p, a0, a1, a2);
		break;
	case BS_REMOVE: case BS_MKDIR: case BS_RMDIR: {
		char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
		if (copy_str(p, path, a0, sizeof path)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		if (resolve_path(path, abs, sizeof abs))
			r = E_NOENT;
		else
			r = f->rax == BS_REMOVE ? vfs_delete(abs) : f->rax == BS_MKDIR ? vfs_mkdir(abs) : vfs_rmdir(abs);
		bkl_leave();
		break;
	}
	case BS_RENAME: {
		char a[VFS_PATH_MAX], b[VFS_PATH_MAX], aa[VFS_PATH_MAX], bb[VFS_PATH_MAX];
		if (copy_str(p, a, a0, sizeof a) || copy_str(p, b, a1, sizeof b)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		r = resolve_path(a, aa, sizeof aa) || resolve_path(b, bb, sizeof bb) ? E_NOENT : vfs_rename(aa, bb);
		bkl_leave();
		break;
	}
	case BS_SET_FS:
		t->fs_base = a0;
		arch_set_fs_base(a0);
		break;
	case BS_NCPUS: r = ncpus; break;
	case BS_CONSOLE: {
		bkl_enter();
		switch (a0) {
		case 0: con_clear(); break;
		case 1: con_gotoxy((int)a1, (int)a2); break;
		case 2: con_attr((int)a1, (int)a2); break;
		case 3: con_cursor((int)a1); break;
		case 4: {
			int c2, r2;
			con_size(&c2, &r2);
			r = (int64_t)c2 << 32 | (uint32_t)r2;
			break;
		}
		}
		bkl_leave();
		break;
	}
	case BS_SYSTEM: {
		char line[512];
		if (copy_str(p, line, a0, sizeof line)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		r = shell_exec(line);
		bkl_leave();
		break;
	}
	case BS_WAIT_NE: {
		/* block while *addr == val (the cheap half of a futex) */
		uint64_t deadline = a2 ? uptime_ms() + a2 : 0;
		for (;;) {
			uint32_t v;
			if (copy_in(p, &v, a0, 4)) {
				r = E_INVAL;
				break;
			}
			if (v != (uint32_t)a1 || p->exiting)
				break;
			if (deadline && uptime_ms() >= deadline) {
				r = E_TIMEDOUT;
				break;
			}
			task_idle();
		}
		break;
	}
	case BS_TIME:
		bkl_enter();
		r = (int64_t)rtc_epoch();
		bkl_leave();
		break;
	case BS_RESOLVE: {
		char name[128];
		uint32_t ip = 0;
		if (copy_str(p, name, a0, sizeof name)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		int e = dns_resolve(name, &ip);
		bkl_leave();
		r = e ? e : (int64_t)ip;
		break;
	}
	case BS_TCP_CONNECT: case BS_TCP_LISTEN: case BS_TCP_ACCEPT: {
		int slot = -1;
		for (int i = 0; i < 16; i++)
			if (!p->sock[i]) {
				slot = i;
				break;
			}
		if (slot < 0) {
			r = E_NFILE;
			break;
		}
		int e = 0;
		struct tcp *s = NULL;
		bkl_enter();
		if (f->rax == BS_TCP_CONNECT)
			s = tcp_connect((uint32_t)a0, (uint16_t)a1, (int)a2, &e);
		else if (f->rax == BS_TCP_LISTEN)
			s = tcp_listen((uint16_t)a0, &e);
		else if (a0 >= 1 && a0 <= 16 && p->sock[a0 - 1])
			s = tcp_accept(p->sock[a0 - 1], (int)a1);
		else
			e = E_BADF;
		bkl_leave();
		if (!s) {
			r = e ? e : E_TIMEDOUT;
			break;
		}
		p->sock[slot] = s;
		r = slot + 1;
		break;
	}
	case BS_TCP_SEND: case BS_TCP_RECV: {
		if (a0 < 1 || a0 > 16 || !p->sock[a0 - 1]) {
			r = E_BADF;
			break;
		}
		struct tcp *s = p->sock[a0 - 1];
		char tmp[4096];
		if (f->rax == BS_TCP_SEND) {
			uint64_t done = 0;
			while (done < a2) {
				size_t k = (size_t)MIN(a2 - done, sizeof tmp);
				if (copy_in(p, tmp, a1 + done, k)) {
					r = E_INVAL;
					break;
				}
				bkl_enter();
				long w = tcp_send(s, tmp, k);
				bkl_leave();
				if (w < 0) {
					r = done ? (int64_t)done : w;
					break;
				}
				done += (uint64_t)w;
			}
			if (done == a2)
				r = (int64_t)done;
		} else {
			bkl_enter();
			long got = tcp_recv(s, tmp, (size_t)MIN(a2, sizeof tmp), (int)a3);
			bkl_leave();
			r = got;
			if (got > 0 && copy_out(p, a1, tmp, (size_t)got))
				r = E_INVAL;
		}
		break;
	}
	case BS_TCP_CLOSE:
		if (a0 < 1 || a0 > 16 || !p->sock[a0 - 1]) {
			r = E_BADF;
			break;
		}
		bkl_enter();
		tcp_close(p->sock[a0 - 1]);
		bkl_leave();
		p->sock[a0 - 1] = NULL;
		break;
	case BS_AUDIO_OPEN: {
		int slot = -1;
		for (int i = 0; i < 4; i++)
			if (!p->audio[i]) {
				slot = i;
				break;
			}
		int id = slot < 0 ? -1 : audio_open((uint32_t)a0);
		if (id < 0) {
			r = slot < 0 ? E_NFILE : E_INVAL;
			break;
		}
		p->audio[slot] = id + 1;
		r = slot + 1;
		break;
	}
	case BS_AUDIO_WRITE: case BS_AUDIO_CLOSE: case BS_AUDIO_QUEUED: {
		if (a0 < 1 || a0 > 4 || !p->audio[a0 - 1]) {
			r = E_BADF;
			break;
		}
		int id = p->audio[a0 - 1] - 1;
		if (f->rax == BS_AUDIO_QUEUED) {
			r = audio_queued(id);
			break;
		}
		if (f->rax == BS_AUDIO_CLOSE) {
			/* let it finish, then free it */
			while (audio_queued(id) > 1 && !p->exiting)
				sleep_ms(10);
			audio_close(id);
			p->audio[a0 - 1] = 0;
			break;
		}
		int ch = a3 == 2 ? 2 : 1;
		int16_t buf[2048];
		uint64_t done = 0;
		while (done < a2 && !p->exiting) {
			uint32_t n = (uint32_t)MIN(a2 - done, (uint64_t)(2048 / ch));
			if (copy_in(p, buf, a1 + done * (uint64_t)ch * 2, (size_t)n * (size_t)ch * 2)) {
				r = E_INVAL;
				break;
			}
			uint32_t k = 0;
			while (k < n && !p->exiting) {
				k += audio_write(id, buf + k * (uint32_t)ch, n - k, ch);
				if (k < n)
					sleep_ms(5);
			}
			done += k;
		}
		if (!r)
			r = (int64_t)done;
		break;
	}
	case BS_GETENV: {
		/* settings double as the environment: getenv("dos.speed") */
		char key[64];
		if (copy_str(p, key, a0, sizeof key)) {
			r = E_INVAL;
			break;
		}
		bkl_enter();
		const char *v = setting(key);
		char val[256];
		strncpy0(val, v ? v : "", sizeof val);
		bkl_leave();
		size_t l = strlen(val) + 1;
		r = !v || !*val ? E_NOENT : l > a2 ? E_NAMETOOLONG : copy_out(p, a1, val, l) ? E_INVAL : 0;
		break;
	}
	default:
		r = E_NOSYS;
	}
	f->rax = (uint64_t)r;
	if (p->exiting)
		thread_exit(p, p->exit_code);
}

struct list_ctx {
	struct proc *p;
	uint64_t buf, n, used;
	int over;
};

static int list_cb(void *arg, struct vstat *st)
{
	struct list_ctx *l = arg;
	char name[VFS_NAME_MAX + 2];
	snprintf(name, sizeof name, "%s%s", st->name, st->type == VFS_DIR ? "\\" : "");
	size_t len = strlen(name) + 1;
	if (l->used + len > l->n) {
		l->over = 1;
		return 1;
	}
	copy_out(l->p, l->buf + l->used, name, len);
	l->used += len;
	return 0;
}

/* names, each NUL-terminated (folders end in \), into buf: returns bytes used */
static int64_t do_list(struct proc *p, uint64_t upath, uint64_t buf, uint64_t n)
{
	char path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
	if (copy_str(p, path, upath, sizeof path))
		return E_INVAL;
	struct list_ctx l = { p, buf, n, 0, 0 };
	bkl_enter();
	int r = resolve_path(path, abs, sizeof abs) ? E_NOENT : vfs_list(abs, list_cb, &l);
	bkl_leave();
	if (r && !l.over)
		return r;
	return l.over ? E_NAMETOOLONG : (int64_t)l.used;
}

/* ---- running a program ----------------------------------------------------------------- */

/* Load and run a native program; returns its exit code when every thread is done. */
int proc_run(const char *path, int argc, char **argv)
{
	size_t n;
	int e;
	uint8_t *img = (uint8_t *)vfs_slurp(path, &n, &e);
	if (!img) {
		kprintf("%s - %s\n", vfs_strerror(e), path);
		return 1;
	}
	struct proc *p = kzalloc(sizeof *p);
	if (p)
		p->as = arch_as_new();
	if (!p || !p->as) {
		kfree(p);
		kfree(img);
		kprintf("Not enough memory\n");
		return 1;
	}
	strncpy0(p->path, path, sizeof p->path);
	p->mmap_next = MMAP_BASE;
	uint64_t entry, sp, uargv;
	char *args0[1] = { (char *)path };
	if (!argc) {
		argc = 1;
		argv = args0;
	}
	e = load_elf(p, img, n, &entry);
	kfree(img);
	if (!e)
		e = build_stack(p, argc, argv, &sp, &uargv);
	if (e) {
		kprintf("%s: %s\n", vfs_basename(path), e == E_INVAL ? "not a bdos program" : vfs_strerror(e));
		arch_as_free(p->as);
		kfree(p);
		return 1;
	}
	/* the main thread starts at _start with the stack as the ABI says,
	 * and argc/argv in RDI/RSI as well for convenience */
	struct thread *th = thread_new(p, entry, (uint64_t)argc, uargv, sp);
	if (!th) {
		arch_as_free(p->as);
		kfree(p);
		kprintf("Not enough memory\n");
		return 1;
	}
	while (p->alive > 0) {
		/* Ctrl+C stops it; other keys wait for the program to read them */
		if (!p->reading && !p->exiting && p->nkeys < (int)NELEM(p->keys)) {
			int k = con_pollkey();
			if (k == 3) {
				kprintf("^C\n");
				p->exit_code = 130;
				p->exiting = 1;
				/* threads asleep in the kernel notice on their way out */
			} else if (k >= 0) {
				p->keys[p->nkeys++] = k;
			}
		}
		task_idle();
	}
	/* every thread is off every core: the address space can go */
	for (int i = 0; i < MAX_THREADS; i++)
		while (p->th[i].used && p->th[i].task && task_alive(p->th[i].task))
			task_idle();
	for (int i = 0; i < 4; i++)
		if (p->audio[i])
			audio_close(p->audio[i] - 1);
	for (int i = 0; i < 16; i++)
		if (p->sock[i])
			tcp_close(p->sock[i]);
	for (int i = 3; i < MAX_FD; i++)
		if (p->fd[i])
			vfs_close(p->fd[i]);
	arch_as_switch(NULL);
	arch_as_free(p->as);
	int code = p->exit_code;
	kfree(p);
	return code;
}

void proc_init(void)
{
	extern void (*user_trap)(void *tf);
	user_trap = user_fault;
	arch_syscall_init();
}
