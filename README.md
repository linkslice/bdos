# bdos: Bryan Disk Operating System

A 64-bit, single-user OS that should feel like home to anyone who grew up at
a `C:\>` prompt. It boots on UEFI with its own loader and drops you at a DOS
prompt. `WIN` starts a Program Manager desktop that works like Plan 9:
desktops nest inside windows, and `WIN \\HOST` puts another machine's desktop
in a window, with your own folder mounted on its `T:` drive.

```
bdos 0.11 (x86-64) - Bryan Disk Operating System
255,848 K memory, 1024x768 display, mouse

Network: e1000 52:54:00:12:34:56, this machine is \\BDOS-123456
Welcome to bdos. Type HELP for a list of commands.

C:\>_
```

## Build and run

You need clang, lld, llvm-objcopy, a host cc, python3 with Pillow, mtools,
nasm, QEMU and OVMF. On Debian or Ubuntu, install `clang lld llvm python3-pil
mtools nasm qemu-system-x86 ovmf fonts-unifont` (and `qemu-system-mips` for
the MIPS port).

```sh
make            # build/bdos.img: a 192 MiB disk, with an ESP plus a bdosfs partition
make iso        # build/bdos.iso: a live CD with the installer (for Proxmox and real PCs)
make run        # boot it in QEMU; COM1 is a terminal too
make test       # the headless suites: shell, files, BASIC, C, DOS, DPMI, desktop, SMP, native programs...
make nettest    # two VMs on one virtual LAN: NET VIEW, CTTY/WIN \\BOX2, NET USE, CPU
make tcptest    # TCP/IP to the outside: DNS, FETCH, TELNET out, telnet in
make hwtest     # HD Audio (recorded and listened to), Sound Blaster, USB, NVMe boot, VirtIO
make isotest    # boot the ISO, install to a blank disk (prompt and window), boot that, upgrade
make btermtest  # bterm (../bterm) against a VM: DHCP, a session, the desktop, T:
make mips       # the MIPS64 kernel (Loongson); make mipstest boots it in QEMU's Fuloong 2E
```

`SMP=1 make test` runs the suites on one core instead of four.

The image also boots real UEFI PCs from a USB stick if you `dd` it across
(Secure Boot off). bdos has its own drivers for AHCI, IDE, NVMe, VirtIO and
VirtIO SCSI disks,
xHCI USB keyboards, mice and tablets, PS/2 (with the wheel), e1000 and VirtIO
network cards and HD Audio.

### The live CD and installing (Proxmox)

`build/bdos.iso` boots straight into a live bdos running from a RAM disk,
desktop and all, with **Install bdos** in Program Manager: pick a disk, set
the computer's name, its address (DHCP or fixed), the time zone and time
server, and whether the desktop starts at boot. It partitions the disk (GPT:
an EFI partition and bdosfs), copies the system across and makes it
bootable. Running it again over an installed bdos offers an **upgrade** that
keeps your files and settings. At the prompt, `FDISK`, `FORMAT` and
`INSTALL n` do the same.

