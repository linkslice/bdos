/*
 * tramp.S - where the other cores start.
 *
 * A core woken with INIT/SIPI starts in 16-bit real mode at a page below
 * 1 MiB. This code is copied to that page (TRAMP_BASE), so every address
 * in it is computed relative to that. It goes to 32-bit protected mode,
 * then to long mode on the kernel's own page tables, and calls
 * ap_entry(struct cpu *) on the stack the boot core left in tramp_stack.
 */
#define TRAMP_BASE 0x8000
#define A(x) ((x) - smp_tramp_start + TRAMP_BASE)

	.section .rodata
	.code16
	.globl smp_tramp_start, smp_tramp_end
	.globl tramp_cr3, tramp_stack, tramp_entry, tramp_arg
smp_tramp_start:
	cli
	cld
	xorw	%ax, %ax
	movw	%ax, %ds
	lgdtl	A(tramp_gdtr)
	movl	%cr0, %eax
	orl	$1, %eax			/* PE */
	movl	%eax, %cr0
	ljmpl	$0x08, $A(tramp_pm)

	.code32
tramp_pm:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movl	%cr4, %eax
	orl	$(1 << 5), %eax			/* PAE */
	movl	%eax, %cr4
	movl	A(tramp_cr3), %eax
	movl	%eax, %cr3
	movl	$0xC0000080, %ecx		/* EFER */
	rdmsr
	orl	$((1 << 8) | (1 << 0)), %eax	/* LME, SCE */
	wrmsr
	movl	%cr0, %eax
	orl	$0x80000000, %eax		/* PG */
	movl	%eax, %cr0
	ljmpl	$0x18, $A(tramp_lm)

	.code64
tramp_lm:
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movq	A(tramp_stack), %rsp
	movq	A(tramp_arg), %rdi
	movq	A(tramp_entry), %rax
	andq	$-16, %rsp
	callq	*%rax
1:	cli
	hlt
	jmp	1b

	.balign 8
tramp_gdt:
	.quad	0
	.quad	0x00CF9A000000FFFF		/* 0x08: 32-bit code */
	.quad	0x00CF92000000FFFF		/* 0x10: data */
	.quad	0x00AF9A000000FFFF		/* 0x18: 64-bit code */
tramp_gdtr:
	.word	4 * 8 - 1
	.long	A(tramp_gdt)
	.balign 8
tramp_cr3:	.quad 0
tramp_stack:	.quad 0
tramp_entry:	.quad 0
tramp_arg:	.quad 0
smp_tramp_end:
