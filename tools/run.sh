#!/bin/sh
# run.sh <bdos.img> [extra qemu args]
# Boots bdos under QEMU + OVMF. COM1 is on this terminal, so you can
# type at the bdos prompt from here as well as in the QEMU window.
set -e
IMG=$1; shift
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
VARS=$(dirname "$IMG")/OVMF_VARS.fd
[ -f "$VARS" ] || cp "$OVMF_VARS" "$VARS"
ACC="-accel tcg"
[ -r /dev/kvm ] && [ -w /dev/kvm ] && ACC="-accel kvm -accel tcg"	# KVM when there is one

exec qemu-system-x86_64 \
	-machine q35 $ACC -m 256M \
	-drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
	-drive if=pflash,format=raw,file="$VARS" \
	-drive format=raw,file="$IMG" \
	-vga std \
	-serial stdio \
	"$@"
