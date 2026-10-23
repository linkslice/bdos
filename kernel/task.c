/*
 * task.c - tasks and the scheduler, on every core.
 *
 * bdos grew up cooperative, like the classic Mac or Windows 3.x: a task
 * runs until it waits for input (task_idle) or yields. That's still true
 * of kernel code, which all runs holding the big kernel lock (BKL), so it
 * never sees another core in the middle of its data.
 *
 * Code that just computes on its own memory - the BASIC and C interpreters,
 * the DOS emulator, native programs - drops the BKL. Those tasks run on any
 * core at the same time as everything else, and since they might not yield
 * for a while, the timer preempts them: after a few milliseconds, if
 * something else wants the core, they're switched out.
 *
 * Each task has its own console and current directory, which is what lets
 * a shell in a desktop window behave exactly like the full-screen one.
 */
#include "kernel.h"

#define STACK_PAGES 64		/* 256 KiB */
#define STACK_CANARY 0x5AFEC0DE5AFEC0DEULL
#define SLICE_MS 10		/* how long a running-free task gets before preemption */

struct cpu cpus[MAX_CPUS];
int ncpus = 1;

static struct task boot_task;	/* kmain's own context */
static struct task *tasks;	/* circular list of everything but the idle tasks */
static struct task *cursor;	/* round robin position */
static int next_id;
static struct spinlock sched_lock = SPINLOCK_INIT;
static struct spinlock bkl = SPINLOCK_INIT;
static volatile int bkl_cpu = -1;	/* which core holds it, for checking */
static volatile uint64_t epoch = 1;	/* moves on every tick and device interrupt */
static uint8_t clean_fx[sizeof(((struct arch_context *)0)->fx)] __attribute__((aligned(16)));

struct task *task_current(void)
{
	return this_cpu()->current;
}

/* ---- the big kernel lock ------------------------------------------------ */

static void bkl_take(void)
{
	spin_lock(&bkl);
	bkl_cpu = this_cpu()->id;
}

static void bkl_drop(void)
{
	bkl_cpu = -1;
	spin_unlock(&bkl);
}

int bkl_held(void)
{
	struct task *t = task_current();
	return t && t->bkl > 0;
}

void bkl_enter(void)
{
	uint64_t f = irq_save();
	struct task *t = task_current();
	if (t->bkl++ == 0)
		bkl_take();
	irq_restore(f);
}

void bkl_leave(void)
{
	uint64_t f = irq_save();
	struct task *t = task_current();
	if (t->bkl <= 0)
		panic("bkl_leave without bkl_enter in '%s'", t->name);
	if (--t->bkl == 0)
		bkl_drop();
	irq_restore(f);
}

int bkl_release_all(void)
{
	uint64_t f = irq_save();
	struct task *t = task_current();
	int d = t->bkl;
	if (d) {
		t->bkl = 0;
		bkl_drop();
	}
	irq_restore(f);
	return d;
}

void bkl_reacquire(int depth)
{
	if (depth <= 0)
		return;
	uint64_t f = irq_save();
	struct task *t = task_current();
	if (t->bkl == 0)
		bkl_take();
	t->bkl += depth;
	irq_restore(f);
}

/* ---- the scheduler ------------------------------------------------------ */

void sched_kick(void)
{
	__atomic_fetch_add(&epoch, 1, __ATOMIC_RELAXED);
}

static void reap(void);

static void idle_loop(void *arg)
{
	(void)arg;
	for (unsigned n = 0;; n++) {
		cpu_idle();	/* sti; hlt: until the next tick or interrupt */
		if (!(n & 63) && this_cpu()->id == 0)
			reap();
		task_yield();
	}
}

/* runs in the next task, after arch_switch: the previous one is now off this core */
static void sched_finish(void)
{
	struct cpu *c = this_cpu();
	if (c->prev) {
		c->prev->cpu = -1;
		c->prev = NULL;
	}
	spin_unlock(&sched_lock);
}

