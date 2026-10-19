#!/usr/bin/env python3
"""Headless smoke test: boot bdos in QEMU, type commands over COM1,
check the replies and save screenshots of the framebuffer.

Usage: test.py <bdos.img> <outdir>
"""
import os, re, socket, subprocess, sys, tempfile, time
from PIL import Image

IMG, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
tmp = tempfile.mkdtemp()
ser_path, mon_path = os.path.join(tmp, "ser"), os.path.join(tmp, "mon")
vars_copy = os.path.join(tmp, "vars.fd")
subprocess.run(["cp", "/usr/share/OVMF/OVMF_VARS_4M.fd", vars_copy], check=True)

qemu = subprocess.Popen([
    "qemu-system-x86_64", "-machine", "q35", "-m", "256M",
    "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
    "-drive", f"if=pflash,format=raw,file={vars_copy}",
    "-drive", f"format=raw,file={IMG}",
    "-vga", "std", "-display", "none",
    "-serial", f"unix:{ser_path},server=on,wait=off",
    "-monitor", f"unix:{mon_path},server=on,wait=off",
])


def connect(path):
    for _ in range(100):
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except OSError:
            time.sleep(0.1)
    raise SystemExit("could not connect to " + path)


ser, mon = connect(ser_path), connect(mon_path)
ser.settimeout(0.2)
log = b""


def read_until(token, timeout=30):
    global log
    end = time.time() + timeout
    start = len(log)
    while time.time() < end:
        try:
            data = ser.recv(4096)
            if data:
                log += data
                if token.encode() in log[start:]:
                    return log[start:].decode(errors="replace")
        except socket.timeout:
            pass
    raise SystemExit(f"timed out waiting for {token!r}; got:\n{log[start:].decode(errors='replace')}")


PROMPT = re.compile(r"\n[A-Z]:\\[^\n<]*>$")


def cmd(line, expect=None):
    global log
    ser.sendall(line.encode() + b"\r")
    start, end = len(log), time.time() + 15
    while time.time() < end:
        try:
            data = ser.recv(4096)
            log += data
        except socket.timeout:
            # quiet line and a prompt at the end: the command finished
            if PROMPT.search(log[start:].decode(errors="replace").replace("\r", "")):
                break
    out = log[start:].decode(errors="replace").replace("\r", "")
    if not PROMPT.search(out):
        raise SystemExit(f"FAIL {line!r}: no prompt came back:\n{out}")
    if expect and expect not in out:
        raise SystemExit(f"FAIL {line!r}: expected {expect!r} in:\n{out}")
    print(f"ok   {line!r}")
    return out


def screenshot(name):
    path = os.path.join(tmp, name + ".ppm")
    mon.sendall(f"screendump {path}\n".encode())
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            time.sleep(0.3)
            break
        time.sleep(0.1)
    Image.open(path).save(os.path.join(OUT, name + ".png"))
    print(f"shot {name}.png")


try:
    boot = read_until("C:\\>", 60)
    assert "bdos 0.1" in boot, boot
    assert "Welcome to bdos" in boot, boot
    assert "Bad command" not in boot, "AUTOEXEC.BAT hit an error:\n" + boot
    print("ok   booted to C:\\>")
    screenshot("01-boot")

    cmd("ver", "bdos version 0.1 (x86-64)")
    cmd("dir", "Read Me First.txt")
    cmd("dir/w", "[GAMES]")
    cmd("ls", "GAMES/")
    cmd("ls -l", "AUTOEXEC.BAT")
    cmd("cd games", None)
    cmd("cd", "C:\\GAMES")
    cmd("pwd", "/GAMES")
    cmd("cd..", None)
    cmd("cd /docs", "C:\\>")         # under CD, '/docs' is a switch: stays put
    cmd("cd \\docs", None)
    cmd("cat Roadmap.txt", "UEFI loader")
    cmd("cat \"../games/lunar lander.bas\"", "Lunar Lander")
    cmd("type \"..\\GAMES\\Lunar Lander.bas\"", "Lunar Lander")
    cmd("cd \\", None)
    cmd("type \"read me first.txt\"", "Bryan Disk Operating System")
    cmd("dir /a", "SYSTEM")
    cmd("dir system", "0 file(s)")   # KERNEL.SYS is hidden
    cmd("dir system /a", "KERNEL.SYS")
    cmd("attrib", "AUTOEXEC.BAT")
    cmd("mem", "Total RAM")
    cmd("date", "Current date is")
    cmd("hello", "Hello from bdos")
    cmd("frobnicate", "Bad command or file name")
    cmd("LS", "Bad command or file name")   # aliases are case-sensitive
    cmd("d:", "Invalid drive specification")
    cmd("win", "isn't built yet")
    cmd("alias", "ls       = DIR")
    cmd("cls")
    cmd("help", "List files")
    cmd("dir")
    screenshot("02-session")

    # Same thing through the emulated PS/2 keyboard instead of COM1.
    start = len(log)
    for key in ["v", "e", "r", "ret"]:
        mon.sendall(f"sendkey {key}\n".encode())
        time.sleep(0.15)
    read_until("bdos version 0.1", 10)
    print("ok   PS/2 keyboard: typed 'ver'")
    print("\nALL PASSED")
finally:
    with open(os.path.join(OUT, "serial.log"), "wb") as f:
        f.write(log)
    qemu.kill()
