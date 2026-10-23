/*
 * user.c - ring 3 for native programs: page tables per program, getting
 * into user mode, and SYSCALL back out.
 *
 * Every address space shares the kernel's first 512 GB (the identity map,
 * supervisor-only); a program's own pages live above that, from USER_BASE.
 * GS always points at the core's struct cpu, even in user mode - the page
 * it's on isn't a user page, so a program can't use it to look in.
 */
#include "../../kernel.h"

#define PTE_P	(1ULL << 0)
#define PTE_W	(1ULL << 1)
#define PTE_U	(1ULL << 2)
#define PTE_PS	(1ULL << 7)
#define ADDR(e)	((e) & 0x000FFFFFFFFFF000ULL)

uint64_t *arch_kernel_pml4(void);
extern void syscall_entry(void);

void *arch_as_new(void)
{
	uint64_t *p = pmm_alloc(1);
	if (!p)
		return NULL;
	p[0] = arch_kernel_pml4()[0];		/* the kernel, shared */
	return p;
}

/* the page table entry for va, making the tables on the way if asked */
static uint64_t *walk(uint64_t *pml4, uint64_t va, int make)
{
	uint64_t *t = pml4;
	for (int level = 3; level > 0; level--) {
		int i = (int)((va >> (12 + 9 * level)) & 511);
		if (!(t[i] & PTE_P)) {
			if (!make)
				return NULL;
			uint64_t *n = pmm_alloc(1);
			if (!n)
				return NULL;
			t[i] = (uint64_t)n | PTE_P | PTE_W | PTE_U;
		}
		if (t[i] & PTE_PS)
			return NULL;
		t = (uint64_t *)ADDR(t[i]);
	}
	return &t[(va >> 12) & 511];
}

int arch_as_map(void *as, uint64_t va, uint64_t pa, int flags)
{
	if (va < USER_BASE || va >= USER_TOP)
		return -8;	/* E_INVAL */
	uint64_t *e = walk(as, va, 1);
	if (!e)
		return -13;	/* E_NOMEM */
	*e = (pa & ~0xFFFULL) | PTE_P | PTE_U | ((flags & VM_WRITE) ? PTE_W : 0);
	__asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
	return 0;
}

uint64_t arch_as_lookup(void *as, uint64_t va)
{
	if (va < USER_BASE || va >= USER_TOP)
		return 0;
	uint64_t *e = walk(as, va, 0);
	if (!e || !(*e & PTE_P))
		return 0;
	return ADDR(*e) | (va & 0xFFF);
}

uint64_t arch_as_unmap(void *as, uint64_t va)
{
	uint64_t *e = walk(as, va, 0);
	if (!e || !(*e & PTE_P))
		return 0;
	uint64_t pa = ADDR(*e);
	*e = 0;
	__asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
	return pa;
}

static void free_level(uint64_t *t, int level)
{
	for (int i = 0; i < 512; i++) {
		if (!(t[i] & PTE_P))
			continue;
		uint64_t *n = (uint64_t *)ADDR(t[i]);
		if (level > 1)
			free_level(n, level - 1);
		pmm_free(n, 1);		/* a table, or at the bottom a data page */
	}
}

void arch_as_free(void *as)
{
	uint64_t *p = as;
	for (int i = 1; i < 256; i++) {
		if (!(p[i] & PTE_P))
			continue;
		uint64_t *n = (uint64_t *)ADDR(p[i]);
		free_level(n, 2);
		pmm_free(n, 1);
	}
	pmm_free(p, 1);
}

void arch_as_switch(void *as)
{
	uint64_t want = as ? (uint64_t)as : (uint64_t)arch_kernel_pml4();
	uint64_t cur;
	__asm__ volatile("mov %%cr3, %0" : "=r"(cur));
	if (cur != want)
		__asm__ volatile("mov %0, %%cr3" : : "r"(want) : "memory");
}

void arch_set_fs_base(uint64_t base)
{
	wrmsr(0xC0000100, base);
}

_Noreturn void arch_enter_user(uint64_t rip, uint64_t rsp, uint64_t a0, uint64_t a1)
{
	/* an IRET frame for ring 3: SS, RSP, RFLAGS (IF on), CS, RIP */
	__asm__ volatile(
	    "cli\n\t"
	    "pushq $0x23\n\t"		/* user data, RPL 3 */
	    "pushq %1\n\t"
	    "pushq $0x202\n\t"
	    "pushq $0x2B\n\t"		/* user 64-bit code, RPL 3 */
	    "pushq %0\n\t"
	    "xorl %%eax, %%eax\n\t"
	    "xorl %%ebx, %%ebx\n\t"
	    "xorl %%ecx, %%ecx\n\t"
	    "xorl %%edx, %%edx\n\t"
	    "xorl %%ebp, %%ebp\n\t"
	    "xorl %%r8d, %%r8d\n\t"
	    "xorl %%r9d, %%r9d\n\t"
	    "xorl %%r10d, %%r10d\n\t"
	    "xorl %%r11d, %%r11d\n\t"
	    "xorl %%r12d, %%r12d\n\t"
	    "xorl %%r13d, %%r13d\n\t"
	    "xorl %%r14d, %%r14d\n\t"
	    "xorl %%r15d, %%r15d\n\t"
	    "mov $0x23, %%ax\n\t"
	    "mov %%ax, %%ds\n\t"
	    "mov %%ax, %%es\n\t"
	    "xorl %%eax, %%eax\n\t"
	    "iretq"
	    : : "r"(rip), "r"(rsp), "D"(a0), "S"(a1) : "memory");
	__builtin_unreachable();
}

void arch_syscall_init(void)
{
	wrmsr(0xC0000080, rdmsr(0xC0000080) | 1);	/* EFER.SCE */
	/* STAR: kernel CS 0x08 (SS 0x10); SYSRET uses 0x18 + 16 = 0x28 for CS, 0x20 for SS */
	wrmsr(0xC0000081, (0x18ULL << 48) | (0x08ULL << 32));
	wrmsr(0xC0000082, (uint64_t)syscall_entry);
	wrmsr(0xC0000084, 0x700);			/* clear TF, IF, DF on entry */
}
