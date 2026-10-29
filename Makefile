# bdos - Bryan Disk Operating System
#
#   make            build build/bdos.img (bootable UEFI disk image)
#   make run        boot it in QEMU with OVMF
#   make test       headless test suite in QEMU
#   make clean
#
# Needs: clang, lld, llvm-objcopy, a host cc, python3 + Pillow, mtools,
# qemu-system-x86_64, OVMF.

CC      := clang
HOSTCC  ?= cc
ARCH    ?= x86_64
FONT    ?= /usr/share/fonts/opentype/unifont/unifont.otf
B := build

LOADER_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
	-mno-red-zone -fno-stack-protector -mno-stack-arg-probe \
	-O2 -Wall -Wextra -std=c11 -Iinclude

KERNEL_CFLAGS_BASE := -target x86_64-unknown-none-elf -ffreestanding -fno-pic -fno-pie \
	-mno-red-zone -fno-stack-protector -fno-builtin \
	-mcmodel=small -O2 -g -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
	-I$(B) -Iinclude -Ikernel/arch/$(ARCH) -DBDOS_KERNEL
# Only the language interpreters use floating point (SSE2); task switches
# are cooperative and interrupt handlers never touch SSE, so that's safe.
KERNEL_CFLAGS := $(KERNEL_CFLAGS_BASE) -mgeneral-regs-only
LANG_CFLAGS := $(KERNEL_CFLAGS_BASE) -msse2 -mno-80387 -fno-math-errno

