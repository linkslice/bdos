/*
 * task.c - cooperative tasks.
 *
 * bdos is single-user and cooperative, like the classic Mac or Windows 3.x:
 * a task runs until it waits for input (task_idle) or yields. Each task
 * has its own console and current directory, which is what lets a shell
 * in a desktop window behave exactly like the full-screen one.
 */
#include "kernel.h"

#define STACK_PAGES 64		/* 256 KiB */
#define STACK_CANARY 0x5AFEC0DE5AFEC0DEULL

static struct task boot_task;	/* kmain's own context */
static struct task *current;
static struct task *tasks;	/* circular list */
static int next_id;

void task_init(void)
{
	boot_task.id = next_id++;
	strncpy0(boot_task.name, "kernel", TASK_NAME_MAX);
	boot_task.state = TASK_READY;
	boot_task.con = screen_console();
	strcpy(boot_task.cwd, "C:\\");
	boot_task.next = &boot_task;
	current = tasks = &boot_task;
}

struct task *task_current(void)
{
	return current;
}

static void task_start(void)
{
	current->entry(current->arg);
}

struct task *task_spawn(const char *name, void (*fn)(void *), void *arg,
    struct console *con)
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
	t->id = next_id++;
	strncpy0(t->name, name, TASK_NAME_MAX);
	t->state = TASK_READY;
	t->entry = fn;
	t->arg = arg;
	t->con = con ? con : current->con;
	if (t->con)
		t->con->refs++;
	strncpy0(t->cwd, current->cwd, sizeof t->cwd);
	arch_task_setup(&t->ctx, (uint8_t *)t->stack + STACK_PAGES * PAGE_SIZE, task_start);

	uint64_t f = irq_save();
	t->next = current->next;
	current->next = t;
	irq_restore(f);
	return t;
}

/* Free dead tasks other than the current one. */
static void reap(void)
{
	struct task *prev = current;
	for (struct task *t = current->next; t != current; ) {
		struct task *nx = t->next;
		if (t->state == TASK_DEAD && !t->waiters && t != &boot_task) {
			prev->next = nx;
			pmm_free(t->stack, t->stack_pages);
			kfree(t);
		} else {
			prev = t;
		}
		t = nx;
	}
}

void task_yield(void)
{
	if (current->stack && *(uint64_t *)current->stack != STACK_CANARY)
		panic("stack overflow in task '%s'", current->name);
	uint64_t f = irq_save();
	struct task *from = current, *to = current->next;
	while (to->state == TASK_DEAD && to != from)
		to = to->next;
	if (to->state == TASK_DEAD) {
		irq_restore(f);
		panic("no runnable tasks");
	}
	if (to != from) {
		current = to;
		arch_switch(&from->ctx, &to->ctx);
	}
	irq_restore(f);
	reap();
}

void task_idle(void)
{
	current->idle = 1;
	/* If every live task has polled and found nothing, sleep the CPU. */
	int all_idle = 1;
	for (struct task *t = current->next; t != current; t = t->next)
		if (t->state != TASK_DEAD && !t->idle)
			all_idle = 0;
	if (all_idle) {
		for (struct task *t = current->next; t != current; t = t->next)
			t->idle = 0;
		cpu_idle();	/* until the next interrupt (1 ms at most) */
	}
	current->idle = 0;
	task_yield();
}

_Noreturn void task_exit(void)
{
	irq_disable();
	if (current == &boot_task)
		panic("kernel task exited");
	current->state = TASK_DEAD;
	if (current->con)
		current->con->refs--;
	task_yield();
	panic("dead task scheduled");
}

int task_wait(struct task *t)
{
	t->waiters++;
	while (t->state != TASK_DEAD)
		task_idle();
	t->waiters--;
	return t->exit_code;
}

int task_alive(struct task *t)
{
	return t->state != TASK_DEAD;
}

void task_list(void)
{
	kprintf("\n  ID  Task                     Console     Directory\n");
	kprintf("  --  -----------------------  ----------  ---------\n");
	struct task *t = current;
	do {
		if (t->state != TASK_DEAD)
			kprintf("  %2d  %-23s  %-10s  %s%s\n", t->id, t->name,
			    t->con ? t->con->name : "-", t->cwd,
			    t == current ? "  <- you" : "");
		t = t->next;
	} while (t != current);
	kprintf("\n");
}
