# Porting bdos to the Lemote Yeeloong 8089B (MIPS64)

Status: **boots to `C:\>` on QEMU's Fuloong 2E** (Loongson 2E, the Yeeloong's
older sibling: same Bonito bridge, same ISA window), on the serial port, with
C: as a RAM disk. The shell, batch files, BASIC, the C interpreter, the DOS
emulator (8086 through 486, no FPU) and preemptive tasks all run there.
**Nothing has been run on a real Yeeloong yet.**

```
make mips          # build-mips64/KERNEL.SYS: a MIPS64 ELF for PMON or QEMU
make run-mips      # QEMU fuloong2e, C: from build/ROOTFS.IMG, serial on stdio
make mipstest      # the MIPS test suite
```

What the port is (`kernel/arch/mips64/`):

- `entry.S`: PMON's entry (a0..a2 = argc, argv, envp as 32-bit pointers), the
  exception entry (saves a frame, drops EXL so the C handler is an ordinary
  kernel context that may switch tasks), `arch_switch` (callee-saved
  registers, Status.IE and all 32 FPU registers), setjmp/longjmp.
- `cpu.c`: the boot info from PMON's `memsize=` and QEMU/PMON's
  `rd_start=`/`rd_size=`; the 16550 at ISA 3F8h and the MC146818 at 70h
  through the Bonito I/O window (KSEG1 0xbfd00000); the 1 kHz tick from CP0
  Count/Compare, its rate measured against the RTC at boot; one core.
