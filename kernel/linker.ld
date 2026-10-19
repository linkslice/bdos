/* bdos kernel: identity-mapped, loaded at 2 MiB physical by the UEFI loader */
ENTRY(_start)

SECTIONS
{
	. = 0x200000;
	__kernel_start = .;

	.text : ALIGN(4096) {
		*(.text.entry)
		*(.text .text.*)
	}

	.rodata : ALIGN(4096) {
		*(.rodata .rodata.*)
	}

	.data : ALIGN(4096) {
		*(.data .data.*)
	}

	.bss : ALIGN(4096) {
		__bss_start = .;
		*(.bss .bss.*)
		*(COMMON)
		__bss_end = .;
	}

	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
