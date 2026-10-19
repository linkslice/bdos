#!/bin/sh
# mkimage.sh <out.img> <builddir>
#
# A 192 MiB disk with an MBR and two partitions:
#   1  EFI System Partition (FAT32, 64 MiB)
#        \EFI\BOOT\BOOTX64.EFI   the loader
#        \BDOS\KERNEL.SYS        the kernel
#        \BDOS\ROOTFS.IMG        fallback C: when there's no SATA disk
#   2  bdosfs (type 0xBD, 127 MiB): drive C:, filled from rootfs/ (staged in build/)
set -e
OUT=$1 B=$2
SIZE_MB=192
ESP_START=2048
ESP_SECTORS=$((64 * 2048))
BFS_START=$((ESP_START + ESP_SECTORS))
BFS_SECTORS=$((SIZE_MB * 2048 - BFS_START))

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=0 seek=$SIZE_MB status=none

python3 - "$OUT" $ESP_START $ESP_SECTORS $BFS_START $BFS_SECTORS <<'PY'
import struct, sys
path = sys.argv[1]
a, an, b, bn = map(int, sys.argv[2:])
mbr = bytearray(512)
def entry(ptype, start, count):
    return struct.pack("<B3sB3sII", 0, b"\xfe\xff\xff", ptype, b"\xfe\xff\xff", start, count)
mbr[446:462] = entry(0xEF, a, an)
mbr[462:478] = entry(0xBD, b, bn)
mbr[510:512] = b"\x55\xaa"
with open(path, "r+b") as f:
    f.write(mbr)
PY

IMG="$OUT@@$((ESP_START * 512))"
mformat -i "$IMG" -F -v BDOS-EFI -T $ESP_SECTORS -h 64 -s 32 ::
mmd -i "$IMG" ::/EFI ::/EFI/BOOT ::/BDOS
mcopy -i "$IMG" "$B/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
mcopy -i "$IMG" "$B/KERNEL.SYS" ::/BDOS/KERNEL.SYS
mcopy -i "$IMG" "$B/ROOTFS.IMG" ::/BDOS/ROOTFS.IMG

"$B/mkbdosfs" "$OUT" $((BFS_START * 512)) $((BFS_SECTORS * 512)) BDOS "$B/rootfs"
echo "built $OUT"
