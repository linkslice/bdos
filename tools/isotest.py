#!/usr/bin/env python3
"""The live CD and the installer: boot bdos.iso with a blank disk, install
from the prompt, then boot the disk alone and check it's a working bdos with
the settings it was given. Then the same through the Install bdos window.

Usage: isotest.py <bdos.iso> <outdir> [suite ...]
"""
import os, re, subprocess, sys, tempfile, time, atexit, shutil


def scratch():
    """a temporary directory, removed when the tests end"""
    d = tempfile.mkdtemp()
    atexit.register(shutil.rmtree, d, True)
    return d

ISO = os.path.abspath(sys.argv[1])
OUTDIR = sys.argv[2]
ONLY_ARGS = sys.argv[3:]
blank = os.path.join(scratch(), "blank.img")
subprocess.run(["truncate", "-s", "2G", blank], check=True)
sys.argv = [sys.argv[0], blank, OUTDIR] + ONLY_ARGS
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])     # VM, check(), suite(); `disk` is a copy of the blank


def live_shell(vm):
    """The live CD starts the desktop by itself: a shell from Alt+N"""
    vm.wait_for("bdos 0.12", 120)
    time.sleep(6)
    start = len(vm.log)
    vm.keys("alt-n")
    vm.wait_for("C:\\>", 20, start)
    time.sleep(0.5)


if suite("cli"):
    vm = VM("live", ["-cdrom", ISO])
    try:
        def live():
            live_shell(vm)
            vm.cmd("type \\bdos\\live.txt", "running from the CD")
            vm.cmd("drives", "RAM disk")
            vm.cmd("fdisk", ["disk0", "2.0 GB", "QEMU HARDDISK", "empty"])
            vm.cmd("dir \\bdos\\boot", ["BOOTX64.EFI", "KERNEL.SYS"])
            vm.screenshot("iso-01-live")
        check("the live CD starts the desktop, with a blank disk to install to", live)

        def install():
            vm.cmd("control net.ntp=time.example")
            vm.cmd("hostname livebox")
            vm.cmd("install 0 /Y", ["Partitioning", "Copying", "Done", "bdos is on disk 0"], timeout=300)
            vm.cmd("fdisk", ["partition 1: EFI system", "partition 2: bdosfs", "bdos (2 partitions)"])
        check("INSTALL puts bdos on the disk", install)
    finally:
        vm.close()

    vm = VM("installed", [])          # the same disk, no CD
    try:
        def installed():
            live_shell(vm)                # it starts the desktop, as asked
            vm.cmd("drives", "disk0 partition 2")
            vm.cmd("ver", "bdos version 0.12")
            vm.cmd("hostname", "LIVEBOX")
            vm.cmd("control list", "net.ntp=time.example")
            vm.cmd("dir \\bdos\\live.txt", absent="LIVE.TXT")
            vm.cmd("dir \\progman\\main", absent="Install bdos")
            vm.cmd("type \\autoexec.bat", "WIN")
            vm.cmd("libctest", "libctest: all passed", timeout=60)
            vm.cmd("echo kept > \\kept.txt")
        check("the installed disk boots on its own, with its settings", installed)

        def upgrade():
            # renew bdos over itself from the installed system onto... itself is C:,
            # which INSTALL refuses; so check the refusal
            vm.cmd("install 0 /UPGRADE /Y", "running from that disk")
        check("INSTALL won't touch the disk it runs from", upgrade)
    finally:
        vm.close()

    vm = VM("upgrade", ["-drive", f"if=none,id=cd,media=cdrom,readonly=on,file={ISO}",
                        "-device", "ide-cd,drive=cd,bus=ide.2,bootindex=0"])
    try:
        def upgrade_from_cd():
            live_shell(vm)
            vm.cmd("drives", ["C:  BDOS", "RAM disk (from the CD)", "D:  BDOS", "disk0 partition 2"])
            vm.cmd("install 0 /UPGRADE /Y", ["Copying", "bdos is on disk 0"], timeout=300)
        check("an upgrade from the CD", upgrade_from_cd)
    finally:
        vm.close()

    vm = VM("upgraded", [])
    try:
        def upgraded():
            live_shell(vm)
            vm.cmd("type \\kept.txt", "kept")                  # your files stay
            vm.cmd("hostname", "LIVEBOX")                       # and settings
        check("after the upgrade, files and settings are still there", upgraded)
    finally:
        vm.close()

