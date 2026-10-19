# bdos - Bryan Disk Operating System
#
#   make            build build/bdos.img (bootable UEFI disk image)
#   make run        boot it in QEMU with OVMF
#   make clean
#
# Needs: clang, lld, llvm-objcopy, python3 + Pillow, mtools, qemu, OVMF.

CC      := clang
FONT    ?= /usr/share/fonts/opentype/unifont/unifont.otf
OVMF_CODE ?= /usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS ?= /usr/share/OVMF/OVMF_VARS_4M.fd

B := build

# ---- loader: PE32+ UEFI application ----
LOADER_CFLAGS := -target x86_64-unknown-windows -ffreestanding -fshort-wchar \
	-mno-red-zone -fno-stack-protector -mno-stack-arg-probe \
	-O2 -Wall -Wextra -std=c11 -Iinclude

# ---- kernel: freestanding ELF64, identity mapped at 2 MiB ----
KERNEL_CFLAGS := -target x86_64-unknown-none-elf -ffreestanding -fno-pic -fno-pie \
	-mno-red-zone -mgeneral-regs-only -fno-stack-protector -fno-builtin \
	-mcmodel=small -O2 -Wall -Wextra -std=c11 -I$(B) -Iinclude

KERNEL_SRCS := $(wildcard kernel/*.c)
KERNEL_OBJS := $(patsubst kernel/%.c,$(B)/kernel/%.o,$(KERNEL_SRCS)) $(B)/kernel/entry.o

.PHONY: all run clean
all: $(B)/bdos.img

$(B)/BOOTX64.EFI: loader/loader.c loader/efi.h include/bootinfo.h
	@mkdir -p $(B)
	$(CC) $(LOADER_CFLAGS) -c loader/loader.c -o $(B)/loader.o
	lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib \
		/out:$@ $(B)/loader.o

$(B)/font.h: tools/gen_font.py
	@mkdir -p $(B)
	python3 tools/gen_font.py $(FONT) $@

$(B)/kernel/%.o: kernel/%.c kernel/kernel.h include/bootinfo.h $(B)/font.h
	@mkdir -p $(B)/kernel
	$(CC) $(KERNEL_CFLAGS) -c $< -o $@

$(B)/kernel/entry.o: kernel/entry.S
	@mkdir -p $(B)/kernel
	$(CC) -target x86_64-unknown-none-elf -c $< -o $@

$(B)/KERNEL.SYS: $(KERNEL_OBJS) kernel/linker.ld
	ld.lld -nostdlib -static -T kernel/linker.ld -o $@ $(KERNEL_OBJS)

$(B)/bdos.img: $(B)/BOOTX64.EFI $(B)/KERNEL.SYS tools/mkimage.sh
	sh tools/mkimage.sh $@ $(B)/BOOTX64.EFI $(B)/KERNEL.SYS

run: $(B)/bdos.img
	sh tools/run.sh $(B)/bdos.img

clean:
	rm -rf $(B)
