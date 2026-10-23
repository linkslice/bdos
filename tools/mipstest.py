#!/usr/bin/env python3
"""bdos on MIPS64: QEMU's Fuloong 2E (Loongson 2E), the nearest thing QEMU
has to a Lemote Yeeloong. Boots the kernel with C: as an initrd, on the
serial port, and runs the portable parts: the shell, BASIC, C, DOS programs.

Usage: mipstest.py <KERNEL.SYS> <ROOTFS.IMG> <outdir>
"""
import os, re, socket, subprocess, sys, tempfile, time

KERNEL, ROOTFS, OUT = sys.argv[1:4]
os.makedirs(OUT, exist_ok=True)
tmp = tempfile.mkdtemp()
sock = os.path.join(tmp, "ser")
proc = subprocess.Popen(["qemu-system-mips64el", "-M", "fuloong2e", "-m", "256", "-kernel", KERNEL,
                         "-initrd", ROOTFS, "-display", "none", "-serial", f"unix:{sock},server=on,wait=off"])
for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sock)
        break
    except OSError:
        time.sleep(0.1)
s.settimeout(0.1)
log = b""
PROMPT = re.compile(r"\n[A-Z]:\\[^\n<]*>$")
passed = failed = 0


def read_until(pred, timeout):
    global log
    end = time.time() + timeout
    start = len(log)
    while time.time() < end:
        try:
            d = s.recv(65536)
            if d:
                log += d
        except socket.timeout:
            pass
        text = log[start:].decode(errors="replace").replace("\r", "")
        if pred(text):
            return text
    raise AssertionError("timed out; got:\n" + log[start:].decode(errors="replace")[-1500:])


def cmd(line, expect=(), timeout=30):
    global log
    start = len(log)
    s.sendall(line.encode() + b"\r")
    out = read_until(lambda t: PROMPT.search(t) and t.count("\n") >= 1, timeout)
    for e in ([expect] if isinstance(expect, str) else expect):
        assert e in out, f"{line!r}: expected {e!r} in:\n{out}"
    return out


def check(name, fn):
    global passed, failed
    try:
        fn()
        passed += 1
        print(f"ok    {name}")
    except AssertionError as e:
        failed += 1
        print(f"FAIL  {name}\n{e}\n")


try:
    def boot():
        out = read_until(lambda t: "C:\\>" in t, 120)
        assert "(mips64)" in out, out
    check("boot to C:\\> on MIPS64", boot)
    check("VER and the CPU", lambda: cmd("ver", "bdos version"))
    check("DIR", lambda: cmd("dir \\", ["BDOS", "DOSAPPS"]))
    check("files on the RAM disk", lambda: (cmd("echo made on a loongson > C:\\MIPS.TXT"),
                                             cmd("type C:\\MIPS.TXT", "made on a loongson")))
    check("BASIC", lambda: cmd("\\demos\\recurse", ["10! = 3628800", "fib(20) = 6765", "sqrt(2) = 1.41421"], timeout=60))
    check("C, interpreted", lambda: cmd("\\demos\\sieve.c", "168", timeout=60))
    check("a DOS program in the 8086 emulator", lambda: cmd("\\dosapps\\hello one two", ["Hello from 16-bit DOS on bdos!", "[ one two]"]))
    check("a DOS program doing sums", lambda: cmd("\\dosapps\\primes", "1229 primes below 10000", timeout=120))
    def clock():
        a = re.search(r"(\d+):(\d+):(\d+)", cmd("time /t")).groups()
        time.sleep(2.2)
        b = re.search(r"(\d+):(\d+):(\d+)", cmd("time /t")).groups()
        assert a != b, (a, b)
    check("the clock", clock)
    check("tasks: START runs one alongside", lambda: (cmd("start \\demos\\spin.c 1", "Started task"), time.sleep(2), cmd("tasks", "SPIN")))
finally:
    open(os.path.join(OUT, "serial.log"), "wb").write(log)
    proc.kill()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