# the Install bdos window, clicked through like a person would
if suite("gui"):
    subprocess.run(["truncate", "-s", "0", disk], check=True)
    subprocess.run(["truncate", "-s", "3G", disk], check=True)
    vm = VM("gui", ["-cdrom", ISO])
    try:
        def gui():
            live_shell(vm)
            vm.cmd("hostname guibox")
            wins, _ = vm.windows()
            shell = [v for k, v in wins.items() if k != "Program Manager"][0]
            vm.send("install\r")
            time.sleep(2)
            vm.point(shell[0] + 30, shell[1] + 8)    # the shell's title: back to it
            time.sleep(0.3)
            vm.click()
            wins, _ = vm.windows()
            assert "Install bdos" in wins, wins
            x, y, w, h = wins["Install bdos"]
            vm.point(x + w // 2, y + 8)              # and the installer's
            time.sleep(0.3)
            vm.click()
            time.sleep(0.5)
            cx, cy = x + 4, y + 24                 # the inside of the window
            def next_():
                vm.point(cx + 560 - 2 * 96 - 12 + 40, cy + 420 - 40 + 14)
                time.sleep(0.3)
                vm.click()
                time.sleep(1)
            vm.screenshot("iso-10-welcome")
            next_()
            vm.point(cx + 60, cy + 44 + 22 + 10)    # the first disk in the list
            time.sleep(0.3)
            vm.click()
            time.sleep(0.5)
            vm.screenshot("iso-11-disk")
            next_()
            vm.screenshot("iso-12-network")
            next_()
            vm.screenshot("iso-13-time")
            next_()
            vm.screenshot("iso-14-startup")
            next_()
            vm.screenshot("iso-15-summary")
            next_()                                  # Install
            time.sleep(2)
            vm.screenshot("iso-16-installing")
            time.sleep(90)                           # copying C: across
            vm.screenshot("iso-17-done")
        check("the Install bdos window, page by page", gui)
    finally:
        vm.close()

    vm = VM("gui2", [])
    try:
        def gui_installed():
            live_shell(vm)
            vm.cmd("drives", "disk0 partition 2")
            vm.cmd("hostname", "GUIBOX")
            vm.cmd("control list", ["net.ntp=pool.ntp.org", "net.udp=1"])
            vm.screenshot("iso-18-installed")
        check("what the window installed boots, with what it was told", gui_installed)
    finally:
        vm.close()

# the way Proxmox makes a VM: the disk on "VirtIO SCSI single", VirtIO network
if suite("proxmox"):
    target = os.path.join(scratch(), "scsi.img")
    subprocess.run(["truncate", "-s", "2G", target], check=True)
    disk = os.path.join(scratch(), "dummy.img")      # VM() always adds a SATA disk
    subprocess.run(["truncate", "-s", "1M", disk], check=True)
    pve = ["-device", "virtio-scsi-pci,id=scsihw0",
           "-drive", f"if=none,id=drive-scsi0,format=raw,file={target}",
           "-device", "scsi-hd,bus=scsihw0.0,channel=0,scsi-id=0,lun=0,drive=drive-scsi0,bootindex=1",
           "-nic", "none", "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0"]
    vm = VM("pve", pve + ["-drive", f"if=none,id=cd,media=cdrom,readonly=on,file={ISO}",
                          "-device", "ide-cd,drive=cd,bus=ide.2,bootindex=0"])
    try:
        def pve_install():
            live_shell(vm)
            out = vm.cmd("fdisk", ["sda", "2.0 GB", "QEMU"])
            n = re.search(r"\n\s+(\d+)\s+sda", out).group(1)
            vm.cmd(f"install {n} /Y", ["Copying", f"bdos is on disk {n}"], timeout=300)
        check("installing onto a VirtIO SCSI disk", pve_install)
    finally:
        vm.close()
    vm = VM("pve2", pve)
    try:
        def pve_boot():
            live_shell(vm)
            vm.cmd("drives", "sda partition 2")
            vm.cmd("ver", "bdos version 0.12")
            vm.cmd("echo on scsi > \\scsi.txt")
            vm.cmd("type \\scsi.txt", "on scsi")
            vm.cmd("libctest", "libctest: all passed", timeout=60)
        check("it boots from the VirtIO SCSI disk", pve_boot)
    finally:
        vm.close()

# Proxmox's default machine, i440fx, with the disk on its IDE controller
if suite("ide"):
    target = os.path.join(scratch(), "ide.img")
    subprocess.run(["truncate", "-s", "2G", target], check=True)
    disk = target
    vm = VM("ide", ["-machine", "pc", "-cdrom", ISO])
    try:
        def ide_install():
            live_shell(vm)
            out = vm.cmd("fdisk", ["ide0", "2.0 GB", "QEMU HARDDISK"])
            n = re.search(r"\n\s+(\d+)\s+ide0", out).group(1)
            vm.cmd(f"install {n} /Y", ["Copying", f"bdos is on disk {n}"], timeout=300)
        check("installing onto an IDE disk (i440fx)", ide_install)
    finally:
        vm.close()
    vm = VM("ide2", ["-machine", "pc"])
    try:
        def ide_boot():
            live_shell(vm)
            vm.cmd("drives", "ide0 partition 2")
            vm.cmd("libctest", "libctest: all passed", timeout=60)
        check("it boots from the IDE disk", ide_boot)
    finally:
        vm.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