KERNEL_C := $(wildcard kernel/*.c kernel/drv/*.c kernel/lang/*.c kernel/gui/*.c kernel/dos/*.c kernel/net/*.c kernel/arch/$(ARCH)/*.c fs/*.c)
KERNEL_S := $(wildcard kernel/arch/$(ARCH)/*.S)
KERNEL_OBJS := $(patsubst %.c,$(B)/%.o,$(KERNEL_C)) $(patsubst %.S,$(B)/%.o,$(KERNEL_S))
KERNEL_HDRS := $(wildcard kernel/*.h kernel/dos/*.h kernel/drv/*.h kernel/lang/*.h kernel/gui/*.h kernel/net/*.h kernel/arch/$(ARCH)/*.h fs/*.h include/*.h)
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null | sed 's/ /\\ /g')

.PHONY: btermtest all run test nettest tcptest hwtest clean iso isotest
all: $(B)/bdos.img

$(B)/BOOTX64.EFI: loader/loader.c loader/efi.h include/bootinfo.h
	@mkdir -p $(B)
	$(CC) $(LOADER_CFLAGS) -c loader/loader.c -o $(B)/loader.o
	lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
		/out:$@ $(B)/loader.o

$(B)/font.h: tools/gen_font.py
	@mkdir -p $(B)
	python3 tools/gen_font.py $(FONT) $@

# the protective MBR's boot code, for disks bdos installs on
$(B)/mbrboot.h: loader/mbrboot.asm
	@mkdir -p $(B)
	nasm -f bin -o $(B)/mbrboot.bin $<
	python3 -c "import sys; d=open('$(B)/mbrboot.bin','rb').read(); print('static const unsigned char mbrboot[] = {' + ','.join(str(x) for x in d) + '};')" > $@

$(B)/kernel/lang/%.o: kernel/lang/%.c $(KERNEL_HDRS) $(B)/font.h
	@mkdir -p $(dir $@)
	$(CC) $(LANG_CFLAGS) -c $< -o $@

$(B)/%.o: %.c $(KERNEL_HDRS) $(B)/font.h $(B)/mbrboot.h
	@mkdir -p $(dir $@)
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(B)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@

$(B)/KERNEL.SYS: $(KERNEL_OBJS) kernel/linker.ld
	ld.lld -nostdlib -static -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(B)/mkbdosfs: tools/mkbdosfs.c fs/bdosfs.c fs/bdosfs.h
	@mkdir -p $(B)
	$(HOSTCC) -O2 -Wall -Wextra -std=gnu11 -o $@ tools/mkbdosfs.c fs/bdosfs.c

# Native 64-bit programs: the C library (lib/), and programs built with it
CLANG_INC := $(shell $(CC) -print-resource-dir)/include
USER_CFLAGS := -target x86_64-unknown-none-elf -ffreestanding -fPIE -fno-stack-protector \
	-nostdinc -isystem lib/include -isystem $(CLANG_INC) -D__bdos__ -fno-addrsig -fno-asynchronous-unwind-tables -fno-unwind-tables \
	-O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LIBC_C := $(wildcard lib/libc/*.c)
LIBC_OBJS := $(patsubst %.c,$(B)/%.o,$(LIBC_C)) $(B)/lib/libc/setjmp.o
LIBC_HDRS := $(wildcard lib/include/*.h lib/include/sys/*.h lib/libc/*.h) include/bdos_sys.h

$(B)/lib/%.o: lib/%.c $(LIBC_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(USER_CFLAGS) -c $< -o $@

$(B)/lib/%.o: lib/%.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@

$(B)/lib/libc.a: $(LIBC_OBJS)
	rm -f $@
	llvm-ar rcs $@ $^

$(B)/lib/crt0.o: lib/crt0.S
	@mkdir -p $(dir $@)
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@

NATIVE_SRCS := $(wildcard lib/samples/*.c)
NATIVE_BINS := $(patsubst lib/samples/%.c,$(B)/bin/%.stamp,$(NATIVE_SRCS))
$(B)/bin/%.stamp: lib/samples/%.c $(B)/lib/libc.a $(B)/lib/crt0.o lib/bdos.ld $(LIBC_HDRS)
	@mkdir -p $(B)/bin $(B)/lib/samples
	$(CC) $(USER_CFLAGS) -c $< -o $(B)/lib/samples/$*.o
	ld.lld -static -nostdlib -T lib/bdos.ld -o $(B)/bin/$$(echo $* | tr a-z A-Z).EXE \
		$(B)/lib/crt0.o $(B)/lib/samples/$*.o $(B)/lib/libc.a
	@touch $@

# The Tiny C Compiler (third_party/tinycc, LGPL), built as a bdos program.
# Its runtime library (libtcc1.a) is compiled by tcc itself, so a host tcc
# is built first just for that.
TCC_DIR := third_party/tinycc
TCC_DEPS := $(wildcard $(TCC_DIR)/*.c $(TCC_DIR)/*.h $(TCC_DIR)/include/*.h)
TCC_LIB_C := libtcc1 va_list stdatomic builtin dsohandle
TCC_LIB_S := alloca atomic
$(B)/tcc/TCC.EXE: $(TCC_DEPS) $(B)/lib/libc.a $(B)/lib/crt0.o lib/bdos.ld $(LIBC_HDRS)
	@mkdir -p $(B)/tcc
	$(CC) $(USER_CFLAGS) -w -include $(TCC_DIR)/config.h -c $(TCC_DIR)/tcc.c -o $(B)/tcc/tcc.o
	ld.lld -static -nostdlib -T lib/bdos.ld -o $@ $(B)/lib/crt0.o $(B)/tcc/tcc.o $(B)/lib/libc.a

$(B)/hosttcc/tcc: $(TCC_DEPS)
	@mkdir -p $(B)/hosttcc
	printf '#define TCC_VERSION "0.9.28rc"\n#define CONFIG_TCC_PREDEFS 0\n#define CONFIG_TCC_BACKTRACE 0\n#define CONFIG_TCC_BCHECK 0\n#define CONFIG_TCCDIR "."\n#define TCC_TARGET_X86_64 1\n#define ONE_SOURCE 1\n' > $(B)/hosttcc/config.h
	$(HOSTCC) -O1 -w -I$(B)/hosttcc -include $(B)/hosttcc/config.h -o $@ $(TCC_DIR)/tcc.c -lm -ldl -lpthread

$(B)/tcc/libtcc1.a: $(B)/hosttcc/tcc $(wildcard $(TCC_DIR)/lib/*)
	@mkdir -p $(B)/tcc/lib
	for f in $(TCC_LIB_C) runmain; do $(B)/hosttcc/tcc -B$(TCC_DIR) -I$(TCC_DIR)/include -D__bdos__ -c $(TCC_DIR)/lib/$$f.c -o $(B)/tcc/lib/$$f.o || exit 1; done
	for f in $(TCC_LIB_S); do $(B)/hosttcc/tcc -B$(TCC_DIR) -I$(TCC_DIR)/include -c $(TCC_DIR)/lib/$$f.S -o $(B)/tcc/lib/$$f.o || exit 1; done
	rm -f $@
	llvm-ar rcs $@ $(patsubst %,$(B)/tcc/lib/%.o,$(TCC_LIB_C) $(TCC_LIB_S))

# crti.o and crtn.o: tcc links them around every program; bdos needs nothing there
$(B)/lib/crtn.o:
	@mkdir -p $(B)/lib
	printf '' | $(CC) -target x86_64-unknown-none-elf -x assembler -c - -o $@

# DOS programs, assembled from dos/*.asm into C:\DOSAPPS
DOS_SRCS := $(wildcard dos/*.asm)
DOS_BINS := $(patsubst dos/%.asm,$(B)/dosapps/%,$(DOS_SRCS))
$(B)/dosapps/%: dos/%.asm dos/lib.inc
	@mkdir -p $(B)/dosapps
	nasm -f bin -o $(B)/dosapps/$$(echo $* | tr a-z A-Z).$$(grep -q "^hdr:" $< && echo EXE || echo COM) $<
	@touch $@

# The C: drive contents: rootfs/ plus generated programs
$(B)/rootfs.stamp: tools/gen_sounds.py tools/mktestiso.py $(ROOTFS_FILES) $(DOS_BINS) $(NATIVE_BINS) $(B)/lib/libc.a $(B)/lib/crt0.o \
		$(B)/lib/crtn.o $(B)/tcc/TCC.EXE $(B)/tcc/libtcc1.a $(B)/BOOTX64.EFI $(B)/KERNEL.SYS
	rm -rf $(B)/rootfs
	cp -r rootfs $(B)/rootfs
	mkdir -p $(B)/rootfs/PROGMAN/Startup $(B)/rootfs/BIN $(B)/rootfs/BDOS/INCLUDE/sys $(B)/rootfs/BDOS/LIB $(B)/rootfs/BDOS/SAMPLES
	cp $(B)/bin/*.EXE $(B)/rootfs/BIN/
	cp lib/samples/*.c $(B)/rootfs/BDOS/SAMPLES/
	cp lib/include/*.h $(B)/rootfs/BDOS/INCLUDE/
	cp lib/include/sys/*.h $(B)/rootfs/BDOS/INCLUDE/sys/
	cp include/bdos_sys.h $(B)/rootfs/BDOS/INCLUDE/
	python3 tools/gen_sounds.py $(B)/rootfs/BDOS/SOUNDS
	cp $(B)/lib/libc.a $(B)/lib/crt0.o $(B)/rootfs/BDOS/LIB/
	cp $(B)/lib/crt0.o $(B)/rootfs/BDOS/LIB/crt1.o
	cp $(B)/lib/crtn.o $(B)/rootfs/BDOS/LIB/crti.o
	cp $(B)/lib/crtn.o $(B)/rootfs/BDOS/LIB/crtn.o
	mkdir -p $(B)/rootfs/BDOS/TCC/include
	cp $(B)/tcc/TCC.EXE $(B)/rootfs/BIN/
	cp $(B)/tcc/libtcc1.a $(B)/tcc/lib/runmain.o $(B)/rootfs/BDOS/TCC/
	cp $(TCC_DIR)/include/*.h $(B)/rootfs/BDOS/TCC/include/
	cp $(TCC_DIR)/COPYING $(B)/rootfs/BDOS/TCC/COPYING.TXT
	mkdir -p $(B)/rootfs/DOSAPPS
	cp $(B)/dosapps/*.COM $(B)/dosapps/*.EXE $(B)/rootfs/DOSAPPS/
	cp dos/*.asm dos/*.inc $(B)/rootfs/DOSAPPS/
	python3 tools/mktestiso.py $(B)/rootfs/BDOS/TESTS	# CD images for MOUNT and the tests
	mkdir -p $(B)/rootfs/BDOS/BOOT		# what INSTALL puts on a new disk's EFI partition
	cp $(B)/BOOTX64.EFI $(B)/KERNEL.SYS $(B)/rootfs/BDOS/BOOT/
	touch $@

# A small copy of C: that the loader can hand over when there is no disk
$(B)/ROOTFS.IMG: $(B)/mkbdosfs $(B)/rootfs.stamp
	rm -f $@
	$(B)/mkbdosfs $@ 0 16777216 BDOS $(B)/rootfs

# The live CD: C: is a RAM disk of the whole system with room to spare, the
# desktop starts by itself, and Install bdos is in Program Manager's Main
# group. The ISO's El Torito image is a FAT EFI system partition holding the
# loader, the kernel and that RAM disk; UEFI firmware boots it like any other.
iso: $(B)/bdos.iso
$(B)/bdos.iso: $(B)/BOOTX64.EFI $(B)/KERNEL.SYS $(B)/mkbdosfs $(B)/rootfs.stamp tools/mkiso.py loader/biosboot.asm
	rm -rf $(B)/liveroot $(B)/LIVEROOT.IMG $(B)/efiboot.img
	cp -r $(B)/rootfs $(B)/liveroot
	echo "bdos is running from the CD: changes are lost when it restarts." > $(B)/liveroot/BDOS/LIVE.TXT
	sed -i 's/^REM WIN\r*$$/WIN\r/' $(B)/liveroot/AUTOEXEC.BAT
	printf '@install\r\n' > "$(B)/liveroot/PROGMAN/Main/Install bdos.lnk"
	printf '"Install bdos.lnk" icon=disk\n"Install bdos.lnk" order=0\n' >> $(B)/liveroot/PROGMAN/Main/.xattr
	$(B)/mkbdosfs $(B)/LIVEROOT.IMG 0 33554432 BDOS $(B)/liveroot
	dd if=/dev/zero of=$(B)/efiboot.img bs=1M count=48 status=none
	mformat -i $(B)/efiboot.img -v BDOS-CD -h 64 -s 32 -T 98304 ::
	mmd -i $(B)/efiboot.img ::/EFI ::/EFI/BOOT ::/BDOS
	mcopy -i $(B)/efiboot.img $(B)/BOOTX64.EFI ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $(B)/efiboot.img $(B)/KERNEL.SYS ::/BDOS/KERNEL.SYS
	mcopy -i $(B)/efiboot.img $(B)/LIVEROOT.IMG ::/BDOS/ROOTFS.IMG
	mcopy -i $(B)/efiboot.img $(B)/liveroot/BDOS/LIVE.TXT ::/BDOS/LIVE.TXT
	nasm -f bin -o $(B)/biosboot.bin loader/biosboot.asm
	python3 tools/mkiso.py $@ $(B)/efiboot.img README.md $(B)/biosboot.bin

$(B)/bdos.img: $(B)/BOOTX64.EFI $(B)/KERNEL.SYS $(B)/ROOTFS.IMG $(B)/mkbdosfs tools/mkimage.sh $(B)/rootfs.stamp $(B)/mbrboot.h
	sh tools/mkimage.sh $@ $(B)

run: $(B)/bdos.img
	sh tools/run.sh $(B)/bdos.img

test: $(B)/bdos.img
	python3 tools/test.py $(B)/bdos.img $(B)/test

nettest: $(B)/bdos.img
	python3 tools/nettest.py $(B)/bdos.img $(B)/nettest

hwtest: $(B)/bdos.img
	python3 tools/hwtest.py $(B)/bdos.img $(B)/hwtest

tcptest: $(B)/bdos.img
	python3 tools/tcptest.py $(B)/bdos.img $(B)/tcptest

# bterm lives in its own repository (github.com/linkslice/bterm); point
# BTERM at a built copy, by default one checked out next to this one.
BTERM ?= ../bterm/bterm

btermtest: $(B)/bdos.img
	@test -x $(BTERM) || { echo "btermtest needs bterm: clone github.com/linkslice/bterm next to bdos and make it, or set BTERM="; exit 1; }
	python3 tools/btermtest.py $(B)/bdos.img $(BTERM) $(B)/btermtest

isotest: $(B)/bdos.iso
	python3 tools/isotest.py $(B)/bdos.iso $(B)/isotest

clean:
	rm -rf $(B)

# ---- the MIPS64 port: Loongson 2E/2F (QEMU's Fuloong 2E; the Lemote Yeeloong) ----
#   make mips        build-mips64/KERNEL.SYS, a MIPS64 ELF PMON (or QEMU -kernel) can load
#   make run-mips    boot it in QEMU's fuloong2e, with C: as an initrd, on the serial port
MB := build-mips64
MIPS_CFLAGS_BASE := -target mips64el-unknown-none-elf -march=mips3 -mabi=64 -ffreestanding -fno-pic \
	-mno-abicalls -G0 -fno-stack-protector -fno-builtin -O2 -g -Wall -Wextra -Wno-unused-parameter \
	-std=gnu11 -I$(B) -Iinclude -Ikernel/arch/mips64 -DBDOS_KERNEL
MIPS_CFLAGS := $(MIPS_CFLAGS_BASE) -mhard-float
MIPS_LANG_CFLAGS := $(MIPS_CFLAGS_BASE) -mhard-float
MIPS_C := $(filter-out kernel/arch/%,$(KERNEL_C)) $(wildcard kernel/arch/mips64/*.c)
MIPS_OBJS := $(patsubst %.c,$(MB)/%.o,$(MIPS_C)) $(MB)/kernel/arch/mips64/entry.o
MIPS_HDRS := $(filter-out kernel/arch/%,$(KERNEL_HDRS)) $(wildcard kernel/arch/mips64/*.h)

.PHONY: mips run-mips mipstest
mipstest: $(MB)/KERNEL.SYS $(B)/ROOTFS.IMG
	python3 tools/mipstest.py $(MB)/KERNEL.SYS $(B)/ROOTFS.IMG $(MB)/test

mips: $(MB)/KERNEL.SYS

$(MB)/kernel/lang/%.o: kernel/lang/%.c $(MIPS_HDRS) $(B)/font.h
	@mkdir -p $(dir $@)
	$(CC) $(MIPS_LANG_CFLAGS) -c $< -o $@

$(MB)/%.o: %.c $(MIPS_HDRS) $(B)/font.h $(B)/mbrboot.h
	@mkdir -p $(dir $@)
	$(CC) $(MIPS_CFLAGS) -c $< -o $@

$(MB)/kernel/arch/mips64/entry.o: kernel/arch/mips64/entry.S
	@mkdir -p $(dir $@)
	$(CC) -target mips64el-unknown-none-elf -march=mips3 -mabi=64 -mno-abicalls -fno-pic -c $< -o $@

$(MB)/KERNEL.SYS: $(MIPS_OBJS) kernel/arch/mips64/linker.ld
	ld.lld -m elf64ltsmip -nostdlib -static -T kernel/arch/mips64/linker.ld -o $@ $(MIPS_OBJS)

run-mips: $(MB)/KERNEL.SYS $(B)/ROOTFS.IMG
	qemu-system-mips64el -M fuloong2e -m 256 -kernel $(MB)/KERNEL.SYS -initrd $(B)/ROOTFS.IMG \
		-nographic -serial mon:stdio
