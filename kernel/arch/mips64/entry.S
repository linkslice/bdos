/*
 * entry.S - MIPS64: where PMON jumps, the exception entry, task switching
 * and setjmp for the interpreters.
 */
	.set	noreorder
	.set	noat

#define FRAME		320
#define F_HI		(32 * 8)
#define F_LO		(33 * 8)
#define F_EPC		(34 * 8)
#define F_STATUS	(35 * 8)
#define F_CAUSE		(36 * 8)
#define F_BADVA		(37 * 8)

	.section .text.start, "ax"
	.globl	_start
_start:
	/* PMON: a0 = argc, a1 = argv, a2 = envp (32-bit pointers, sign-extended) */
	lui	$t0, %hi(boot_stack_top)
	daddiu	$t0, $t0, %lo(boot_stack_top)
	move	$sp, $t0
	lui	$t0, %hi(__bss_start)
	daddiu	$t0, $t0, %lo(__bss_start)
	lui	$t1, %hi(__bss_end)
	daddiu	$t1, $t1, %lo(__bss_end)
1:	beq	$t0, $t1, 2f
	nop
	sd	$zero, 0($t0)
	b	1b
	daddiu	$t0, $t0, 8
2:	jal	mips_start
	nop
3:	b	3b
	nop

/* copied to the exception vectors: on to exc_entry */
	.globl	exc_stub, exc_stub_end
exc_stub:
	lui	$k0, %hi(exc_entry)
	daddiu	$k0, $k0, %lo(exc_entry)
	jr	$k0
	nop
exc_stub_end:

	.text
exc_entry:
	move	$k1, $sp
	daddiu	$sp, $sp, -FRAME
	sd	$k1, 29*8($sp)
	sd	$1, 1*8($sp)
	sd	$2, 2*8($sp)
	sd	$3, 3*8($sp)
	sd	$4, 4*8($sp)
	sd	$5, 5*8($sp)
	sd	$6, 6*8($sp)
	sd	$7, 7*8($sp)
	sd	$8, 8*8($sp)
	sd	$9, 9*8($sp)
	sd	$10, 10*8($sp)
	sd	$11, 11*8($sp)
	sd	$12, 12*8($sp)
	sd	$13, 13*8($sp)
	sd	$14, 14*8($sp)
	sd	$15, 15*8($sp)
	sd	$16, 16*8($sp)
	sd	$17, 17*8($sp)
	sd	$18, 18*8($sp)
	sd	$19, 19*8($sp)
	sd	$20, 20*8($sp)
	sd	$21, 21*8($sp)
	sd	$22, 22*8($sp)
	sd	$23, 23*8($sp)
	sd	$24, 24*8($sp)
	sd	$25, 25*8($sp)
	sd	$28, 28*8($sp)
	sd	$30, 30*8($sp)
	sd	$31, 31*8($sp)
	mfhi	$k0
	sd	$k0, F_HI($sp)
	mflo	$k0
	sd	$k0, F_LO($sp)
	dmfc0	$k0, $14
	sd	$k0, F_EPC($sp)
	mfc0	$k0, $12
	sd	$k0, F_STATUS($sp)
	mfc0	$k0, $13
	sd	$k0, F_CAUSE($sp)
	dmfc0	$k0, $8
	sd	$k0, F_BADVA($sp)
	/* leave exception level, interrupts still off: a normal kernel context
	 * that may switch tasks */
	mfc0	$k0, $12
	li	$k1, ~0x1F
	and	$k0, $k0, $k1
	mtc0	$k0, $12
	nop
	nop
	nop
	move	$a0, $sp
	jal	mips_trap
	nop
	/* back: interrupts off, as they were when the C code was entered */
	mfc0	$k0, $12
	li	$k1, ~0x1F
	and	$k0, $k0, $k1
	mtc0	$k0, $12
	nop
	nop
	nop
	ld	$k0, F_HI($sp)
	mthi	$k0
	ld	$k0, F_LO($sp)
	mtlo	$k0
	ld	$2, 2*8($sp)
	ld	$3, 3*8($sp)
	ld	$4, 4*8($sp)
	ld	$5, 5*8($sp)
	ld	$6, 6*8($sp)
	ld	$7, 7*8($sp)
	ld	$8, 8*8($sp)
	ld	$9, 9*8($sp)
	ld	$10, 10*8($sp)
	ld	$11, 11*8($sp)
	ld	$12, 12*8($sp)
	ld	$13, 13*8($sp)
	ld	$14, 14*8($sp)
	ld	$15, 15*8($sp)
	ld	$16, 16*8($sp)
	ld	$17, 17*8($sp)
	ld	$18, 18*8($sp)
	ld	$19, 19*8($sp)
	ld	$20, 20*8($sp)
	ld	$21, 21*8($sp)
	ld	$22, 22*8($sp)
	ld	$23, 23*8($sp)
	ld	$24, 24*8($sp)
	ld	$25, 25*8($sp)
	ld	$28, 28*8($sp)
	ld	$30, 30*8($sp)
	ld	$31, 31*8($sp)
	/* the saved Status has EXL set (the exception set it): from here on
	 * nothing can interrupt, and ERET clears it on the way out */
	ld	$k0, F_STATUS($sp)
	mtc0	$k0, $12
	nop
	nop
	nop
	ld	$k1, F_EPC($sp)
	dmtc0	$k1, $14
	ld	$1, 1*8($sp)
	ld	$sp, 29*8($sp)
	nop
	eret
	nop

