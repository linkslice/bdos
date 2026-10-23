#!/usr/bin/env python3
"""mkiso.py <out.iso> <efiboot.img> <readme> [biosboot.bin]

A UEFI-bootable ISO 9660 image: the El Torito boot entry (platform EFI) is
efiboot.img, a FAT image with \\EFI\\BOOT\\BOOTX64.EFI and bdos on it. The
readme is there to read from any machine. A BIOS-only machine (Proxmox's
default SeaBIOS) gets biosboot.bin, which says how to boot it with UEFI.
"""
import os, sys
import pycdlib

out, efi, readme = sys.argv[1:4]
bios = sys.argv[4] if len(sys.argv) > 4 else None
iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, joliet=3, rock_ridge="1.09", vol_ident="BDOS")
iso.add_file(efi, iso_path="/EFIBOOT.IMG;1", joliet_path="/efiboot.img", rr_name="efiboot.img")
iso.add_file(readme, iso_path="/README.MD;1", joliet_path="/README.md", rr_name="README.md")
if bios:
    iso.add_file(bios, iso_path="/BIOSBOOT.BIN;1", joliet_path="/biosboot.bin", rr_name="biosboot.bin")
    iso.add_eltorito("/BIOSBOOT.BIN;1", bootcatfile="/BOOT.CAT;1", media_name="noemul", boot_load_size=4)
iso.add_eltorito("/EFIBOOT.IMG;1", bootcatfile="/BOOT.CAT;1", media_name="noemul", efi=True,
                 platform_id=0xEF, boot_load_size=0)
iso.write(out)
iso.close()
print(f"built {out} ({os.path.getsize(out) // 1024} KiB)")
