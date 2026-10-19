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
KERNEL_HDRS := $(wildcard kernel/*.h kernel/drv/*.h kernel/lang/*.h kernel/gui/*.h kernel/net/*.h kernel/arch/$(ARCH)/*.h fs/*.h include/*.h)
ROOTFS_FILES := $(shell find rootfs -type f 2>/dev/null | sed 's/ /\\ /g')

.PHONY: all run test nettest clean
all: $(B)/bdos.img

$(B)/BOOTX64.EFI: loader/loader.c loader/efi.h include/bootinfo.h
	@mkdir -p $(B)
	$(CC) $(LOADER_CFLAGS) -c loader/loader.c -o $(B)/loader.o
	lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
		/out:$@ $(B)/loader.o

$(B)/font.h: tools/gen_font.py
	@mkdir -p $(B)
	python3 tools/gen_font.py $(FONT) $@

$(B)/kernel/lang/%.o: kernel/lang/%.c $(KERNEL_HDRS) $(B)/font.h
	@mkdir -p $(dir $@)
	$(CC) $(LANG_CFLAGS) -c $< -o $@

$(B)/%.o: %.c $(KERNEL_HDRS) $(B)/font.h
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

# DOS programs, assembled from dos/*.asm into C:\DOSAPPS
DOS_SRCS := $(wildcard dos/*.asm)
DOS_BINS := $(patsubst dos/%.asm,$(B)/dosapps/%,$(DOS_SRCS))
$(B)/dosapps/%: dos/%.asm
	@mkdir -p $(B)/dosapps
	nasm -f bin -o $(B)/dosapps/$$(echo $* | tr a-z A-Z).$$(grep -q "^hdr:" $< && echo EXE || echo COM) $<
	@touch $@

# The C: drive contents: rootfs/ plus generated programs
$(B)/rootfs.stamp: $(ROOTFS_FILES) $(DOS_BINS)
	rm -rf $(B)/rootfs
	cp -r rootfs $(B)/rootfs
	mkdir -p $(B)/rootfs/DOSAPPS
	cp $(B)/dosapps/*.COM $(B)/dosapps/*.EXE $(B)/rootfs/DOSAPPS/
	cp dos/*.asm $(B)/rootfs/DOSAPPS/
	touch $@

# A small copy of C: that the loader can hand over when there is no disk
$(B)/ROOTFS.IMG: $(B)/mkbdosfs $(B)/rootfs.stamp
	rm -f $@
	$(B)/mkbdosfs $@ 0 4194304 BDOS $(B)/rootfs

$(B)/bdos.img: $(B)/BOOTX64.EFI $(B)/KERNEL.SYS $(B)/ROOTFS.IMG $(B)/mkbdosfs tools/mkimage.sh $(B)/rootfs.stamp
	sh tools/mkimage.sh $@ $(B)

run: $(B)/bdos.img
	sh tools/run.sh $(B)/bdos.img

test: $(B)/bdos.img
	python3 tools/test.py $(B)/bdos.img $(B)/test

nettest: $(B)/bdos.img
	python3 tools/nettest.py $(B)/bdos.img $(B)/nettest

clean:
	rm -rf $(B)