/*
 * void arch_switch(struct arch_context *from, struct arch_context *to)
 * The callee-saved registers and Status on the stack; the FPU in the context.
 */
	.globl	arch_switch
arch_switch:
	daddiu	$sp, $sp, -112
	sd	$16, 0($sp)
	sd	$17, 8($sp)
	sd	$18, 16($sp)
	sd	$19, 24($sp)
	sd	$20, 32($sp)
	sd	$21, 40($sp)
	sd	$22, 48($sp)
	sd	$23, 56($sp)
	sd	$30, 64($sp)
	sd	$31, 72($sp)
	sd	$28, 80($sp)
	mfc0	$t0, $12
	sd	$t0, 88($sp)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	sdc1	$f\n, 16+\n*8($a0)
	.endr
	cfc1	$t1, $31
	sd	$t1, 16+32*8($a0)
	sd	$sp, 0($a0)
	ld	$sp, 0($a1)
	.irp n, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ldc1	$f\n, 16+\n*8($a1)
	.endr
	ld	$t1, 16+32*8($a1)
	ctc1	$t1, $31
	/* Status: only its interrupt enable belongs to the task */
	ld	$t0, 88($sp)
	andi	$t0, $t0, 1
	mfc0	$t2, $12
	li	$t3, ~1
	and	$t2, $t2, $t3
	or	$t2, $t2, $t0
	mtc0	$t2, $12
	nop
	nop
	nop
	ld	$16, 0($sp)
	ld	$17, 8($sp)
	ld	$18, 16($sp)
	ld	$19, 24($sp)
	ld	$20, 32($sp)
	ld	$21, 40($sp)
	ld	$22, 48($sp)
	ld	$23, 56($sp)
	ld	$30, 64($sp)
	ld	$31, 72($sp)
	ld	$28, 80($sp)
	jr	$31
	daddiu	$sp, $sp, 112

/* A new task's first return lands here with its entry point in s0 */
	.globl	arch_task_trampoline
arch_task_trampoline:
	move	$t9, $s0
	jalr	$t9
	nop
	jal	task_exit
	nop
1:	b	1b
	nop

/* int arch_setjmp(struct arch_jmpbuf *); void arch_longjmp(struct arch_jmpbuf *, int) */
	.globl	arch_setjmp
arch_setjmp:
	sd	$16, 0($a0)
	sd	$17, 8($a0)
	sd	$18, 16($a0)
	sd	$19, 24($a0)
	sd	$20, 32($a0)
	sd	$21, 40($a0)
	sd	$22, 48($a0)
	sd	$23, 56($a0)
	sd	$30, 64($a0)
	sd	$29, 72($a0)
	sd	$31, 80($a0)
	sd	$28, 88($a0)
	sdc1	$f24, 96($a0)
	sdc1	$f25, 104($a0)
	sdc1	$f26, 112($a0)
	sdc1	$f27, 120($a0)
	sdc1	$f28, 128($a0)
	sdc1	$f29, 136($a0)
	sdc1	$f30, 144($a0)
	sdc1	$f31, 152($a0)
	jr	$31
	move	$v0, $zero

	.globl	arch_longjmp
arch_longjmp:
	ld	$16, 0($a0)
	ld	$17, 8($a0)
	ld	$18, 16($a0)
	ld	$19, 24($a0)
	ld	$20, 32($a0)
	ld	$21, 40($a0)
	ld	$22, 48($a0)
	ld	$23, 56($a0)
	ld	$30, 64($a0)
	ld	$29, 72($a0)
	ld	$31, 80($a0)
	ld	$28, 88($a0)
	ldc1	$f24, 96($a0)
	ldc1	$f25, 104($a0)
	ldc1	$f26, 112($a0)
	ldc1	$f27, 120($a0)
	ldc1	$f28, 128($a0)
	ldc1	$f29, 136($a0)
	ldc1	$f30, 144($a0)
	ldc1	$f31, 152($a0)
	move	$v0, $a1
	bnez	$v0, 1f
	nop
	li	$v0, 1
1:	jr	$31
	nop

	/* not .bss: kmain clears that, and this stack is in use when it does */
	.section .data.stack, "aw"
	.balign	16
	.space	16384
	.globl	boot_stack_top
boot_stack_top:
