/* bdos kernel on MIPS64: in KSEG0, loaded at 2 MiB physical by PMON (or QEMU's -kernel) */
ENTRY(_start)

SECTIONS
{
	. = 0xffffffff80200000;
	__kernel_start = .;

	.text : ALIGN(4096) {
		*(.text.start)
		*(.text .text.*)
	}

	.rodata : ALIGN(4096) {
		*(.rodata .rodata.*)
	}

	.data : ALIGN(4096) {
		*(.data .data.*)
		*(.sdata .sdata.*)
	}

	.bss : ALIGN(4096) {
		__bss_start = .;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(8);
		__bss_end = .;
	}

	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
		*(.MIPS.abiflags)
		*(.reginfo)
		*(.MIPS.options)
		*(.pdr)
	}
}
