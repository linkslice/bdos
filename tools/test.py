#!/usr/bin/env python3
"""Headless bdos test suite: boots the image in QEMU, types at the prompt
over COM1 (and the emulated PS/2 keyboard), checks the replies and saves
screenshots of the framebuffer.

Usage: test.py <bdos.img> <outdir> [suite ...]
"""
import os, re, socket, subprocess, sys, tempfile, threading, time, shutil
from PIL import Image

IMG, OUT = sys.argv[1], sys.argv[2]
ONLY = set(sys.argv[3:])
os.makedirs(OUT, exist_ok=True)
tmp = tempfile.mkdtemp()
disk = os.path.join(tmp, "disk.img")
shutil.copy(IMG, disk)          # tests write to C:, keep the build pristine
PROMPT = re.compile(r"\n[A-Z]:\\[^\n<]*>$")
passed = failed = 0


class VM:
    def __init__(self, name="vm", extra=()):
        self.ser_path = os.path.join(tmp, name + ".ser")
        self.mon_path = os.path.join(tmp, name + ".mon")
        vars_copy = os.path.join(tmp, name + ".vars")
        shutil.copy("/usr/share/OVMF/OVMF_VARS_4M.fd", vars_copy)
        self.proc = subprocess.Popen([
            "qemu-system-x86_64", "-machine", "q35", "-m", "256M",
            "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
            "-drive", f"if=pflash,format=raw,file={vars_copy}",
            "-drive", f"format=raw,file={disk}",
            "-vga", "std", "-display", "none",
            "-serial", f"unix:{self.ser_path},server=on,wait=off",
            "-monitor", f"unix:{self.mon_path},server=on,wait=off",
            *extra,
        ])
        self.ser = self._connect(self.ser_path)
        self.mon = self._connect(self.mon_path)
        self.log = b""
        self.lock = threading.Lock()
        self.alive = True
        # Drain COM1 continuously, like a real terminal would.
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        self.ser.settimeout(0.1)
        while self.alive:
            try:
                data = self.ser.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not data:
                break
            with self.lock:
                self.log += data
                self.last_rx = time.time()

    @staticmethod
    def _connect(path):
        for _ in range(100):
            try:
                s = socket.socket(socket.AF_UNIX)
                s.connect(path)
                return s
            except OSError:
                time.sleep(0.1)
        raise SystemExit("could not connect to " + path)

    def text(self, start=0):
        return self.log[start:].decode(errors="replace").replace("\r", "")

    def wait_for(self, token, timeout=60, start=None):
        if start is None:
            start = len(self.log)
        end = time.time() + timeout
        while time.time() < end:
            if token in self.text(start):
                return self.text(start)
            time.sleep(0.05)
        raise AssertionError(f"timed out waiting for {token!r}; got:\n{self.text(start)[-2000:]}")

    def settle(self, start, timeout=20):
        """Read until the line goes quiet with a prompt at the end."""
        end = time.time() + timeout
        self.last_rx = time.time()
        while time.time() < end:
            time.sleep(0.05)
            quiet = time.time() - self.last_rx > 0.25
            if quiet and PROMPT.search(self.text(start)):
                return self.text(start)
        raise AssertionError("no prompt came back:\n" + self.text(start)[-2000:])

    def send(self, data):
        self.ser.sendall(data if isinstance(data, bytes) else data.encode())

    def cmd(self, line, expect=None, absent=None, timeout=20):
        start = len(self.log)
        self.send(line + "\r")
        out = self.settle(start, timeout)
        for e in ([expect] if isinstance(expect, str) else expect or []):
            if e not in out:
                raise AssertionError(f"{line!r}: expected {e!r} in:\n{out}")
        for a in ([absent] if isinstance(absent, str) else absent or []):
            if a in out:
                raise AssertionError(f"{line!r}: did not expect {a!r} in:\n{out}")
        return out

    def keys(self, *names):
        for k in names:
            self.mon.sendall(f"sendkey {k}\n".encode())
            time.sleep(0.08)

    def screenshot(self, name):
        path = os.path.join(tmp, name + ".ppm")
        if os.path.exists(path):
            os.remove(path)
        self.mon.sendall(f"screendump {path}\n".encode())
        for _ in range(100):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                time.sleep(0.3)
                break
            time.sleep(0.1)
        Image.open(path).save(os.path.join(OUT, name + ".png"))

    def close(self):
        self.alive = False
        with open(os.path.join(OUT, "serial.log"), "ab") as f:
            f.write(self.log)
        self.proc.kill()
        self.proc.wait()


