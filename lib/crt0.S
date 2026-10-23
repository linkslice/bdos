/* crt0.S - where a native bdos program starts: RDI = argc, RSI = argv */
	.text
	.globl _start
_start:
	xorl	%ebp, %ebp
	andq	$-16, %rsp
	call	__libc_start
1:	hlt
	jmp	1b