static int wants_to_run(struct task *t)
{
	return t->state == TASK_READY && t->poll_epoch != epoch;
}

/*
 * Pick what this core runs next and switch to it. Called with interrupts
 * off. A task that needs the BKL is only picked if the lock can be had
 * (or is being handed over from the task leaving), so no core ever spins
 * in here waiting for it.
 */
static void schedule(void)
{
	struct cpu *c = this_cpu();
	struct task *from = c->current, *to = NULL;
	if (from->stack && *(uint64_t *)from->stack != STACK_CANARY)
		panic("stack overflow in task '%s'", from->name);
	spin_lock(&sched_lock);
	int took_bkl = 0;
	if (!c->parked) {
		struct task *start = from != c->idle && from->state == TASK_READY ? from : cursor;
		struct task *t = start;
		do {
			t = t->next;
			if (t == from) {
				if (wants_to_run(t)) {
					to = t;
					break;
				}
				continue;
			}
			if (t->cpu >= 0 || !wants_to_run(t))
				continue;
			if (t->bkl > 0 && from->bkl == 0) {
				if (!spin_trylock(&bkl))
					continue;
				bkl_cpu = c->id;
				took_bkl = 1;
			}
			to = t;
			break;
		} while (t != start);
	}
	if (!to)
		to = from != c->idle && wants_to_run(from) && !c->parked ? from : c->idle;
	if (to == from) {
		if (took_bkl)
			bkl_drop();
		spin_unlock(&sched_lock);
		return;
	}
	/* the lock goes with whoever needs it */
	if (from->bkl > 0 && to->bkl == 0)
		bkl_drop();
	if (to != c->idle)
		cursor = to;
	to->cpu = c->id;
	to->last_cpu = c->id;
	c->current = to;
	c->prev = from;
	c->slice = 0;
	if (to->stack)
		arch_set_kstack((uint8_t *)to->stack + to->stack_pages * PAGE_SIZE);
	/* a native program's thread: its address space and thread-local storage
	 * (struct proc starts with the address space) */
	if (to->proc || from->proc)
		arch_as_switch(to->proc ? *(void **)to->proc : NULL);
	if (to->proc)
		arch_set_fs_base(to->fs_base);
	arch_switch(&from->ctx, &to->ctx);
	sched_finish();		/* back in 'from', maybe on another core */
}

void task_yield(void)
{
	uint64_t f = irq_save();
	task_current()->poll_epoch = 0;
	schedule();
	irq_restore(f);
}

void task_idle(void)
{
	uint64_t f = irq_save();
	/* Nothing to do until the world moves on: the next tick (1 ms) or
	 * a device interrupt. If every task is like this, cores halt. */
	task_current()->poll_epoch = epoch;
	schedule();
	irq_restore(f);
}

/* per-core timer: accounting for Pulse, and preemption of running-free tasks */
void sched_tick(void)
{
	struct cpu *c = this_cpu();
	if (c->id == 0) {
		sched_kick();
		static unsigned n;
		if (++n % 250 == 0)
			cpu_sample();
	}
	struct task *t = c->current;
	if (!t || t == c->idle) {
		c->idle_ticks++;
		return;
	}
	c->busy_ticks++;
	t->ticks++;
	if (++c->slice >= SLICE_MS || c->parked)
		c->need_resched = 1;
}

/* the very end of an interrupt, after EOI */
void sched_preempt_check(void)
{
	struct cpu *c = this_cpu();
	if (!c->need_resched)
		return;
	c->need_resched = 0;
	struct task *t = c->current;
	if (t && t != c->idle && t->bkl == 0) {
		t->poll_epoch = 0;
		schedule();
	}
}

static void task_start(void)
{
	sched_finish();
	irq_enable();
	struct task *t = task_current();
	t->entry(t->arg);
}

