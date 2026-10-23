#!/usr/bin/env python3
"""bdos on hardware QEMU can pretend to have: HD Audio (recorded to a WAV
file and listened to here), USB keyboard and tablet on xHCI, and an NVMe disk.

Usage: hwtest.py <bdos.img> <outdir> [suite ...]
"""
import os, struct, subprocess, sys, time

ONLY_ARGS = sys.argv[3:]
sys.argv = [sys.argv[0], sys.argv[1], sys.argv[2]] + ONLY_ARGS
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])     # VM, check(), suite()

WAV = os.path.join(tmp, "out.wav")
NVME = os.path.join(tmp, "nvme.img")
subprocess.run(["truncate", "-s", "64M", NVME], check=True)

extra = [
    "-audiodev", f"wav,id=snd0,path={WAV}",
    "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0",
    "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,bus=xhci.0", "-device", "usb-tablet,bus=xhci.0",
]


def windows_of(path):
    """[(rms, Hz)] for each quarter second of the recording"""
    raw = open(path, "rb").read()
    rate = struct.unpack("<I", raw[24:28])[0] or 44100
    data = raw[44:]
    n = len(data) // 4
    left = struct.unpack(f"<{n * 2}h", data[:n * 4])[0::2]
    win = rate // 4
    res = []
    for start in range(0, n - win + 1, win):
        seg = left[start:start + win]
        rms = (sum(v * v for v in seg) / len(seg)) ** 0.5
        crossings = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
        res.append((rms, crossings * rate // win))
    return res


def tone_in_wav(path):
    """(rms, Hz) of the loudest stretch of the recording"""
    raw = open(path, "rb").read()
    rate = struct.unpack("<I", raw[24:28])[0] or 44100
    data = raw[44:]
    n = len(data) // 4
    left = struct.unpack(f"<{n * 2}h", data[:n * 4])[0::2]
    best = (0, 0)
    win = rate // 4
    for start in range(0, max(1, n - win), win):
        seg = left[start:start + win]
        if not seg:
            break
        rms = (sum(v * v for v in seg) / len(seg)) ** 0.5
        if rms > best[0]:
            crossings = sum(1 for a, b in zip(seg, seg[1:]) if a < 0 <= b)
            best = (rms, crossings * rate // win)
    return best


vm = VM("hw", extra)
try:
    vm.wait_for("C:\\>", 90)
    passed += 1

    def sound():
        vm.cmd("sound", "HD Audio")
        vm.cmd("play \\bdos\\sounds\\tone.wav", "Playing tone.wav: 22050 Hz", timeout=20)
        time.sleep(0.5)
        rms, hz = tone_in_wav(WAV)
        assert rms > 2000 and 400 < hz < 480, f"recording: rms {rms:.0f}, {hz} Hz"
    if suite("sound"):
        check("HD Audio plays a WAV", sound)

    def dos_sound():
        before = len(windows_of(WAV))
        vm.cmd("cd \\dosapps")
        vm.cmd("sbtest", ["DSP version 4.5", "IRQ 7 came", "OPL2 found", "Sound test done"], timeout=40)
        vm.cmd("cd \\")
        time.sleep(0.5)
        w = windows_of(WAV)[before:]
        loud = [hz for rms, hz in w if rms > 1500 and 400 < hz < 480]
        assert len(loud) >= 3, f"recording after SBTEST: {[(int(r), h) for r, h in w]}"
    if suite("sound"):
        check("a DOS program on the Sound Blaster and OPL2", dos_sound)

    def usb():
        out = vm.cmd("usb", ["keyboard", "tablet"])
        vm.keys(*"echo", "spc", *"typed", "spc", *"on", "spc", *"usb", "ret")
        time.sleep(1)
        assert "typed on usb" in vm.text(), vm.text()[-500:]
        out = vm.cmd("usb")
        import re
        kb = int(re.search(r"keyboard\s+slot \d+, (\d+) reports", out).group(1))
        assert kb > 10, out
        vm.point(512, 192)                      # half way across, a quarter down
        time.sleep(0.5)
        m = re.search(r"tablet\s+slot \d+, (\d+) reports, at (\d+),(\d+)", vm.cmd("usb"))
        assert m and int(m.group(1)) > 0, "no tablet reports"
        x, y = int(m.group(2)), int(m.group(3))
        assert abs(x - 32768) < 400 and abs(y - 16384) < 400, (x, y)
    if suite("usb"):
        check("USB keyboard and tablet on xHCI", usb)

    def native_sound():
        vm.cmd("tune", "Done.", timeout=30)
    if suite("sound"):
        check("a native program plays a tune", native_sound)
finally:
    vm.close()

# a machine whose only disk is NVMe: bdos boots from it
if suite("nvme"):
    shutil.copy(IMG, NVME)
    disk = os.path.join(tmp, "blank.img")      # the SATA disk VM() always adds: empty
    subprocess.run(["truncate", "-s", "1M", disk], check=True)
    vm = VM("nv", ["-device", "nvme,serial=bdos0001,drive=nv0",
                   "-drive", f"if=none,id=nv0,format=raw,file={NVME}"])
    try:
        def nvme_boot():
            vm.wait_for("C:\\>", 90)
            vm.cmd("drives", "nvme0 partition")
            vm.cmd("echo written to NVMe > C:\\NV.TXT")
            vm.cmd("type C:\\NV.TXT", "written to NVMe")
            vm.cmd("copy C:\\BIN\\TCC.EXE C:\\TCC2.EXE", "1 file(s) copied")
            vm.cmd("dir C:\\TCC2.EXE", "TCC2.EXE")
            vm.cmd("libctest", "libctest: all passed", timeout=60)
        check("booting from NVMe", nvme_boot)
    finally:
        vm.close()

# Proxmox's favourite: VirtIO disk and network, the modern (1.0) interface only
if suite("virtio"):
    VD = os.path.join(tmp, "vd.img")
    shutil.copy(IMG, VD)
    disk = os.path.join(tmp, "blank.img")
    subprocess.run(["truncate", "-s", "1M", disk], check=True)
    vm = VM("vio", ["-nic", "none",
                    "-drive", f"if=none,id=vd0,format=raw,file={VD}",
                    "-device", "virtio-blk-pci,drive=vd0,disable-legacy=on",
                    "-netdev", "user,id=n0", "-device", "virtio-net-pci,netdev=n0,disable-legacy=on"])
    try:
        def virtio():
            vm.wait_for("C:\\>", 90)
            vm.cmd("drives", "vda partition")
            vm.cmd("fdisk", "VirtIO disk")
            vm.cmd("echo written to VirtIO > C:\\VD.TXT")
            vm.cmd("type C:\\VD.TXT", "written to VirtIO")
            vm.cmd("copy C:\\BIN\\TCC.EXE C:\\TCC2.EXE", "1 file(s) copied")
            vm.cmd("libctest", "libctest: all passed", timeout=60)
            for _ in range(20):
                out = vm.cmd("ipconfig")
                if "10.0.2.15" in out:
                    break
                time.sleep(1)
            assert "10.0.2.15" in out, out
            vm.cmd("ipconfig", "virtio")
            vm.cmd("ping 10.0.2.2", "Reply from 10.0.2.2", timeout=30)
        check("VirtIO disk and network", virtio)
    finally:
        vm.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