On Proxmox VE, make a VM with:
- **OS**: the ISO; **System**: BIOS `OVMF (UEFI)` and an EFI disk with
  **Pre-Enroll keys unticked** (bdos isn't signed for Secure Boot); machine
  `i440fx` (the default) or `q35`. Left on SeaBIOS, the CD says what to change.
- **Disks**: the default VirtIO SCSI controller is fine, as are VirtIO Block,
  SATA, IDE and NVMe; a few GB is plenty
- **Network**: VirtIO or Intel E1000
- **Display**: Default (VGA) or `std`; 256 MB of memory or more

Boot, install, detach the ISO, reboot. Then reach it from your desk with
bterm (`bterm <address>`) - enable it in the installer's Network page.

## What's in it

**Boot.** `loader/` is our own PE32+ UEFI application. It sets a
1024×768 GOP mode, loads `\BDOS\KERNEL.SYS` (ELF64) at 2 MiB, collects
the memory map and RSDP, and exits boot services. It passes a
`struct bdos_bootinfo` to the kernel.

**Kernel.**
- Its own page tables, with the framebuffer mapped write-combining and MMIO
  wherever PCI put it.
- Every core: the ACPI MADT, INIT-SIPI-SIPI, a local APIC timer per core.
  Tasks are preemptive (10 ms slices) and spread over the cores; a big
  kernel lock keeps the old single-core code safe, and the interpreters and
  the DOS emulator run outside it, so four DOS programs get four cores.
  Each desktop has a lock of its own and draws outside the big one, so
  several people connected with bterm draw on different cores at once.
  `CPU$` reports per-core load and parks or unparks cores (`echo off 2 > CPU$`).
- A bitmap page allocator and a slab heap, both thread-safe.
- Each task has its own console, current directory and desktop.

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
- Program Manager is a real MDI window. Each group is a folder under
  `C:\PROGMAN` and each icon is a file in it (a `.lnk` holds a command).
  Groups move, resize, minimize, maximize and scroll; drag icons between
  groups, or drop any file in to make an item that opens it.
- File Manager has a folder tree, a sortable file list, and copy, move,
  rename, delete, new folder and properties. Select with Ctrl+click,
  Shift+click or by dragging a box; Cut, Copy and Paste (Ctrl+X, C, V) work
  across File Manager windows. Drag files between folders, drives, File
  Manager windows, Program Manager, or into a shell (which types their
  names).
- Right-click almost anything for a menu: the desktop, a title bar, icons,
  groups and files. Menus have Windows 3.1 keyboard letters, and Alt+letter
  opens a window's menu bar.
- Every window minimizes to an icon along the bottom of the desktop. The
  control-menu box, a right-click on the title, or Alt+Space give Restore,
  Minimize, Maximize and Close.
- Themes are files in `C:\BDOS\THEMES`: window colours, desktop colour and
  an optional desktop pattern. The defaults are bdos Classic, Ocean, Plum,
  Graphite, Rose, Desert, Plan 9, Sunrise to Sunset, and of course Hot Dog
  Stand. Save your own from the Themes applet.
- The desktop can follow the time of day: night blue, a sunrise, a day sky,
  a sunset. Tick it in the Desktop applet, where Play a Day shows you a
  whole day in five seconds.
- Each terminal window is a full shell.
- Programs talk to the desktop through devices: `DESK$`, `DRAW$` and
  `MOUSE$`. For example, `echo color #3a3a6e > DESK$`.
- `WIN` typed inside a window nests a whole desktop in that window, and each
  level gets its own colour.
- Windows resize from any edge or corner (the pointer shows which), DOS
  games and nested desktops included; shells reflow to the new size.
  Double-click a title bar, or use the button beside the close box, to
  maximize. Seen through bterm, the desktop follows bterm's window size.
- Shell windows keep 500 lines of scrollback (the wheel, Shift+PgUp), and
  text can be selected, copied and pasted; the clipboard is also `CLIP$`.
  The mouse wheel scrolls whatever is under the pointer.
- Program Manager items can set a working directory, start minimized, or
  pin a DOS program's speed; the Startup group runs with the desktop; and
  Options has Auto Arrange, Minimize on Use and Save Settings on Exit.
  F1 opens Help, which reads the files in `C:\Docs`.
- The Control Panel has applets for Date/Time (with a calendar, time zone
  and an internet time server), Network (name, DHCP or a fixed address,
  who may connect, browse the LAN, open a remote shell or desktop),
  Desktop (colour, time of day, start at boot), Themes, Mouse, Keyboard,
  DOS (speed, screen scaling) and System.
- Accessories: Calculator, Character Map, Terminal (a telnet phone book),
  Task Manager (Ctrl+Esc), Eyes and Neko; Mines in Games; Clock and Pulse,
  whose minimized icons stay live.

**Settings** live in `C:\BDOS\SETTINGS.INI` and change the same way from
everywhere: the Control Panel, `CONTROL key=value` at the prompt, or the
`OS_SetSetting` SYS call from BASIC and C. `DATE` and `TIME` set the clock.

**DOS programs.** Every DOS program runs in a box of its own - a whole PC:
- An interpreted 386/486 (real mode, protected mode, V86-free DPMI) with the
  x87 run on the real FPU, 8259 PICs, an 8254 PIT, the 8042 keyboard
  controller with real scancodes, CMOS, A20, the BIOS and DOS itself
  (files, FCBs, EXEC, XMS, environment and `COMMAND.COM /C`).
- A VGA: text modes, mode 13h, planar 16-colour modes, mode X and VESA
  banked modes, in a window or full screen; an INT 33h mouse.
- A Sound Blaster 16 (DSP 4.05, 8- and 16-bit DMA, IRQ 7) and an OPL2 FM
  synthesizer, played through the sound card (`BLASTER=A220 I7 D1 H5 T6`).
- A DPMI 0.9 host, so DOS/4GW-style 32-bit programs run.
- Speed control: each program runs at XT, AT, 386, 486, Pentium or flat-out
  speed (the `dos.speed` setting, or a file's own "Run at" in File Manager).
- Saved states: Ctrl+Alt+F11 freezes a program to `C:\BDOS\STATES`,
  Ctrl+Alt+F12 goes back to it, and `RESUME NAME` starts from one even after a
  reboot.
- `SUBST A: T:\GAME` makes a folder drive A:, for installers that will only
  run from a floppy.
- `C:\DOSAPPS` has samples and tests assembled from `dos/*.asm`.

**Native 64-bit programs.** `.EXE` files that are ELF run as real x86-64
programs in ring 3, each with its own address space, threads that run on all
the cores, and system calls for files, the console, TCP/IP and sound.
- A C library (`lib/`): stdio with a full printf/scanf, exact
  float-to-text and correctly rounded strtod, malloc, math, time, dirent,
  setjmp, and bdos.h for threads, mutexes, sockets and sound.
- **TCC**, the Tiny C Compiler, runs on bdos: `tcc hello.c` makes
  `HELLO.EXE`, `tcc -run hello.c` compiles and runs in memory. The library
  and headers are in `C:\BDOS\LIB` and `C:\BDOS\INCLUDE`; samples in
  `C:\BDOS\SAMPLES` (`PRIMES.EXE` counts primes on every core).
- Ctrl+C stops a native program, threads and all.
- `.C` files still run in the interpreter, as before.

**Sound.** An HD Audio driver and a mixer: `PLAY file.wav`, native programs'
streams, the DOS boxes' Sound Blasters and OPL2s all play at once.

**Networking (BDNET).** A small LAN protocol carried directly on Ethernet,
in the spirit of NetBEUI, with an e1000 driver.
- `NET VIEW` finds the other machines on the network.
- `CTTY \\BOX2` opens a shell on another machine.
- `WIN \\BOX2` shows its desktop, either full screen or in a window
  (F12 hangs up).
- `NET USE E: \\BOX2\C` maps a drive. In every remote session, `T:` is the
  directory you launched the session from, the same idea as drawterm's
  `/mnt/term`.

**TCP/IP, and bterm.** ARP, IPv4, ICMP, UDP, TCP and a DHCP client.
- `IPCONFIG` shows the address; `PING` and everything else take names now
  (`NSLOOKUP` asks DNS directly).
- `TELNET host [port]` is a client with an ANSI terminal and CP437, good
  for BBSes. With `CONTROL net.telnet=1`, bdos is a telnet server too: each
  caller gets a shell (asked for `net.password` if one's set).
- `FETCH http://host/path [file]` downloads over HTTP.
- `NETSTAT` lists TCP connections.
- `CONTROL net.ip=...` sets a fixed address instead of DHCP.
- BDNET also runs over UDP port 17979, so sessions can cross routers.
- **[bterm](https://github.com/linkslice/bterm)** is a drawterm for bdos.
  From Linux or macOS, `bterm host` opens the machine's desktop in a window,
  and `bterm -t host` opens a shell in your terminal. Either way, the
  directory you started from is `T:` on the bdos side.
- `CPU \\HOST command` runs a command on another machine with your
  directory as its `T:`: a CPU server, Plan 9 style.

**Pulse** shows every core's load as LED bars, and its icon keeps
showing them when it's minimized.

## Layout

```
include/                bootinfo.h (loader -> kernel), bdos_sys.h (system calls)
loader/                 BOOTX64.EFI (efi.h written from the spec)
kernel/arch/x86_64/     entry, GDT/IDT/paging, SMP and the APICs, user mode, PS/2, COM1
kernel/arch/mips64/     the Loongson port: PMON entry, exceptions, CP0 timer, Bonito I/O
kernel/*.c              memory, tasks, console, VFS, shell, EDIT, native programs, audio mixer
kernel/drv/             PCI, AHCI, IDE, NVMe, VirtIO (disk, SCSI, net), xHCI USB, HD Audio, e1000, RAM disk
kernel/lang/            BASIC, the C interpreter, soft math
kernel/gui/             drawing, window manager, forms, Program Manager, File Manager, Control Panel,
                        the installer, Pulse and the accessories
kernel/dos/             the DOS machine: 386/486, x87, hardware, VGA, BIOS, DOS, DPMI, Sound Blaster, OPL2
kernel/net/             BDNET, the remote services, IP, TCP and DNS, telnet and HTTP
lib/                    the C library and sample native programs
third_party/tinycc/     the Tiny C Compiler (LGPL; see BDOS.txt there)
fs/                     bdosfs (shared with the host tool)
tools/                  mkbdosfs, image builder, font and table generators, QEMU test harnesses
dos/                    DOS programs and tests (nasm)
rootfs/                 what ends up on C:
docs/                   PORTING-YEELOONG.md
```

## Known limits

- USB: no hubs yet (plug keyboards into the machine itself), and only HID
  devices. HD Audio plays but doesn't record.
- TCP is plain: no window scaling or congestion control. No TLS, so FETCH
  is http:// only, and bterm's traffic isn't encrypted.
- DPMI runs DOS/4GW (Doom) and its own tests; DJGPP binaries are untested.
  There's no paging, so DPMI memory is flat physical memory (16 MB per box).
- The OPL2 has no OPL3 mode; rhythm-mode drums are approximate.
- Native programs: no fork/exec of other native programs, no signals, and
  the C library is a small one.
- The C interpreter doesn't support unions, goto, function pointers,
  bit-fields or `FILE*` I/O yet.

## The Yeeloong (MIPS64)

`make mips` builds the kernel for Loongson 2E/2F, and `make mipstest` boots
it on QEMU's Fuloong 2E: the shell, BASIC, C and DOS programs run there,
with preemptive tasks, on the serial port. It hasn't met a real Yeeloong yet;
`docs/PORTING-YEELOONG.md` says what's done and what's next (the SM712
display, PCI, the keyboard).