static struct task *task_new(const char *name, void (*fn)(void *), void *arg)
{
	struct task *t = kzalloc(sizeof *t);
	if (!t)
		return NULL;
	t->stack = pmm_alloc(STACK_PAGES);
	if (!t->stack) {
		kfree(t);
		return NULL;
	}
	t->stack_pages = STACK_PAGES;
	*(uint64_t *)t->stack = STACK_CANARY;	/* lowest word: overflow detector */
	t->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
	strncpy0(t->name, name, TASK_NAME_MAX);
	t->state = TASK_READY;
	t->cpu = -1;
	t->entry = fn;
	t->arg = arg;
	memcpy(t->ctx.fx, clean_fx, sizeof clean_fx);
	arch_task_setup(&t->ctx, (uint8_t *)t->stack + STACK_PAGES * PAGE_SIZE, task_start);
	return t;
}

void task_init(void)
{
	struct cpu *c = &cpus[0];
	arch_fpu_clean(clean_fx);
	boot_task.id = next_id++;
	strncpy0(boot_task.name, "kernel", TASK_NAME_MAX);
	boot_task.state = TASK_READY;
	boot_task.con = screen_console();
	boot_task.cpu = 0;
	boot_task.bkl = 1;
	strcpy(boot_task.cwd, "C:\\");
	boot_task.next = &boot_task;
	tasks = cursor = &boot_task;
	bkl_take();
	c->current = &boot_task;
	c->online = 1;
	c->idle = task_new("idle", idle_loop, NULL);
	if (!c->idle)
		panic("no memory for the idle task");
}

/* An extra core, set up by the arch layer, starts scheduling here. */
void sched_start_cpu(void)
{
	struct cpu *c = this_cpu();
	struct task *idle = task_new("idle", idle_loop, NULL);
	if (!idle)
		panic("no memory for core %d's idle task", c->id);
	idle->cpu = c->id;
	c->idle = idle;
	c->current = idle;
	c->online = 1;
	/* become the idle task: its context starts in task_start, which
	 * expects the scheduler lock held */
	struct arch_context dummy;
	spin_lock(&sched_lock);
	c->prev = NULL;
	arch_switch(&dummy, &idle->ctx);
	panic("core %d left its idle task", c->id);
}

struct task *task_spawn(const char *name, void (*fn)(void *), void *arg,
    struct console *con)
{
	struct task *cur = task_current();
	struct task *t = task_new(name, fn, arg);
	if (!t)
		return NULL;
	t->bkl = 1;		/* kernel code, until it says otherwise */
	t->con = con ? con : cur->con;
	if (t->con)
		t->con->refs++;
	strncpy0(t->cwd, cur->cwd, sizeof t->cwd);
	t->desk = cur->desk;
	t->tdrive = cur->tdrive;

	uint64_t f = spin_lock_irqsave(&sched_lock);
	t->next = cur->next;
	cur->next = t;
	spin_unlock_irqrestore(&sched_lock, f);
	return t;
}

/* Free dead tasks: nobody waiting, not on any core. */
static void reap(void)
{
	struct task *dead = NULL;
	uint64_t f = spin_lock_irqsave(&sched_lock);
	struct task *prev = &boot_task;
	for (struct task *t = boot_task.next; t != &boot_task; ) {
		struct task *nx = t->next;
		if (t->state == TASK_DEAD && !t->waiters && t->cpu < 0) {
			prev->next = nx;
			if (cursor == t)
				cursor = prev;
			t->next = dead;
			dead = t;
		} else {
			prev = t;
		}
		t = nx;
	}
	spin_unlock_irqrestore(&sched_lock, f);
	while (dead) {
		struct task *nx = dead->next;
		pmm_free(dead->stack, dead->stack_pages);
		kfree(dead);
		dead = nx;
	}
}

