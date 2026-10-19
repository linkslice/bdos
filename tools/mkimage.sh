#!/bin/sh
# mkimage.sh <out.img> <BOOTX64.EFI> <KERNEL.SYS>
#
# Builds a 64 MiB disk image: an MBR with one EFI System Partition
# (type 0xEF) starting at 1 MiB, formatted FAT32 with mtools.
#   \EFI\BOOT\BOOTX64.EFI   the loader (the UEFI fallback boot path)
#   \BDOS\KERNEL.SYS        the kernel
set -e
OUT=$1 EFI=$2 KERNEL=$3
SIZE_MB=64
START=2048                                   # first sector of the ESP
SECTORS=$((SIZE_MB * 2048 - START))

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=$SIZE_MB status=none

python3 - "$OUT" "$START" "$SECTORS" <<'EOF'
import struct, sys
path, start, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mbr = bytearray(512)
# one partition: not bootable, CHS fields unused (0xFFFFFE), type 0xEF
entry = struct.pack("<B3sB3sII", 0x00, b"\xfe\xff\xff", 0xEF, b"\xfe\xff\xff", start, count)
mbr[446:446 + 16] = entry
mbr[510:512] = b"\x55\xaa"
with open(path, "r+b") as f:
    f.write(mbr)
EOF

IMG="$OUT@@$((START * 512))"
mformat -i "$IMG" -F -v BDOS -T $SECTORS -h 64 -s 32 ::
mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/BDOS
mcopy -i "$IMG" "$EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$KERNEL" ::/BDOS/KERNEL.SYS
echo "built $OUT"