- Everything runs in KSEG0, so physical memory is at 0xffffffff80000000;
  `P2V`/`V2P` in `arch.h` translate (they're the identity on x86).

Not done yet, in the order the Yeeloong needs them:

1. **Display.** The console wants a 32-bpp framebuffer; the SM712 runs at
   16 bpp, so `console.c` needs a 16-bpp path, and the SM712's address has to
   come from PCI.
2. **PCI.** `arch_pci_read32` is wired to the Bonito's configuration window
   but switched off (`pci_on`): the slot-to-IDSEL mapping differs between
   boards and a wrong guess is a bus error. Check it against Linux's
   `arch/mips/loongson64` on the hardware first. The PCI drivers also hand
   devices virtual addresses for DMA; they need `V2P()` before any of them
   (RTL8139 doesn't exist yet anyway) is used on MIPS.
3. **Keyboard.** `ps2_init` is a stub; the x86 i8042 code works through the
   same ISA window, polled, once someone confirms the EC behaves like one.
4. **Interrupts** from the southbridge's 8259s (Cause IP2 through the Bonito):
   only the CP0 timer is enabled now; drivers poll.
5. **Loongson 2F errata.** clang's assembler has no `-mfix-loongson2f-nop` or
   `-mfix-loongson2f-jump`; a real 2F build should use GNU as for those (see
   below). QEMU doesn't model the errata.

Not coming to MIPS: native 64-bit programs and TCC (they're x86-64 code), and
the DOS box's x87 (on x86 it runs on the real x87; here the box simply has no
coprocessor).

## The machine

| Part | 8089B | What bdos needs |
|---|---|---|
| CPU | Loongson 2F, MIPS64 (MIPS III + extensions), little-endian, ~800 MHz | new arch layer |
| RAM | 1 GiB DDR2 | bitmap PMM works as-is |
| Firmware | PMON2000, no UEFI, no ACPI | new boot path |
| Display | Silicon Motion SM712, 1024×600, 16 bpp | framebuffer setup + 16-bpp `pixel_pack` |
| Southbridge | AMD CS5536: IDE, USB OHCI/EHCI, AC'97, RTC, PIC | IDE driver, RTC, interrupt routing |
| Keyboard / touchpad | via the KB3310 embedded controller, i8042-compatible *(verify)* | the existing PS/2 code, through the ISA I/O window |
| Network | Realtek RTL8139 (10/100) | new NIC driver; BDNET is unchanged |
| Storage | 2.5" PATA disk or SD card reader | IDE (PIO first) instead of AHCI |

## What's already portable

Everything outside `kernel/arch/x86_64/` is plain C with no inline assembly.
Done as part of this milestone:

- `cli; hlt` in the panic path → `arch_halt()`.
- PCI config access (0xCF8/0xCFC) → `arch_pci_read32/write32`; `drv/pci.c`
  only enumerates.
- `fmath.c`'s `sqrtsd` is `#if defined(__x86_64__)` with a portable
  Newton fallback (MIPS has `sqrt.d`; use it when porting).

Already independent of the CPU by design:

- **bdosfs** is little-endian on disk and the 2F runs little-endian, so disks
  move between machines unchanged. `mkbdosfs` stays the host tool.
- **BDNET** builds frames with explicit big-endian helpers; nothing assumes
  host order.
- **The 8086 emulator** is a byte-level interpreter, so `.COM`/`.EXE` programs
  run on MIPS too: that's the backward-compatibility story on the Yeeloong.
- **BASIC and C** are interpreters. The x86 build compiles `kernel/lang/` with
  SSE2 because task switches are cooperative and no ISR touches FP; on MIPS
  the same argument holds for the FPU (set `Status.CU1`, and `FR=1` for 32
  double registers).
- The GUI draws into 32-bit surfaces and only converts at the framebuffer
  edge (`pixel_pack`), so 16-bpp output is one function.

## The arch contract (`kernel/arch/<arch>/arch.h`)

A new architecture supplies:

```
arch_init(bootinfo)            CPU setup, exception vectors, timer at 1 kHz
arch_switch / arch_task_setup  cooperative context switch (callee-saved regs + sp)
arch_setjmp / arch_longjmp     for the interpreters' error exits
arch_reboot / arch_halt
arch_rtc(...)                  wall-clock time
arch_pci_read32 / write32      config space
irq_register(irq, fn), irq_enable/disable
cpu_pause, rdtsc-equivalent counter (CP0 Count), mmio_barrier (sync)
ARCH_NAME                      "mips64"
```

Port I/O helpers (`inb`/`outb`) are only used inside `arch/x86_64` and by
the PS/2 and serial drivers that live there; on MIPS the ISA I/O space is
memory-mapped through the PCI/ISA window, so `arch/mips64/` provides its own
`ps2.c` and `serial.c` (mostly a copy with MMIO accessors).

## Booting without UEFI

`loader/` is UEFI-only and isn't used on the Yeeloong. Instead:

1. Build `KERNEL.SYS` as a MIPS64 ELF (`-mabi=64`, linked in CKSEG0 at
   `0xffffffff80200000` *(verify load address vs PMON's own use of low RAM)*).
2. PMON loads it from disk: `load /dev/fs/ext2@wd0/boot/kernel.sys` then `g`
   *(PMON's FAT support is unreliable; a tiny ext2 boot partition is the safe
   choice)*. PMON passes `argc/argv/envp` in `a0..a2`; `envp` has
   `memsize=` and `highmemsize=` *(verify names)*.
3. `arch/mips64/entry.S` sets up a stack, zeroes BSS and builds the same
   `struct bdos_bootinfo` the UEFI loader does: memory ranges from
   `memsize`/`highmemsize`, `fb` from the SM712's PCI BAR, no `acpi_rsdp`,
   and no `rootfs_base` (the root is a real bdosfs partition on the IDE disk).

## Memory and the TLB

Keep it as simple as the x86 identity map:

- Run the kernel unmapped: CKSEG0 for code, **XKPHYS** for all of physical
  memory (cached `0x9800000000000000 | pa`, uncached `0x9000000000000000 | pa`
  for MMIO and the framebuffer). No TLB entries are needed for any of that.
- `pmm`/`heap` hand out physical pages; a `phys_to_virt()` macro in the arch
  header picks the XKPHYS window. (Today x86 identity-maps, so the macro is
  the identity there.)
- The TLB refill handler only has to exist so a stray user-segment access
  reports an exception instead of looping; bdos has no user space.

## Loongson 2F errata

Build with GCC (clang's Loongson 2F support is thin):

```
mips64el-linux-gnuabi64-gcc -march=loongson2f -mabi=64 -G0 -mno-abicalls \
    -fno-pic -ffreestanding -Wa,-mfix-loongson2f-nop,-mfix-loongson2f-jump
```

- **NOP erratum** - the 2F can hang on certain instruction sequences; the
  assembler's `-mfix-loongson2f-nop` rewrites nops.
- **Jump erratum** - speculative jumps through registers can fetch from I/O
  space; `-mfix-loongson2f-jump` masks jump targets. This matters because
  bdos keeps MMIO mapped in XKPHYS.
- Use `sync` around MMIO (it's what `mmio_barrier()` becomes).

## Drivers to write

In order of when they unblock something:

1. **Serial** (CS5536 UART, if wired out on the 8089B *(verify; may need the
   EC's debug port)*) - the test harness drives COM1, so this comes first.
2. **SM712 framebuffer** - PMON leaves it in 1024×600×16; just find the BAR
   and draw. `console.c` already copes with any width/height; add the
   RGB565 `pixel_pack`. 1024×600 gives a 128×37 text console.
3. **Timer** - CP0 `Count`/`Compare` at 1 kHz feeds `time.c` directly.
4. **Interrupts** - CS5536's PICs cascade into a 2F interrupt line through
   the Bonito *(verify routing)*; once routed, IRQ 1/12 from the EC look just
   like PC keyboard/mouse IRQs.
5. **IDE** (CS5536, PIO mode) - a `blkdev` like `ahci.c` registers; bdosfs
   doesn't care.
6. **RTL8139** - registers a `struct netif`; BDNET, CTTY `\\HOST`, `WIN \\HOST`
   and `NET USE` then work against the x86 machines on the same LAN.

USB, sound and suspend aren't needed for the first boot.

## Suggested first milestone

Serial + framebuffer + timer, booting to `C:\>` on a RAM-only bdosfs image
loaded by PMON next to the kernel (the same `rootfs_base` path UEFI uses).
That exercises the whole portable kernel - shell, BASIC, C, the 8086
emulator - before any disk or network driver exists.