_Noreturn void task_exit(void)
{
	irq_disable();
	struct task *t = task_current();
	if (t == &boot_task)
		panic("kernel task exited");
	if (t->con)
		t->con->refs--;
	if (t->bkl) {
		t->bkl = 0;
		bkl_drop();
	}
	t->state = TASK_DEAD;
	sched_kick();
	schedule();
	panic("dead task scheduled");
}

int task_wait(struct task *t)
{
	__atomic_fetch_add(&t->waiters, 1, __ATOMIC_RELAXED);
	while (t->state != TASK_DEAD || t->cpu >= 0)
		task_idle();
	int code = t->exit_code;
	__atomic_fetch_sub(&t->waiters, 1, __ATOMIC_RELAXED);
	reap();
	return code;
}

int task_alive(struct task *t)
{
	return t->state != TASK_DEAD;
}

int cpu_park(int id, int parked)
{
	if (id < 0 || id >= ncpus || !cpus[id].online)
		return -1;
	if (parked) {
		int left = 0;
		for (int i = 0; i < ncpus; i++)
			left += cpus[i].online && !cpus[i].parked && i != id;
		if (!left)
			return -1;	/* someone has to do the work */
	}
	cpus[id].parked = parked;
	return 0;
}

void cpu_sample(void)
{
	for (int i = 0; i < ncpus; i++) {
		struct cpu *c = &cpus[i];
		uint64_t b = c->busy_ticks, d = c->idle_ticks;
		uint64_t db = b - c->last_busy, di = d - c->last_idle;
		c->last_busy = b;
		c->last_idle = d;
		c->load = db + di ? (int)(db * 100 / (db + di)) : 0;
	}
}

int task_snapshot(struct task_info *out, int max)
{
	uint64_t f = spin_lock_irqsave(&sched_lock);
	struct task *t = &boot_task;
	int n = 0;
	do {
		if (t->state != TASK_DEAD && n < max) {
			out[n].id = t->id;
			out[n].cpu = t->cpu >= 0 ? t->cpu : -1;
			out[n].ms = t->ticks;
			strncpy0(out[n].name, t->name, sizeof out[n].name);
			strncpy0(out[n].con, t->con ? t->con->name : "-", sizeof out[n].con);
			strncpy0(out[n].cwd, t->cwd, sizeof out[n].cwd);
			n++;
		}
		t = t->next;
	} while (t != &boot_task);
	spin_unlock_irqrestore(&sched_lock, f);
	return n;
}

void task_list(void)
{
	kprintf("\n  ID  Task                     Core  CPU ms   Console     Directory\n");
	kprintf("  --  -----------------------  ----  -------  ----------  ---------\n");
	struct task *me = task_current();
	uint64_t f = spin_lock_irqsave(&sched_lock);
	struct task *t = &boot_task;
	struct { int id, cpu; uint64_t ms; char name[TASK_NAME_MAX], con[12], cwd[40]; int me; } rows[64];
	int n = 0;
	do {
		if (t->state != TASK_DEAD && n < 64) {
			rows[n].id = t->id;
			rows[n].cpu = t->cpu >= 0 ? t->cpu : -1;
			rows[n].ms = t->ticks;
			strncpy0(rows[n].name, t->name, sizeof rows[n].name);
			strncpy0(rows[n].con, t->con ? t->con->name : "-", sizeof rows[n].con);
			strncpy0(rows[n].cwd, t->cwd, sizeof rows[n].cwd);
			rows[n].me = t == me;
			n++;
		}
		t = t->next;
	} while (t != &boot_task);
	spin_unlock_irqrestore(&sched_lock, f);
	for (int i = 0; i < n; i++) {
		char core[8] = "-";
		if (rows[i].cpu >= 0)
			snprintf(core, sizeof core, "%d", rows[i].cpu);
		kprintf("  %2d  %-23s  %4s  %7lu  %-10s  %s%s\n", rows[i].id, rows[i].name, core, rows[i].ms,
		    rows[i].con, rows[i].cwd, rows[i].me ? "  <- you" : "");
	}
	kprintf("\n");
}
