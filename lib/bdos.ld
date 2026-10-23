/* bdos.ld - native bdos programs: a static ELF at the bottom of user space */
ENTRY(_start)
SECTIONS
{
	. = 0x8000000000 + SIZEOF_HEADERS;
	.text : { *(.text.start) *(.text .text.*) }
	.rodata : { *(.rodata .rodata.*) }
	.eh_frame : { *(.eh_frame) }
	. = ALIGN(4096);
	.data : { *(.data .data.*) *(.got .got.*) }
	.bss : { *(.bss .bss.*) *(COMMON) }
	/DISCARD/ : { *(.comment) *(.note*) *(.interp) *(.dynamic) *(.dynsym) *(.dynstr) *(.hash) *(.gnu.hash) }
}