def check(name, fn, *args):
    global passed, failed
    try:
        fn(*args)
        passed += 1
        print(f"ok    {name}")
    except AssertionError as e:
        failed += 1
        print(f"FAIL  {name}\n{e}\n")


def suite(name):
    return not ONLY or name in ONLY


# ---- the tests ------------------------------------------------------------

def t_boot(vm):
    out = vm.wait_for("C:\\>", 90)
    assert "bdos 0.2" in out, out
    assert "Welcome to bdos" in out, out
    assert "Bad command" not in out, "AUTOEXEC.BAT hit an error:\n" + out
    assert "panic" not in out, out
    vm.screenshot("01-boot")


def t_basics(vm):
    vm.cmd("ver", "bdos version 0.2 (x86-64)")
    vm.cmd("dir", ["Volume in drive C is BDOS", "Read Me First.txt", "<DIR>", "bytes free"])
    vm.cmd("dir/w", "[GAMES]")
    vm.cmd("ls", "GAMES/")
    vm.cmd("ls -l", "AUTOEXEC.BAT")
    vm.cmd("type \"read me first.txt\"", "Bryan Disk Operating System")
    vm.cmd("type README~1.TXT", "Bryan Disk Operating System")   # 8.3 alias
    vm.cmd("mem", ["Total RAM", "Kernel heap"])
    vm.cmd("tasks", ["COMMAND", "<- you"])
    vm.cmd("hwinfo", ["SATA (AHCI) controller", "disk0"])
    vm.cmd("drives", ["C:", "bdosfs on disk0 partition 2"])
    vm.cmd("frobnicate", "Bad command or file name")
    vm.cmd("LS", "Bad command or file name")      # aliases are case-sensitive
    vm.cmd("d:", "Invalid drive specification")
    vm.cmd("set", "PATH=C:\\BDOS")


def t_dirs(vm):
    vm.cmd("md TESTDIR")
    vm.cmd("cd testdir")
    vm.cmd("cd", "C:\\TESTDIR")
    vm.cmd("pwd", "\\TESTDIR")
    vm.cmd("mkdir -p a/b/c")
    vm.cmd("tree", ["\\---a", "\\---b", "\\---c"])
    vm.cmd("cd \\")
    vm.cmd("cd /games", "C:\\>")          # under CD, '/games' is a switch
    vm.cmd("cd \\games", "C:\\GAMES>")
    vm.cmd("cd..", "C:\\>")
    vm.cmd("rd TESTDIR", "Directory not empty")


def t_files(vm):
    vm.cmd("cd \\testdir")
    vm.cmd("echo hello world > a.txt")
    vm.cmd("type a.txt", "hello world")
    vm.cmd("echo second line >> a.txt")
    out = vm.cmd("type a.txt", ["hello world", "second line"])
    vm.cmd("copy a.txt b.txt", "1 file(s) copied")
    vm.cmd("dir *.txt", ["a.txt", "b.txt", "2 file(s)"])
    vm.cmd("ren b.txt \"Long name with spaces.txt\"")
    vm.cmd("dir /b", "Long name with spaces.txt")
    vm.cmd("cp a.txt c.txt")
    vm.cmd("mv c.txt a/moved.txt")
    vm.cmd("ls a", "moved.txt")
    vm.cmd("rm a/moved.txt")
    vm.cmd("ls a", absent="moved.txt")
    vm.cmd("attrib +r a.txt")
    vm.cmd("del a.txt", "Access denied")
    vm.cmd("attrib -r a.txt")
    vm.cmd("touch empty.dat")
    vm.cmd("dir empty.dat", "0  ")
    vm.cmd("copy a.txt nul", "1 file(s) copied")
    vm.cmd("copy a.txt con", "hello world")
    # COPY CON: type lines, end with Ctrl-Z
    start = len(vm.log)
    vm.send("copy con typed.txt\r")
    time.sleep(0.3)
    vm.send("line one\rline two\r\x1a")
    vm.settle(start)
    vm.cmd("type typed.txt", ["line one", "line two"])


