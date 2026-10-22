# bdos: Bryan Disk Operating System

A 64-bit, single-user OS that should feel like home to anyone who grew up at
a `C:\>` prompt. It boots on UEFI with its own loader and drops you at a DOS
prompt. `WIN` starts a Program Manager desktop that works like Plan 9:
desktops nest inside windows, and `WIN \\HOST` puts another machine's desktop
in a window, with your own folder mounted on its `T:` drive.

```
bdos 0.9 (x86-64) - Bryan Disk Operating System
255,848 K memory, 1024x768 display, mouse

Network: e1000 52:54:00:12:34:56, this machine is \\BDOS-123456
Welcome to bdos. Type HELP for a list of commands.

C:\>_
```

## Build and run

You need clang, lld, llvm-objcopy, a host cc, python3 with Pillow, mtools,
nasm, QEMU and OVMF. On Debian or Ubuntu, install `clang lld llvm python3-pil
mtools nasm qemu-system-x86 ovmf fonts-unifont`.

```sh
make            # build/bdos.img: a 192 MiB disk, with an ESP plus a bdosfs partition
make run        # boot it in QEMU; COM1 is a terminal too
make test       # 16 headless suites: shell, files, BASIC, C, DOS, desktop...
make nettest    # two VMs on one virtual LAN: NET VIEW, CTTY/WIN \\BOX2, NET USE
make btermtest  # bterm (../bterm) against a VM: DHCP, a session, the desktop, T:
```

The image also boots real UEFI PCs from a USB stick if you `dd` it across.
Secure Boot must be off, and you need a PS/2 keyboard or the firmware's
legacy USB keyboard emulation.

## What's in it

**Boot.** `loader/` is our own PE32+ UEFI application. It sets a
1024×768 GOP mode, loads `\BDOS\KERNEL.SYS` (ELF64) at 2 MiB, collects
the memory map and RSDP, and exits boot services. It passes a
`struct bdos_bootinfo` to the kernel.

**Kernel.**
- Its own page tables, with the framebuffer mapped write-combining.
- The 8259 PIC and a 1 kHz PIT.
- Interrupt-driven PS/2 keyboard and mouse, plus COM1.
- A bitmap page allocator and a slab heap.
- Cooperative tasks, each with its own console, current directory and desktop.

**bdosfs**, our own filesystem:
- 4 KiB blocks and two CRC32C superblocks.
- A metadata journal in ordered mode.
- Extents, long case-preserving names with 8.3 aliases, extended attributes,
  and parent pointers.
- `CHKDSK` checks a volume, and `tools/mkbdosfs` builds and inspects images
  on the host.

**The shell.** `COMMAND.COM` provides about 60 commands, including:
- Files and directories: `DIR`, `COPY`, `XCOPY`, `DELTREE`, `TREE`, `ATTRIB`.
- Batch files with `GOTO`, `IF`, `FOR`, `CALL` and `SHIFT`, plus `%VARS%`,
  redirection and pipes.
- `MORE`, `SORT` and `FIND`, tab completion and history.
- Unix aliases (`ls`, `cat`, `cp`, `rm`…) that also switch to Unix argument
  parsing.

`EDIT` is a full-screen editor; F5 runs the file you're editing.

**BASIC.** BBC BASIC style:
- Real numbers, `PROC`/`FN` with `LOCAL`, `REPEAT`/`UNTIL` and block `IF`.
- Immediate mode with `RUN`, `LIST`, `SAVE` and `RENUMBER`.
- `SYS "Name", args TO result` calls.

**C, run from source** (`kernel/lang/cint.c`). Type `HELLO.C` and it runs.
- Supported language: structs, pointers, multi-dimensional arrays, enums,
  typedefs, recursion and `#define` macros with arguments.
- Library: printf/scanf, string.h, malloc/free, math.h, and Turbo C's
  conio.h.
- Programs run in their own memory image, so a bad pointer is reported as
  `FILE.C:7: null pointer used` and nothing crashes.
- Any function a program calls but doesn't define becomes a SYS call. For
  example, `desk_color(0x204060)` does the same as BASIC's
  `SYS "Desk_Color", &204060`. `HELP SYS` lists them all.
- See `C:\Docs\C Programs.txt` on the disk.

**The desktop.** It looks like Windows 3.1 and works like Plan 9:
- Program Manager groups are folders under `C:\PROGMAN`, and its items are
  `.lnk` files.
