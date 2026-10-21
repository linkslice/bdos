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
        self.qmp_path = os.path.join(tmp, name + ".qmp")
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
            "-qmp", f"unix:{self.qmp_path},server=on,wait=off",
            *extra,
        ])
        self.ser = self._connect(self.ser_path)
        self.monsock = self._connect(self.mon_path)
        self.qmp = self._connect(self.qmp_path)
        self.qmp.settimeout(5)
        self._qmp_read()                        # greeting
        self._qmp({"execute": "qmp_capabilities"})
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
            self.monsock.sendall(f"sendkey {k}\n".encode())
            time.sleep(0.08)

    # ---- QMP: absolute pointer events, the way a host cursor arrives
    def _qmp_read(self):
        buf = b""
        while not buf.endswith(b"\n"):
            buf += self.qmp.recv(65536)
        return buf

    def _qmp(self, msg):
        import json
        self.qmp.sendall(json.dumps(msg).encode() + b"\n")
        while b'"return"' not in self._qmp_read():
            pass                                # skip async events

    def _input(self, *events):
        self._qmp({"execute": "input-send-event", "arguments": {"events": list(events)}})
        time.sleep(0.08)

    def point(self, x, y):
        vx = int((x + 0.5) * 32767 / 1024)
        vy = int((y + 0.5) * 32767 / 768)
        self._input({"type": "abs", "data": {"axis": "x", "value": vx}},
                    {"type": "abs", "data": {"axis": "y", "value": vy}})

    def button(self, down):
        self._input({"type": "btn", "data": {"down": bool(down), "button": "left"}})

    # ---- the monitor
    def mon(self, c):
        self.monsock.sendall((c + "\n").encode())
        time.sleep(0.15)

    def windows(self):
        """{title: (x, y, w, h)} and the pointer, from DESK$ and MOUSE$"""
        out = self.cmd("type DESK$")
        wins = {}
        for m in re.finditer(r'window (\d+) (-?\d+) (-?\d+) (\d+) (\d+)( focus)? "([^"]*)"', out):
            wins[m.group(7)] = tuple(map(int, m.groups()[1:5]))
        mp = re.findall(r"\n(\d+) (\d+) (\d+)", self.cmd("type MOUSE$"))[-1]
        return wins, (int(mp[0]), int(mp[1]))

    def move_to(self, x, y):
        self.point(x, y)
        time.sleep(0.3)
        _, here = self.windows()
        assert here == (x, y), f"pointer at {here}, wanted {(x, y)}"

    def click(self):
        self.button(1)
        self.button(0)
        time.sleep(0.5)

    def drag(self, x0, y0, x1, y1, steps=4):
        self.move_to(x0, y0)
        self.button(1)
        for i in range(1, steps + 1):
            self.point(x0 + (x1 - x0) * i // steps, y0 + (y1 - y0) * i // steps)
        self.button(0)
        time.sleep(0.5)

    def double_click(self):
        for b in (1, 0, 1, 0):
            self.button(b)
        time.sleep(0.6)

    def screenshot(self, name):
        path = os.path.join(tmp, name + ".ppm")
        if os.path.exists(path):
            os.remove(path)
        self.monsock.sendall(f"screendump {path}\n".encode())
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
    assert "bdos 0.9" in out, out
    assert "Welcome to bdos" in out, out
    assert "Bad command" not in out, "AUTOEXEC.BAT hit an error:\n" + out
    assert "panic" not in out, out
    vm.screenshot("01-boot")


def t_basics(vm):
    vm.cmd("ver", "bdos version 0.9 (x86-64)")
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
    vm.cmd("control kbd.rate=20", "kbd.rate=20")
    vm.cmd("cd \\")
    vm.send("reboot\r")
    vm.wait_for("Restarting", 10)
    vm.wait_for("C:\\>", 90)
    vm.cmd("type \\testdir\\keep.txt", "survived a reboot")
    vm.cmd("type \\testdir\\poem.txt", "Roses are red")
    vm.cmd("control", "kbd.rate=20")
    vm.cmd("control", "kbd.rate=20")
    vm.cmd("chkdsk", "No problems found")


def t_ps2(vm):
    start = len(vm.log)
    vm.keys("v", "e", "r", "ret")
    vm.wait_for("bdos version 0.9", 10, start)


def t_basic(vm):
    vm.cmd("cd \\demos")
    vm.cmd("recurse", ["10! = 3628800", "fib(20) = 6765", "Move disc 3 from A to C",
                       "sqrt(2) = 1.41421356", "pi = 3.14159265", "sin(30 deg) = 0.5",
                       "Upper: BDOS", "2^10 = 1024", "17 DIV 5 = 3", "17 MOD 5 = 2"])
    vm.cmd("primes", ["2 3 5 7 11", "199", "46 primes below 200"])
    out = vm.cmd("mandel", "@", timeout=120)
    assert out.count("\n") > 20, out
    vm.cmd("cd \\")
    start = len(vm.log)
    vm.send("hello\r")
    vm.wait_for("name?", 10, start)
    vm.send("Bryan\r")
    out = vm.settle(start)
    assert "Nice to meet you, Bryan." in out and "*****" in out, out
    # immediate mode, program entry, SAVE, errors and SYS
    start = len(vm.log)
    vm.send("basic\r")
    vm.wait_for(">", 10, start)
    for l in ['10 FOR I=1 TO 3', '20 PRINT "line ";I', '30 NEXT', 'RUN']:
        vm.send(l + "\r")
        time.sleep(0.1)
    vm.wait_for("line 3", 10, start)
    for l in ['LIST', 'SAVE "\\testdir\\count.bas"', 'PRINT 6*7', 'PRINT 1/0',
              'SYS "OS_Time" TO t : PRINT t > 0', 'PRINT LEFT$("bdos rocks", 4); MID$("abcdef", 3, 2)',
              'QUIT']:
        vm.send(l + "\r")
        time.sleep(0.2)
    out = vm.settle(start)
    for want in ["20 PRINT \"line \";I", "42", "Division by zero", "-1", "bdoscd"]:
        assert want in out, f"{want!r} missing:\n{out}"
    vm.cmd("type \\testdir\\count.bas", "30 NEXT")
    vm.cmd("count", "Bad command")                     # not on PATH
    vm.cmd("\\testdir\\count", "line 3")              # by path
    vm.cmd("echo PRINT 2+ > bad.bas")
    vm.cmd("bad", "Syntax error at line 1")
    vm.cmd("del bad.bas")


def t_c(vm):
    vm.cmd("cd \\")
    vm.cmd("md \\testdir")
    vm.cmd("hello.c world", ["Hello from C on bdos!", "5 squared is 25", "You said: world"])
    vm.cmd("\\bdos\\tests\\ctest", "ALL OK", absent="FAIL", timeout=40)
    vm.cmd("\\bdos\\tests\\cerr", ["before", ":7: null pointer used"], absent="after")
    vm.cmd("cd \\demos")
    vm.cmd("sieve", ["    2    3    5    7", "997", "168 primes below 1000"], timeout=40)
    vm.cmd("list", ["Sophie    29", "Grace     85", "average age 52.2"])
    vm.cmd("draw.c", "start the desktop with WIN first")
    vm.cmd("help sys", ["Win_Open t$", "Desk_Color", "OS_Sleep"])
    # edit a program and run it again: run-from-source
    vm.cmd("echo int main(void) { int x = 6; printf(\"%d\\n\", x * 7); return 3; } > \\testdir\\t.c")
    vm.cmd("\\testdir\\t", "42")
    vm.cmd("echo %ERRORLEVEL%", "3")
    vm.cmd("echo int main() { return y; } > \\testdir\\t.c")
    vm.cmd("\\testdir\\t", ":1: 'y' undeclared")
    vm.cmd("cd \\")
    vm.cmd("cls")
    vm.cmd("type hello.c", "int main")
    vm.cmd("hello.c Bryan", "You said: Bryan")
    vm.cmd("\\demos\\list", "average age")
    vm.screenshot("08-c")


def t_control(vm):
    vm.cmd("cd \\")
    vm.cmd("control", ["mouse.speed=5", "desktop.scheme=Default", "clock.24h=1"])
    vm.cmd("date 2026-07-04")
    vm.cmd("date /t", "Sat 2026-07-04")
    vm.cmd("time 9:30 pm")
    vm.cmd("time /t", "21:30")
    vm.cmd("control clock.24h=0", "clock.24h=0")
    vm.cmd("time /t", ["9:30", "pm"])
    vm.cmd("control clock.24h=1")
    vm.cmd("date 2026-02-30", "Invalid date")
    vm.cmd("time 25:00", "Invalid time")
    vm.cmd("type \\bdos\\settings.ini", "clock.24h=1")
    # programs see the same settings through SYS
    vm.cmd("md \\testdir")
    vm.cmd('echo int main(void) { os_set_setting("mouse.speed", 7); printf("speed %s\\n", os_setting("mouse.speed")); return 0; } > \\testdir\\set.c')
    vm.cmd("\\testdir\\set", "speed 7")
    vm.cmd("echo SYS \"OS_Setting\", \"mouse.speed\" TO s$ : PRINT \"basic sees \"; s$ > \\testdir\\set.bas")
    vm.cmd("\\testdir\\set.bas", "basic sees 7")
    vm.cmd("control mouse.speed=5")

    # on the desktop
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.send("\r")
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    vm.cmd("control desktop.color=3a3a6e")
    vm.cmd("type DESK$", "color #3a3a6e")
    # drag the shell's bottom-right corner: it grows by whole characters
    wins, _ = vm.windows()
    x, y, w, h = wins["Shell"]
    vm.drag(x + w - 2, y + h - 2, x + w + 158, y + h + 102)
    wins, _ = vm.windows()
    assert wins["Shell"] == (x, y, w + 160, h + 96), wins["Shell"]
    vm.cmd("dir /w", "bytes free")
    # the left edge shrinks it from the other side
    x, y, w, h = wins["Shell"]
    vm.drag(x + 1, y + h // 2, x + 201, y + h // 2)
    wins, _ = vm.windows()
    assert wins["Shell"] == (x + 200, y, w - 200, h), wins["Shell"]
    # double-click the title bar: maximized; the button restores it
    x, y, w, h = wins["Shell"]
    vm.move_to(x + w // 2, y + 12)
    vm.double_click()
    wins, _ = vm.windows()
    assert wins["Shell"][:3] == (0, 0, 1024), wins["Shell"]
    mx, my, mw, mh = wins["Shell"]
    vm.move_to(mx + mw - 4 - 36 + 8, my + 4 + 8)
    vm.click()
    wins, _ = vm.windows()
    assert wins["Shell"] == (x, y, w, h), wins["Shell"]
    # the Control Panel, and an applet driven from the keyboard
    vm.send("control\r")
    time.sleep(2)
    vm.screenshot("09-control-panel")
    vm.keys("esc")
    time.sleep(0.5)
    vm.send("control date\r")
    time.sleep(2)
    vm.send("\t\t15\r")                         # Day: 15, Enter = OK
    time.sleep(1)
    vm.cmd("date /t", "2026-07-15")
    vm.cmd("control desktop.scheme=Ocean", "Ocean")
    vm.send("control colours\r")
    time.sleep(1.5)
    vm.screenshot("10-colours")
    vm.keys("esc")
    time.sleep(0.5)
    vm.cmd("control desktop.scheme=Default")
    vm.cmd("control desktop.color=2f6f73")
    # Program Manager's close box asks before leaving, and the question shows
    wins, _ = vm.windows()
    x, y, w, h = wins["Program Manager"]
    vm.move_to(x + w - 4 - 9, y + 4 + 8)
    vm.click()
    vm.screenshot("11-exit-dialog")
    shot = Image.open(os.path.join(OUT, "11-exit-dialog.png")).convert("RGB")
    raw = shot.crop((302, 241, 722, 329)).tobytes()
    dark = sum(1 for i in range(0, len(raw), 3) if max(raw[i:i + 3]) < 40)
    assert dark > 200, f"the Exit Desktop dialog looks empty ({dark} dark pixels)"
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version")


def t_dos(vm):
    vm.cmd("cd \\dosapps")
    vm.cmd("hello one two", ["Hello from 16-bit DOS on bdos!", "Command tail:[ one two]"])
    vm.cmd("primes", "1229 primes below 10000", timeout=60)
    vm.cmd("fileio", ["File written and read back by a DOS program.", "DOSTEST.TXT"])
    vm.cmd("echo %ERRORLEVEL%", "7")
    vm.cmd("type dostest.txt", "File written and read back")      # visible to bdos
    vm.cmd("helloexe", "Hello from an .EXE with relocations!")
    vm.cmd("echo %ERRORLEVEL%", "5")
    start = len(vm.log)
    vm.send("askname\r")
    vm.wait_for("your name?", 10, start)
    vm.send("Bryan\r")
    out = vm.settle(start)
    assert "Pleased to meet you, Bryan!" in out, out
    vm.cmd("box", "Written straight to B800:0000")
    vm.screenshot("07-dos-box")
    vm.cmd("cls")
    vm.cmd("cd \\")


def t_desktop(vm):
    vm.cmd("cd \\")
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.screenshot("04-desktop")
    vm.send("\r")                                   # Program Manager: launch Shell
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    vm.cmd("ver", "bdos version 0.9")
    vm.cmd("echo color #3a3a6e > DESK$")
    vm.cmd("type DESK$", ["desktop level 0", "color #3a3a6e"])
    vm.cmd("type MOUSE$", " ")
    vm.cmd("\\demos\\sunset", "desktop is now #3a1d2a", timeout=30)   # C changing its desktop
    vm.cmd("type DESK$", "color #3a1d2a")
    vm.cmd("echo color #3a3a6e > DESK$")
    vm.cmd("echo rect 300 10 200 60 #ffcc00 > DRAW$")   # draw on our own window
    vm.cmd("echo text 310 30 #000000 drawn with DRAW$ > DRAW$")
    # a desktop inside a window
    vm.cmd("echo run WIN > DESK$")
    time.sleep(3)
    start = len(vm.log)
    vm.send("\r")                                   # the nested Program Manager has focus
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    vm.cmd("type DESK$", ["desktop level 1", "color #5b3f72"])
    vm.screenshot("06-nested")
    start = len(vm.log)
    vm.send("echo exit > DESK$\r")                   # leave the nested desktop
    vm.wait_for("press any key", 10, start)
    vm.send(" ")
    time.sleep(1)
    # a BASIC program's own window and the same in C, started from C
    vm.send("\\bdos\\tests\\twowin\r")
    time.sleep(5)
    vm.screenshot("05-windows")
    # leave the real desktop: Alt+X, then confirm
    start = len(vm.log)
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version 0.9")              # back at the full-screen prompt


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
                     ("chkdsk", t_chkdsk), ("ps2", t_ps2), ("basic", t_basic), ("c", t_c), ("control", t_control), ("dos", t_dos),
                     ("desktop", t_desktop), ("screens", t_screens),
                     ("persist", t_persist)]:
        if suite(name):
            check(name, fn, vm)
finally:
    vm.close()

# Real PCs have no vmmouse: the plain PS/2 (relative) mouse must still work.
def t_ps2mouse(vm2):
    vm2.wait_for("C:\\>", 90)
    start = len(vm2.log)
    vm2.send("win\r")
    time.sleep(3)
    vm2.send("\r")
    vm2.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    _, (x, y) = vm2.windows()
    vm2.mon("mouse_move 100 -50")
    time.sleep(0.4)
    _, (x2, y2) = vm2.windows()
    assert (x2, y2) == (x + 100, y - 50), f"PS/2 mouse went {(x, y)} -> {(x2, y2)}"


if suite("ps2mouse"):
    disk = os.path.join(tmp, "disk2.img")
    shutil.copy(IMG, disk)
    vm2 = VM("ps2", ["-machine", "vmport=off"])
    try:
        check("ps2mouse", t_ps2mouse, vm2)
    finally:
        vm2.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
