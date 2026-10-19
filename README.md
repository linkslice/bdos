# bdos — Bryan Disk Operating System

A 64-bit, single-user OS that should feel like home to anyone who grew up at
a `C:\>` prompt. Boots on UEFI with its own loader, no third-party bootloader.

```
bdos 0.1 (x86-64) - Bryan Disk Operating System
255,848 K memory, 1024x768 display

Welcome to bdos. Type HELP for a list of commands.

C:\>_
```

## Build and run

Needs clang, lld, llvm-objcopy, python3 with Pillow, mtools, QEMU and OVMF
(on Ubuntu/Debian: `clang lld llvm python3-pil mtools qemu-system-x86 ovmf
fonts-unifont`).

```sh
make          # -> build/bdos.img, a bootable 64 MiB disk image
make run      # boot it in QEMU; COM1 is your terminal too
python3 tools/test.py build/bdos.img build/test   # headless smoke test
```

`build/bdos.img` also boots on real hardware from a USB stick (`dd` it over),
as long as the machine can boot UEFI with Secure Boot off and has PS/2 or
legacy-USB keyboard emulation.

## What works (0.1)

- **UEFI loader** (`loader/`): a PE32+ app built from our own `efi.h`. Picks a
  1024×768 GOP mode, loads `\BDOS\KERNEL.SYS` (ELF64) at 2 MiB, finds the ACPI
  RSDP, takes the memory map, exits boot services and jumps in with a
  `struct bdos_bootinfo` (`include/bootinfo.h`).
- **Kernel** (`kernel/`): own GDT/IDT with exception reporting, framebuffer
  console using GNU Unifont 8×16 (128×48 at 1024×768), polled PS/2 keyboard,
  COM1 mirror (output and input), CMOS clock.
- **Shell**: `DIR` `CD` `TYPE` `CLS` `VER` `ECHO` `REM` `MEM` `ATTRIB` `DATE`
  `TIME` `ALIAS` `HELP` `REBOOT` `WIN`, batch files, `AUTOEXEC.BAT`, command
  history (↑/↓), Esc and ^C. DOS habits like `dir/w`, `cd\`, `cd..` and
  `ECHO.` work.
- **Unix aliases** that switch parsing rules by invoked name: `ls` `cat`
  `clear` `pwd` `free` `uname` `man`. As `DIR`, `/` starts a switch and `\`
  separates paths; as `ls`, `-` starts a switch and `/` separates paths.
  Aliases are case-sensitive like Unix; DOS commands aren't.
- **RAM disk C:** a small read-only tree built into the kernel with long,
  case-preserving names (`Read Me First.txt`) and DOS attributes, standing in
  until the real filesystem exists.

## Layout

```
include/bootinfo.h   loader -> kernel hand-off
loader/efi.h         minimal UEFI definitions, from the spec
loader/loader.c      BOOTX64.EFI
kernel/entry.S       _start and exception stubs
kernel/cpu.c         GDT, IDT, reboot
kernel/console.c     framebuffer text console
kernel/kbd.c         PS/2 + serial input
kernel/serial.c      COM1
kernel/mem.c         memory map accounting
kernel/ramdisk.c     built-in C: drive
kernel/shell.c       COMMAND.COM
kernel/lib.c         memcpy, printf and friends
tools/gen_font.py    Unifont -> build/font.h
tools/mkimage.sh     MBR + FAT32 ESP image via mtools
tools/run.sh         QEMU + OVMF
tools/test.py        boots headless, drives COM1 and the PS/2 keyboard
```

## Known limits

- Paging still uses the firmware's identity map; no allocator yet.
- No interrupts: keyboard and serial are polled.
- Keyboard is PS/2 only (USB needs the firmware's legacy emulation).
- `.BAS` and `.C` files print their source; the interpreters come later.

## Roadmap

1. Physical page allocator, own page tables, interrupts and a timer
2. Block device (virtio-blk / AHCI / NVMe) and the bdos filesystem:
   journaled, CRC32C metadata, long names with 8.3 aliases, extended
   attributes, plus `mkfs` and image tools for the host
3. BBC-style BASIC (porting Brandy) and run-from-source C, one shared `SYS` API
4. Per-process device tables: `CON`, `MOUSE$`, `DRAW$`, `DESK$`
5. The desktop: Program Manager layout, nestable, `WIN` to start it
6. Networking, `WIN \\HOST` / `CTTY \\HOST` remote sessions with a `T:` drive
   mapped to the launching directory
7. 8086 emulator for classic `.COM` programs
8. Lemote Yeeloong 8089B (Loongson 2F, MIPS64, PMON) port