- Each terminal window is a full shell.
- Programs talk to the desktop through devices: `DESK$`, `DRAW$` and
  `MOUSE$`. For example, `echo color #3a3a6e > DESK$`.
- `WIN` typed inside a window nests a whole desktop in that window, and each
  level gets its own colour.
- Windows resize from any edge or corner, and shells reflow to the new size.
  Double-click a title bar, or use the button beside the close box, to
  maximize.
- The Control Panel has applets for Date/Time (with a calendar), Network
  (rename the machine, browse the LAN, open a remote shell or desktop),
  Desktop (colour, start at boot), Colours (window schemes), Mouse,
  Keyboard and System. There's a Clock too.

**Settings** live in `C:\BDOS\SETTINGS.INI` and change the same way from
everywhere: the Control Panel, `CONTROL key=value` at the prompt, or the
`OS_SetSetting` SYS call from BASIC and C. `DATE` and `TIME` set the clock.

**DOS programs.** An 8086 emulator runs `.COM` and MZ `.EXE` programs.
- It handles INT 21h file and console calls, plus INT 10h, 16h and 1Ah.
- Writes to B800 text memory are mirrored to the screen.
- `C:\DOSAPPS` has samples assembled from `dos/*.asm`.

**Networking (BDNET).** A small LAN protocol carried directly on Ethernet,
in the spirit of NetBEUI, with an e1000 driver.
- `NET VIEW` finds the other machines on the network.
- `CTTY \\BOX2` opens a shell on another machine.
- `WIN \\BOX2` shows its desktop, either full screen or in a window
  (F12 hangs up).
- `NET USE E: \\BOX2\C` maps a drive. In every remote session, `T:` is the
  directory you launched the session from, the same idea as drawterm's
  `/mnt/term`.

**TCP/IP, and bterm.** bdos also has a small IP stack: ARP, IPv4, ICMP, UDP
and a DHCP client.
- `IPCONFIG` shows the address, and `PING` checks a machine answers.
- `CONTROL net.ip=...` sets a fixed address instead of DHCP.
- BDNET also runs over UDP port 17979, so sessions can cross routers.
- **[bterm](https://github.com/linkslice/bterm)** is a drawterm for bdos.
  From Linux or macOS, `bterm host` opens the machine's desktop in a window,
  and `bterm -t host` opens a shell in your terminal. Either way, the
  directory you started from is `T:` on the bdos side.

## Layout

```
include/bootinfo.h      loader -> kernel hand-off
loader/                 BOOTX64.EFI (efi.h written from the spec)
kernel/arch/x86_64/     entry, GDT/IDT/paging/PIC/PIT, PS/2, COM1 - all the CPU-specific code
kernel/*.c              memory, tasks, console, VFS, shell, EDIT, SYS table
kernel/drv/             PCI, AHCI (+ RAM disk), e1000
kernel/lang/            BASIC, the C interpreter, soft math
kernel/gui/             drawing, window manager, forms, Program Manager, File Manager, Control Panel
kernel/dos/             8086 emulator
kernel/net/             BDNET and the remote services
fs/                     bdosfs (shared with the host tool)
tools/                  mkbdosfs, image builder, font generator, QEMU test harnesses
dos/                    sample DOS programs (nasm)
rootfs/                 what ends up on C:
docs/                   PORTING-YEELOONG.md
```

## Known limits

- PS/2 input only: there's no USB stack, so real hardware needs the
  firmware's legacy emulation. Under QEMU, VMware and VirtualBox the mouse
  uses the vmmouse absolute pointer, so it tracks the host cursor exactly.
- Name lookup (`\\BOX2`) only works on the local network. To reach a machine
  across a router, give bterm its address. There's no TCP or DNS yet, and
  bterm's traffic isn't encrypted.
- Tasks are cooperative. A program that never calls into the system can hog
  the CPU. The interpreters and the 8086 emulator yield on their own.
- The 8086 emulator has no timer interrupts or sound, and there's no
  protected-mode DOS.
- The C interpreter doesn't support unions, goto, function pointers,
  bit-fields or `FILE*` I/O yet.
- File timestamps mix the host's local time with the RTC, so they're offset
  by your UTC difference.

## Next: the Yeeloong

`docs/PORTING-YEELOONG.md` is the plan for the Lemote Yeeloong 8089B:
- Booting from PMON.
- XKPHYS instead of page tables.
- Builds with the Loongson 2F errata flags.
- Drivers for the SM712, CS5536 IDE and RTL8139.

All CPU-specific code already sits behind `kernel/arch/`. It hasn't been run
on the hardware.