def t_pipes(vm):
    vm.cmd("cd \\testdir")
    vm.cmd("echo zebra > s.txt")
    vm.cmd("echo apple >> s.txt")
    vm.cmd("echo mango >> s.txt")
    out = vm.cmd("type s.txt | sort")
    assert out.index("apple") < out.index("mango") < out.index("zebra"), out
    out = vm.cmd("sort /r < s.txt")
    assert out.index("zebra") < out.index("mango") < out.index("apple"), out
    vm.cmd("type s.txt | find \"an\"", "mango", absent="zebra")
    vm.cmd("cat s.txt | grep -v an", ["zebra", "apple"], absent="mango")
    vm.cmd("type s.txt | find /c \"a\"", "3")
    vm.cmd("dir /b | sort > list.txt")
    vm.cmd("type list.txt", ["s.txt", "a.txt"])


def t_batch(vm):
    vm.cmd("cd \\testdir")
    lines = [
        "@echo off",
        "set GREETING=howdy",
        "echo %GREETING% from %0 with %1",
        "if \"%1\"==\"jump\" goto skip",
        "echo not skipped",
        ":skip",
        "if exist a.txt echo a.txt exists",
        "if not exist nope.txt echo nope.txt missing",
        "for %%f in (one two three) do echo item %%f",
        "for %%f in (*.txt) do echo file %%f",
        "frobnicate",
        "if errorlevel 1 echo errorlevel worked",
    ]
    start = len(vm.log)
    vm.send("copy con test.bat\r")
    time.sleep(0.3)
    for l in lines:
        vm.send(l + "\r")
        time.sleep(0.05)
    vm.send("\x1a")
    vm.settle(start)
    out = vm.cmd("test jump", ["howdy from", "with jump", "a.txt exists", "nope.txt missing",
                               "item one", "item three", "file a.txt", "errorlevel worked"],
                 absent="not skipped")
    vm.cmd("test", "not skipped")
    vm.cmd("whereis AUTOEXEC.BAT", "C:\\AUTOEXEC.BAT")   # batch on PATH


def t_edit(vm):
    vm.cmd("cd \\testdir")
    start = len(vm.log)
    vm.send("edit poem.txt\r")
    time.sleep(1.0)
    vm.send("Roses are red\rbdos is new\r")
    time.sleep(0.3)
    vm.send("\x13")          # ^S save
    time.sleep(0.5)
    vm.screenshot("03-edit")
    vm.send("\x11")          # ^Q quit
    vm.settle(start)
    vm.cmd("type poem.txt", ["Roses are red", "bdos is new"])


def t_chkdsk(vm):
    out = vm.cmd("chkdsk", ["Checking drive C:", "bytes total disk space"])
    assert "No problems found" in out, out


def t_persist(vm):
    vm.cmd("cd \\testdir")
    vm.cmd("echo survived a reboot > keep.txt")
    vm.cmd("cd \\")
    vm.send("reboot\r")
    vm.wait_for("Restarting", 10)
    vm.wait_for("C:\\>", 90)
    vm.cmd("type \\testdir\\keep.txt", "survived a reboot")
    vm.cmd("type \\testdir\\poem.txt", "Roses are red")
    vm.cmd("chkdsk", "No problems found")


def t_ps2(vm):
    start = len(vm.log)
    vm.keys("v", "e", "r", "ret")
    vm.wait_for("bdos version 0.2", 10, start)


def t_screens(vm):
    vm.cmd("cls")
    vm.cmd("cd \\")
    vm.cmd("dir")
    vm.cmd("tree /f")
    vm.screenshot("02-session")


vm = VM()
try:
    check("boot to C:\\>", t_boot, vm)
    for name, fn in [("basics", t_basics), ("dirs", t_dirs), ("files", t_files),
                     ("pipes", t_pipes), ("batch", t_batch), ("edit", t_edit),
                     ("chkdsk", t_chkdsk), ("ps2", t_ps2), ("screens", t_screens),
                     ("persist", t_persist)]:
        if suite(name):
            check(name, fn, vm)
finally:
    vm.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
