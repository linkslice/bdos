/*
 * entry.S - kernel entry, interrupt stubs, context switch (x86-64).
 *
 * The loader calls _start with the boot info pointer in %rdi, interrupts
 * disabled, on a 64 KiB stack it allocated.
 */
	.section .text.entry, "ax"
	.globl _start
_start:
	andq	$-16, %rsp		/* System V wants 16-byte alignment */
	call	kmain
1:	cli
	hlt
	jmp	1b

/*
 * Interrupt stubs. Each pushes a dummy error code when the CPU doesn't,
 * then the vector number, and jumps to a common handler that calls
 * trap_dispatch(struct trapframe *) and returns with iretq.
 */
	.text
.macro STUB_NOERR n
	.globl isr\n
isr\n:
	pushq	$0
	pushq	$\n
	jmp	isr_common
.endm

.macro STUB_ERR n
	.globl isr\n
isr\n:
	pushq	$\n
	jmp	isr_common
.endm

STUB_NOERR 0
STUB_NOERR 1
STUB_NOERR 2
STUB_NOERR 3
STUB_NOERR 4
STUB_NOERR 5
STUB_NOERR 6
STUB_NOERR 7
STUB_ERR   8
STUB_NOERR 9
STUB_ERR   10
STUB_ERR   11
STUB_ERR   12
STUB_ERR   13
STUB_ERR   14
STUB_NOERR 15
STUB_NOERR 16
STUB_ERR   17
STUB_NOERR 18
STUB_NOERR 19
STUB_NOERR 20
STUB_ERR   21
STUB_NOERR 22
STUB_NOERR 23
STUB_NOERR 24
STUB_NOERR 25
STUB_NOERR 26
STUB_NOERR 27
STUB_NOERR 28
STUB_ERR   29
STUB_ERR   30
STUB_NOERR 31
/* hardware IRQs 0-15 remapped to vectors 32-47, plus the spurious vector */
.irp n, 32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,240,241,242,255
STUB_NOERR \n
.endr

isr_common:
	cld
	pushq	%rax
	pushq	%rbx
	pushq	%rcx
	pushq	%rdx
	pushq	%rsi
	pushq	%rdi
	pushq	%rbp
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rsp, %rdi
	movq	%rsp, %rbx		/* callee-saved: survives the call */
	andq	$-16, %rsp
	call	trap_dispatch
	movq	%rbx, %rsp
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rbp
	popq	%rdi
	popq	%rsi
	popq	%rdx
	popq	%rcx
	popq	%rbx
	popq	%rax
	addq	$16, %rsp		/* vector + error code */
	iretq

/*
 * void arch_switch(struct arch_context *from, struct arch_context *to)
 * Cooperative switch: push the callee-saved registers, swap stacks, pop.
 */
	.globl arch_switch
arch_switch:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	pushfq
	fxsave	16(%rdi)
	movq	%rsp, (%rdi)
	movq	(%rsi), %rsp
	fxrstor	16(%rsi)
	popfq
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret

/* First return of a new task lands here with the entry point in %r12. */
	.globl arch_task_trampoline
arch_task_trampoline:
	call	*%r12			/* task_start: interrupts on when it's ready */
	call	task_exit		/* never returns */
1:	hlt
	jmp	1b

	.section .rodata
	.globl isr_table
isr_table:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
	.quad isr\n
	.endr
	.globl isr_spurious
isr_spurious:
	.quad isr255
	.globl isr_lapic
isr_lapic:
	.quad isr240, isr241, isr242

/*
 * int arch_setjmp(struct arch_jmpbuf *); void arch_longjmp(struct arch_jmpbuf *, int)
 * For the language interpreters' error handling.
 */
	.text
	.globl arch_setjmp
arch_setjmp:
	movq	%rbx, 0(%rdi)
	movq	%rbp, 8(%rdi)
	movq	%r12, 16(%rdi)
	movq	%r13, 24(%rdi)
	movq	%r14, 32(%rdi)
	movq	%r15, 40(%rdi)
	leaq	8(%rsp), %rdx		/* caller's rsp after return */
	movq	%rdx, 48(%rdi)
	movq	(%rsp), %rdx		/* return address */
	movq	%rdx, 56(%rdi)
	xorl	%eax, %eax
	ret

	.globl arch_longjmp
arch_longjmp:
	movl	%esi, %eax
	testl	%eax, %eax
	jnz	1f
	incl	%eax
1:	movq	0(%rdi), %rbx
	movq	8(%rdi), %rbp
	movq	16(%rdi), %r12
	movq	24(%rdi), %r13
	movq	32(%rdi), %r14
	movq	40(%rdi), %r15
	movq	48(%rdi), %rsp
	jmpq	*56(%rdi)

/*
 * SYSCALL from a native program: RCX = its RIP, R11 = its RFLAGS, RSP still
 * its stack. Switch to the task's kernel stack (kept in the core's struct
 * cpu), save a struct sysframe, and call syscall_dispatch(frame).
 */
	.text
	.globl syscall_entry
syscall_entry:
	movq	%rsp, %gs:16
	movq	%gs:8, %rsp
	pushq	%gs:16			/* rsp */
	pushq	%r11			/* rflags */
	pushq	%rcx			/* rip */
	pushq	%rax
	pushq	%rdi
	pushq	%rsi
	pushq	%rdx
	pushq	%r10
	pushq	%r8
	pushq	%r9
	pushq	%rbx
	pushq	%rbp
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rsp, %rdi
	sti
	call	syscall_dispatch
	cli
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbp
	popq	%rbx
	popq	%r9
	popq	%r8
	popq	%r10
	popq	%rdx
	popq	%rsi
	popq	%rdi
	popq	%rax
	popq	%rcx
	popq	%r11
	popq	%rsp
	sysretq
