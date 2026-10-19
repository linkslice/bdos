/*
 * entry.S - kernel entry and CPU exception stubs.
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
 * Exception stubs. Each pushes a dummy error code when the CPU doesn't,
 * then the vector number, and jumps to a common handler that calls
 * isr_dispatch(struct trapframe *).
 */
	.text
.macro ISR_NOERR n
	.globl isr\n
isr\n:
	pushq	$0
	pushq	$\n
	jmp	isr_common
.endm

.macro ISR_ERR n
	.globl isr\n
isr\n:
	pushq	$\n
	jmp	isr_common
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

isr_common:
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
	andq	$-16, %rsp
	call	isr_dispatch		/* does not return for exceptions */
1:	cli
	hlt
	jmp	1b

	.section .rodata
	.globl isr_table
isr_table:
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.quad isr\n
	.endr
