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
            "qemu-system-x86_64", "-machine", "q35", "-m", "256M", "-smp", os.environ.get("SMP", "4"),
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

    def button(self, down, which="left"):
        self._input({"type": "btn", "data": {"down": bool(down), "button": which}})

    def wheel(self, clicks):
        """+ up (away from you), - down"""
        b = "wheel-up" if clicks > 0 else "wheel-down"
        for _ in range(abs(clicks)):
            self.button(1, b)
            self.button(0, b)

    def rclick(self):
        self.button(1, "right")
        self.button(0, "right")
        time.sleep(0.5)

    # ---- the monitor
    def mon(self, c):
        self.monsock.sendall((c + "\n").encode())
        time.sleep(0.15)

    def windows(self):
        """{title: (x, y, w, h)} and the pointer, from DESK$ and MOUSE$"""
        out = self.cmd("type DESK$")
        wins = {}
        self.minimized = set()
        self.focus = None
        self.back = {}
        for m in re.finditer(r'window (\d+) (-?\d+) (-?\d+) (\d+) (\d+)((?: [a-z0-9]+)*) "([^"]*)"', out):
            wins[m.group(7)] = tuple(map(int, m.groups()[1:5]))
            b = re.search(r"back (\d+)", m.group(6))
            self.back[m.group(7)] = int(b.group(1)) if b else 0
            if "minimized" in m.group(6):
                self.minimized.add(m.group(7))
            if "focus" in m.group(6):
                self.focus = m.group(7)
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
        import traceback
        tb = traceback.extract_tb(e.__traceback__)
        where = " / ".join(f"{f.name}:{f.lineno}" for f in tb[1:])
        print(f"FAIL  {name}  ({where})\n{e}\n")


def suite(name):
    return not ONLY or name in ONLY


# ---- the tests ------------------------------------------------------------

def t_boot(vm):
    out = vm.wait_for("C:\\>", 90)
    assert "bdos 0.11" in out, out
    assert "Welcome to bdos" in out, out
    assert "Bad command" not in out, "AUTOEXEC.BAT hit an error:\n" + out
    assert "panic" not in out, out
    vm.screenshot("01-boot")


def t_basics(vm):
    vm.cmd("ver", "bdos version 0.11 (x86-64)")
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
    vm.wait_for("bdos version 0.11", 10, start)


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
    vm.cmd("control", ["mouse.speed=5", "desktop.theme=bdos Classic", "clock.24h=1"])
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
    vm.cmd("control desktop.theme=Ocean", "Ocean")
    vm.send("control colours\r")
    time.sleep(1.5)
    vm.screenshot("10-colours")
    vm.keys("esc")
    time.sleep(0.5)
    vm.cmd("control desktop.theme=bdos Classic")
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
    vm.cmd("envpath", ["ENVPATH var: PATH=", "ENVPATH self: C:\\DOSAPPS\\ENVPATH.COM"])
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
    # SUBST: a folder as A:, for installers that want a floppy
    vm.cmd("subst a: c:\\dosapps")
    vm.cmd("subst", "A: => C:\\DOSAPPS")
    vm.cmd("a:")
    vm.cmd("dir hello.com", "HELLO")
    vm.cmd("hello from a", "Command tail:[ from a]")
    vm.cmd("c:")
    vm.cmd("drives", "A:")
    vm.cmd("subst a: /d")
    vm.cmd("dir a:", absent="HELLO")
    vm.cmd("subst c: c:\\bin", "C: is a drive already")


def t_dos2(vm):
    vm.cmd("cd \\dosapps")
    vm.cmd("cpu486", ["GenuineIntel", "CPU486 OK"])
    vm.cmd("fpu", "FPU OK")
    out = vm.cmd("timer", "TIMER: interrupts in 18 ticks:", timeout=30)
    n = int(re.search(r"18 ticks: (\d+)", out).group(1))
    assert 850 <= n <= 1150, f"timer ran at {n}/s"
    vm.cmd("xms", "round trip through extended memory OK")
    # pacing: the same program, held to an AT's speed, then flat out
    vm.cmd("control dos.speed=xt")
    t0 = time.time()
    vm.cmd("primes", "1229 primes", timeout=120)
    slow = time.time() - t0
    vm.cmd("control dos.speed=max")
    t0 = time.time()
    vm.cmd("primes", "1229 primes", timeout=60)
    fast = time.time() - t0
    vm.cmd("control dos.speed=486")
    assert slow > fast * 3, f"XT pace {slow:.1f}s vs flat out {fast:.1f}s"
    vm.cmd("parent", ["Hello from 16-bit DOS on bdos!", "Command tail:[ from mum]",
                      "child exit code 0", "bdos version", "back from COMMAND.COM"])
    # a hooked INT 9 sees make and break codes
    start = len(vm.log)
    vm.send("kbdhook\r")
    vm.wait_for("Esc to stop", 10, start)
    vm.send("a")
    time.sleep(0.5)
    vm.send("\x1b")
    out = vm.settle(start, 15)
    assert "1E 9E" in out and "01 81" in out, out
    # saved states: count to 3, save, count on, go back, carry on
    start = len(vm.log)
    vm.send("counter\r")
    vm.wait_for("q to quit", 10, start)
    for _ in range(3):
        vm.send("a")
        time.sleep(0.3)
    vm.wait_for("count 3", 10, start)
    vm.keys("ctrl-alt-f11")
    time.sleep(1.5)
    for _ in range(2):
        vm.send("a")
        time.sleep(0.3)
    vm.wait_for("count 5", 10, start)
    vm.keys("ctrl-alt-f12")
    time.sleep(1.5)
    vm.send("a")
    vm.wait_for("count 4", 10, start)
    vm.send("q")
    vm.wait_for("COUNTER final 4", 10, start)
    vm.settle(start)
    vm.cmd("dir \\bdos\\states", "COUNTER.BST")
    start = len(vm.log)
    vm.send("resume counter\r")
    time.sleep(2)
    vm.send("q")
    vm.wait_for("COUNTER final 3", 15, start)
    vm.settle(start)
    vm.cmd("cd \\")


def t_dosfull(vm):
    # no desktop: graphics take the whole screen, and give it back
    vm.cmd("cd \\dosapps")
    start = len(vm.log)
    vm.send("mode13\r")
    time.sleep(3)
    vm.screenshot("26-mode13-full")
    vm.keys("spc")
    vm.wait_for("MODE13 done", 15, start)
    time.sleep(0.5)
    vm.cmd("ver", "bdos version")
    vm.screenshot("27-after-full")
    vm.cmd("cd \\")


def t_dosgfx(vm):
    vm.cmd("cd \\dosapps")
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.send("\r")
    vm.wait_for("C:\\", 15, start)
    time.sleep(0.5)
    vm.cmd("cd \\dosapps")
    for prog, shot in (("mode13", "22-mode13"), ("planar", "23-planar"), ("modex", "24-modex")):
        start = len(vm.log)
        vm.send(prog + "\r")
        time.sleep(3)
        vm.screenshot(shot)
        vm.keys("spc")                          # the window has the keys now
        vm.wait_for(prog.upper() + " done", 15, start)
        time.sleep(0.5)
    # the mouse: click in the middle of the window, then the right button ends it
    start = len(vm.log)
    vm.send("mouse\r")
    time.sleep(3)
    wins, _ = (None, None)
    vm.point(420, 280)
    time.sleep(0.4)
    vm.click()
    time.sleep(0.5)
    vm.screenshot("25-mouse")
    vm.button(1, "right")
    vm.button(0, "right")
    vm.wait_for("MOUSE: clicked at", 15, start)
    time.sleep(0.5)
    # quick clicks, the press and release together, while a "game" is busy:
    # its event handler still sees the button down, and so does polling
    start = len(vm.log)
    vm.send("clicks\r")
    time.sleep(4)
    vm.point(420, 280)
    for _ in range(2):
        time.sleep(1.5)
        vm.button(1)                    # (QEMU merges a press and release sent together)
        vm.button(0)
    time.sleep(3)
    vm.button(1, "right")
    vm.button(0, "right")
    vm.wait_for("polling saw", 60, start)
    time.sleep(0.5)
    out = vm.text(start)
    m = re.search(r"down (\d+)\s+of (\d+), polling saw (\d+)", out)   # the line may wrap
    assert m and m.group(1) == m.group(2) == "2" and int(m.group(3)) >= 1, out[-300:]
    # a game's keyboard and mouse: INT 9 sees a key held for 1.5 s as held,
    # and with its range set to 0..319 the mouse is where the pointer is
    wins, _ = vm.windows()
    sx, sy, _, _ = wins["Shell"]
    start = len(vm.log)
    vm.send("keyhold\r")
    time.sleep(3)
    vm.mon("sendkey a 1500")
    time.sleep(2.5)
    vm.point(sx + 4 + 480, sy + 24 + 300)          # 3/4 across, 3/4 down of 640x400
    time.sleep(0.4)
    vm.click()
    vm.keys("esc")
    vm.wait_for("KEYHOLD mouse", 15, start)
    out = vm.settle(start)
    n = int(re.search(r"1E=(\d+)", out).group(1))
    assert 20 <= n <= 35, out
    assert "KEYHOLD mouse: 240,150" in out, out
    assert re.search(r"events [1-9]", out), out          # its event handler ran, and returned
    # the corners of the BIOS and mouse driver: stuffing keys, the display's
    # state, the mouse's settings, a keyboard hook on INT 15h that swallows
    # a key, and the grey arrows as DOS reads them (0, then the scan code)
    # (with the mouse on, a text program is drawn as pixels: its words go to a file)
    start = len(vm.log)
    vm.send("biosapi > bapi.txt\r")
    time.sleep(4)
    vm.keys("a")
    time.sleep(0.3)
    vm.keys("b")
    time.sleep(0.5)
    vm.keys("up")
    time.sleep(2)
    vm.wait_for("C:\\DOSAPPS>", 15, start)
    out = vm.cmd("type bapi.txt")
    assert re.search(r"stuff ok full ok vga ok mouse ok ready hook b arrow 00000048", re.sub(r"\s+", " ", out)), out[-400:]
    vm.cmd("del bapi.txt")
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
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
    vm.cmd("ver", "bdos version 0.11")
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
    vm.cmd("ver", "bdos version 0.11")              # back at the full-screen prompt


def t_win31(vm):
    vm.cmd("cd \\")
    vm.cmd("md \\fmtest")
    vm.cmd("echo one > \\fmtest\\one.txt")
    vm.cmd("echo two > \\fmtest\\two.txt")
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.send("\r")
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    # themes are files, and Hot Dog Stand is one of them
    vm.cmd("dir \\bdos\\themes", ["Hot Dog Stand.THM", "Ocean.THM"])
    vm.cmd("control desktop.theme=Hot Dog Stand", "Hot Dog Stand")
    vm.cmd("type DESK$", "color #ff0000")
    vm.screenshot("12-hot-dog-stand")
    vm.cmd("control desktop.theme=bdos Classic")
    vm.cmd("type DESK$", "color #2f6f73")
    # File Manager, opened on a folder; it takes the focus, so click the
    # Shell's title (clear of it) before asking DESK$ anything
    vm.cmd("echo files C:\\FMTEST > DESK$")
    time.sleep(1)
    def shell_focus():
        vm.point(600, 88)
        time.sleep(0.3)
        vm.click()
    shell_focus()
    wins, _ = vm.windows()
    assert "File Manager - C:\\fmtest" in wins, wins
    fx, fy, fw, fh = wins["File Manager - C:\\fmtest"]
    def fm_focus():                             # its corner pokes out below the Shell
        vm.point(fx + fw - 12, fy + fh - 8)
        time.sleep(0.3)
        vm.click()
    # Ctrl+N in File Manager makes a folder (keys reach the focused window)
    vm.point(fx + fw - 12, fy + fh - 8)
    time.sleep(0.3)
    vm.click()
    vm.keys("ctrl-n")
    time.sleep(0.6)
    vm.send("NEWDIR\r")
    time.sleep(0.8)
    vm.screenshot("13-file-manager")
    shell_focus()
    vm.cmd("dir \\fmtest", "NEWDIR")
    # drag one.txt from File Manager into Program Manager's Games group:
    # it becomes an item there, a .lnk that opens it
    fm_focus()
    vm.point(400, 220)
    time.sleep(0.3)
    vm.button(1)
    for x, y in ((390, 225), (300, 300), (150, 400), (60, 440)):
        vm.point(x, y)
        time.sleep(0.1)
    vm.screenshot("13b-dragging")
    vm.button(0)
    time.sleep(1)
    shell_focus()
    # whichever group was under the pointer now has it
    found = None
    for g in ("Main", "Demos", "Games", "Network"):
        out = vm.cmd(f"dir \\progman\\{g}")
        if "one.lnk" in out:
            found = g
    assert found, "no group got one.lnk"
    vm.cmd(f"type \\progman\\{found}\\one.lnk", 'EDIT "C:\\fmtest\\one.txt"')
    vm.cmd(f"del \\progman\\{found}\\one.lnk")
    # the clipboard: Ctrl+A, Ctrl+C, Ctrl+V into the same folder makes copies
    fm_focus()
    vm.keys("ctrl-a", "ctrl-c", "ctrl-v")
    time.sleep(1.5)
    shell_focus()
    vm.cmd("dir \\fmtest", ["one (2).txt", "two (2).txt", "NEWDIR (2)"])
    # a box dragged from the empty space up to the first file selects them all; Del
    lx = fx + 4 + max(120, (fw - 8) * 35 // 100) + 4 + 40
    ly = fy + 24 + 46 + 2
    fm_focus()
    vm.point(lx, ly + 7 * 18 + 30)
    time.sleep(0.3)
    vm.button(1)
    for yy in (ly + 100, ly + 50, ly + 18 + 9):
        vm.point(lx + 60, yy)
        time.sleep(0.15)
    vm.screenshot("13d-box-select")
    vm.button(0)
    time.sleep(0.3)
    vm.keys("delete")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    shell_focus()
    vm.cmd("dir \\fmtest", "0 file(s)", absent=["one.txt", "NEWDIR"])
    # cut and paste: Ctrl+X a file, open a folder, Ctrl+V moves it in
    vm.cmd("md \\fmtest\\sub")
    vm.cmd("echo three > \\fmtest\\three.txt")
    fm_focus()
    vm.keys("f5", "end", "ctrl-x", "home", "down", "ret")
    time.sleep(0.5)
    vm.keys("ctrl-v")
    time.sleep(1.5)
    vm.keys("backspace")
    time.sleep(0.5)
    shell_focus()
    vm.cmd("type \\fmtest\\sub\\three.txt", "three")
    vm.cmd("dir \\fmtest", "sub", absent="three.txt")
    vm.cmd("del \\fmtest\\sub\\three.txt")
    vm.cmd("rd \\fmtest\\sub")
    # a desktop that follows the sun, and the Desktop applet that controls it
    vm.cmd("control desktop.theme=Sunrise to Sunset")
    vm.cmd("control list", "desktop.timeofday=1")
    vm.send("control desktop\r")
    time.sleep(1.5)
    vm.screenshot("13c-desktop-applet")
    vm.keys("esc")
    time.sleep(0.5)
    shell_focus()
    vm.cmd("control desktop.theme=bdos Classic")
    vm.cmd("control list", "desktop.timeofday=0")
    # right-click the desktop: its menu
    vm.move_to(1000, 600)
    vm.rclick()
    vm.screenshot("14-desktop-menu")
    vm.keys("esc")
    time.sleep(0.4)
    # right-click a title: the control menu; Minimize from it, by key
    wins, _ = vm.windows()
    fx, fy, fw, fh = wins["File Manager - C:\\fmtest"]
    fm_focus()
    vm.point(fx + fw // 2, fy + 10)
    time.sleep(0.3)
    vm.rclick()
    vm.screenshot("15-control-menu")
    vm.keys("n")                                    # Mi_n_imize
    time.sleep(0.6)
    shell_focus()
    wins, _ = vm.windows()
    assert "File Manager - C:\\fmtest" in vm.minimized, vm.minimized
    vm.screenshot("16-minimized")
    # and a double-click on the icon brings it back
    vm.point(40, 768 - 30)
    time.sleep(0.3)
    vm.double_click()
    vm.screenshot("16b-restored")
    shell_focus()
    wins, _ = vm.windows()
    assert "File Manager - C:\\fmtest" not in vm.minimized, vm.minimized
    # Program Manager: right-click on an icon
    wins, _ = vm.windows()
    px, py, pw, ph = wins["Program Manager"]
    vm.point(px + 60, py + 120)
    time.sleep(0.3)
    vm.rclick()
    vm.screenshot("17-icon-menu")
    vm.keys("esc")
    time.sleep(0.4)
    # shrink Program Manager: its workspace grows scroll bars
    vm.point(px + pw - 2, py + ph - 2)
    time.sleep(0.3)
    vm.button(1)
    for i in range(1, 5):
        vm.point(px + pw - 2 - (pw - 380) * i // 4, py + ph - 2 - (ph - 260) * i // 4)
    vm.button(0)
    time.sleep(0.5)
    vm.screenshot("18-progman-small")
    shell_focus()
    wins, _ = vm.windows()
    assert wins["Program Manager"][2:] == (380 - px + 2, 260 - py + 2) or wins["Program Manager"][2] < pw, wins
    start = len(vm.log)
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version")


def t_smp(vm):
    want = int(os.environ.get("SMP", "4"))
    out = vm.cmd("type CPU$", f"cores {want}")
    vm.cmd("tasks", "Core")
    if want < 2:
        vm.cmd("echo off 0 > CPU$")
        out = vm.cmd("type CPU$")                   # the last core stays on
        assert re.search(r"cpu 0 load \d+ on", out), out
        # one core: a program computing in the background is preempted,
        # so the shell still answers straight away
        start = len(vm.log)
        vm.cmd("start \\demos\\spin.c 6", "Started task")
        t0 = time.time()
        vm.cmd("ver", "bdos version")
        assert time.time() - t0 < 3, f"the shell took {time.time() - t0:.1f}s"
        vm.wait_for("in 6 s", 30, start)
        return
    # four programs computing at once: several cores light up
    start = len(vm.log)
    for _ in range(4):
        vm.cmd("start \\demos\\spin.c 8", "Started task")
    time.sleep(4)
    out = vm.cmd("type CPU$")
    busy = [int(m) for m in re.findall(r"cpu \d+ load (\d+)", out)]
    assert sum(1 for b in busy if b >= 50) >= 2, f"only {busy} busy:\n{out}"
    vm.wait_for("in 8 s", 30, start)
    time.sleep(2)
    out = vm.text(start)
    assert out.count("SPIN:") == 4, out
    # park a core: it stops taking work
    vm.cmd("echo off 1 > CPU$")
    out = vm.cmd("type CPU$", "cpu 1 load")
    assert re.search(r"cpu 1 load \d+ off", out), out
    start = len(vm.log)
    for _ in range(3):
        vm.cmd("start \\demos\\spin.c 3", "Started task")
    time.sleep(2)
    out = vm.cmd("type CPU$")
    assert re.search(r"cpu 1 load [0-9] off", out), out
    vm.wait_for("in 3 s", 30, start)
    time.sleep(2)
    vm.cmd("echo on 1 > CPU$")
    vm.cmd("type CPU$", "cpu 1 load")
    # Pulse on the desktop, with the cores busy
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.send("\r")
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    for _ in range(3):
        vm.cmd("start \\demos\\spin.c 6", "Started task")
    vm.cmd("echo run @pulse > DESK$")
    time.sleep(2.5)
    vm.screenshot("20-pulse")
    vm.point(300, 400)                              # inside the Shell: focus back
    time.sleep(0.3)
    vm.click()
    wins, _ = vm.windows()
    assert "Pulse" in wins, wins
    # minimized, its icon keeps showing the bars
    px, py, pw, ph = wins["Pulse"]
    vm.point(px + pw - 4 - 45, py + 4 + 8)          # the minimize button
    time.sleep(0.3)
    vm.click()
    time.sleep(1)
    vm.screenshot("21-pulse-icon")
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version")


def t_winkeys(vm):
    """Windows 3.1 keyboard habits: Alt+Space, Move, Size; Program Manager remembers its place."""
    def desktop():
        start = len(vm.log)
        vm.send("win\r")
        time.sleep(3)
        vm.send("\r")
        vm.wait_for("C:\\>", 15, start)
        time.sleep(0.5)
    def leave():
        vm.keys("alt-x")
        time.sleep(0.8)
        vm.keys("ret")
        time.sleep(1.5)
        vm.cmd("ver", "bdos version")
    def focus(r):
        vm.point(r[0] + r[2] - 90, r[1] + 8)    # a title bar, clear of the other window
        time.sleep(0.3)
        vm.click()
    def where():
        vm.point(shell[0] + shell[2] - 20, shell[1] + shell[3] - 20)  # COM1 types into the focused window
        time.sleep(0.3)
        vm.click()
        return vm.windows()[0]["Program Manager"]
    desktop()
    wins, _ = vm.windows()
    shell = wins["Shell"]
    x, y, w, h = wins["Program Manager"]
    focus((x, y, 200, h))
    vm.keys("alt-spc", "m", "right", "right", "right", "right", "down", "ret")
    time.sleep(0.8)
    r = where()
    assert r[:2] == (x + 32, y + 8), (r, (x, y))
    focus((r[0], r[1], 200, h))
    vm.keys("alt-spc", "s", "right", "right", "down", "ret")
    time.sleep(0.8)
    moved = where()
    assert moved[2] >= w + 8 and moved[3] >= h + 8, (moved, (w, h))
    # the menu bar: Alt+F, then right arrow walks to the next menu
    focus((moved[0], moved[1], 200, h))
    vm.keys("alt-f", "right")
    time.sleep(0.5)
    vm.screenshot("22-menu-arrows")
    vm.keys("esc")
    time.sleep(0.3)
    leave()
    desktop()
    assert where() == moved, (where(), moved)
    # put it back for everyone else
    focus((moved[0], moved[1], 200, h))
    vm.keys("alt-spc", "m", "left", "left", "left", "left", "up", "ret")
    time.sleep(0.5)
    vm.keys("alt-spc", "s", "left", "left", "up", "ret")
    time.sleep(0.5)
    assert where() == (x, y, w, h), (where(), (x, y, w, h))
    leave()


def t_termui(vm):
    """Terminal windows: scrollback (wheel, Shift+PgUp), selecting and copying, pasting."""
    start = len(vm.log)
    vm.send("win\r")
    time.sleep(3)
    vm.send("\r")
    vm.wait_for("C:\\>", 15, start)
    time.sleep(0.5)
    wins, _ = vm.windows()
    sx, sy, sw, sh = wins["Shell"]
    vm.cmd("cls")
    for i in range(40):
        vm.send(f"echo scrolled line {i:02d}\r")
        time.sleep(0.05)
    vm.cmd("echo the end", "the end")
    def looking_back(name):
        """the yellow 'lines back' note at the top right of the Shell"""
        vm.screenshot(name)
        im = Image.open(os.path.join(OUT, name + ".png")).convert("RGB")
        return any(im.getpixel((x, y)) == (255, 255, 85)
                   for x in range(sx + sw - 260, sx + sw - 8, 2) for y in range(sy + 24, sy + 40, 2))
    vm.point(sx + sw // 2, sy + sh // 2)   # the wheel over the Shell looks back
    time.sleep(0.3)
    assert not looking_back("40-live")
    vm.wheel(4)
    time.sleep(0.5)
    assert looking_back("41-scrollback"), "the wheel didn't scroll back"
    vm.wheel(-4)
    time.sleep(0.5)
    assert not looking_back("42-wheel-down"), "the wheel didn't come back"
    vm.keys("shift-pgup")
    time.sleep(0.5)
    assert looking_back("43-shift-pgup"), "Shift+PgUp didn't scroll back"
    vm.cmd("ver", "bdos version")          # typing comes back to now
    assert not looking_back("44-typed")
    # select the line at the top of the window with the mouse, copy it, show it
    vm.cmd("cls")
    vm.cmd("echo select me please")
    cx, cy = sx + 4, sy + 24
    vm.drag(cx + 8 * 18 + 4, cy + 16 + 8, cx - 2, cy + 16 + 8)   # right to left: the edge would resize
    time.sleep(0.3)
    vm.screenshot("45-selected")
    vm.keys("ctrl-insert")
    time.sleep(0.3)
    vm.cmd("type CLIP$", "select me please")
    # Shift+Insert pastes, as typing
    vm.cmd("echo ver> CLIP$")
    start = len(vm.log)
    vm.keys("shift-insert")
    vm.wait_for("bdos version", 10, start)
    # right-click: the menu
    vm.point(cx + 200, cy + 100)
    time.sleep(0.3)
    vm.rclick()
    vm.screenshot("46-term-menu")
    vm.keys("esc")
    time.sleep(0.3)
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version")


def t_applets(vm):
    """The Startup group, item options, and the small programs."""
    vm.cmd("echo CD> C:\\PROGMAN\\Startup\\Where.lnk")
    vm.cmd("echo dir=C:\\BDOS>> C:\\PROGMAN\\Startup\\Where.lnk")
    vm.cmd("echo @eyes> C:\\PROGMAN\\Startup\\Eyes.lnk")
    vm.cmd("echo run=minimized>> C:\\PROGMAN\\Startup\\Eyes.lnk")
    start = len(vm.log)
    vm.send("win\r")
    vm.wait_for("C:\\BDOS", 20, start)            # Where.lnk ran CD in C:\BDOS
    time.sleep(2)
    vm.screenshot("50-startup")
    vm.keys("ret")                                  # [Finished - press any key]
    time.sleep(1)
    vm.keys("alt-n")                                # a shell to work from
    time.sleep(1.5)
    vm.screenshot("50b-shell")
    wins, _ = vm.windows()
    assert "Eyes" in wins and "Eyes" in vm.minimized, (wins, vm.minimized)
    def run(app, title, settle=1.5):
        start = len(vm.log)
        vm.send(f"echo run {app} > DESK$\r")
        time.sleep(settle)
    def close():
        vm.keys("alt-q")
        time.sleep(0.6)
    shell_rect = vm.windows()[0]["Shell"]
    def locate(title):
        """where the new window is: back to the shell (its bottom shows), ask, raise it again"""
        sx, sy, sw, sh = shell_rect
        vm.point(sx + 40, sy + sh - 20)
        time.sleep(0.3)
        vm.click()
        wins, _ = vm.windows()
        vm.cmd(f"echo raise {title} > DESK$")
        time.sleep(0.5)
        return wins[title]
    # the calculator: typed sums, the answer copied
    run("@calc", "Calculator")
    vm.send("12*3.5=")
    time.sleep(0.5)
    vm.screenshot("51-calculator")
    vm.send("\x03")
    time.sleep(0.3)
    close()
    vm.cmd("type CLIP$", "42")
    # Character Map: find A, add it, copy
    run("@charmap", "Character Map")
    vm.send("A\r\x03")
    time.sleep(0.5)
    vm.screenshot("52-charmap")
    close()
    vm.cmd("type CLIP$", "A")
    run("@help", "Help")
    vm.screenshot("53-help")
    close()
    wins, _ = vm.windows()
    assert not any(t.startswith("Help") for t in wins), wins
    run("@mines", "Mines")
    x, y, w, h = locate("Mines")
    vm.point(x + w // 2, y + 24 + 36 + 44 + 4 * 24 + 12)   # a cell in the middle
    time.sleep(0.3)
    vm.click()
    vm.screenshot("54-mines")
    close()
    run("@terminal", "Terminal")
    vm.screenshot("55-terminal")
    close()
    run("@neko", "Neko")
    x, y, w, h = locate("Neko")
    vm.point(x + 40, y + 60)
    time.sleep(2)
    vm.screenshot("56-neko")
    close()
    vm.keys("ctrl-esc")
    time.sleep(1.5)
    vm.screenshot("57-taskman")
    close()
    wins, _ = vm.windows()
    assert "Task Manager" not in wins, wins
    vm.cmd("control dos")
    time.sleep(1)
    vm.screenshot("58-dos-applet")
    close()
    vm.cmd("del C:\\PROGMAN\\Startup\\Where.lnk")
    vm.cmd("del C:\\PROGMAN\\Startup\\Eyes.lnk")
    vm.keys("alt-x")
    time.sleep(0.8)
    vm.keys("ret")
    time.sleep(1.5)
    vm.cmd("ver", "bdos version")


def t_dpmi(vm):
    vm.cmd("cd \\dosapps")
    out = vm.cmd("dpmitest", "DPMI test passed", timeout=60)
    for i in range(8):
        assert f"ok {i}" in out, out
    vm.cmd("echo %ERRORLEVEL%", "0")
    vm.cmd("cd \\")


def t_native(vm):
    vm.cmd("hello64 one two", ["Hello from a native 64-bit bdos program!", "argument 2: two"])
    vm.cmd("libctest", "libctest: all passed", timeout=60)
    out = vm.cmd("primes 2000000", "148933 primes", timeout=120)
    # one thread vs all of them: more cores should be faster
    t1 = float(re.search(r"in ([\d.]+) seconds", vm.cmd("primes 3000000 1", "216816 primes", timeout=180)).group(1))
    tn = float(re.search(r"in ([\d.]+) seconds", vm.cmd("primes 3000000", "216816 primes", timeout=180)).group(1))
    ncpu = int(os.environ.get("SMP", "4"))
    if ncpu > 1:
        assert tn < t1 * 0.75, f"threads didn't help: 1 thread {t1}s, {ncpu} threads {tn}s"
    # Ctrl+C stops a busy program, threads and all
    start = len(vm.log)
    vm.send("spin64 60\r")
    vm.wait_for("Ctrl+C stops", 20, start)
    time.sleep(1.5)
    vm.send("\x03")
    out = vm.settle(start, 15)
    assert "^C" in out and "Done." not in out, out
    vm.cmd("echo %ERRORLEVEL%", "130")
    vm.cmd("ver", "bdos version")


def t_tcc(vm):
    vm.cmd("cd \\bdos\\samples")
    vm.cmd("tcc -v", "tcc version 0.9.28")
    vm.cmd("tcc -o T64.EXE hello64.c", absent=["error", "rror:"], timeout=60)
    vm.cmd("t64 x y", ["Hello from a native 64-bit bdos program!", "argument 2: y"])
    vm.cmd("tcc -run hello64.c a b", ["Hello from a native 64-bit bdos program!", "argument 2: b"], timeout=60)
    vm.cmd("tcc -o LT.EXE libctest.c", absent=["error", "rror:"], timeout=60)
    vm.cmd("lt", "libctest: all passed", timeout=60)
    vm.cmd("tcc primes.c", absent="rror", timeout=60)
    vm.cmd("primes 1000000", "78498 primes", timeout=60)
    vm.cmd("del t64.exe")
    vm.cmd("del lt.exe")
    vm.cmd("del primes.exe")
    vm.cmd("cd \\")


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
                     ("chkdsk", t_chkdsk), ("ps2", t_ps2), ("basic", t_basic), ("c", t_c), ("control", t_control), ("dos", t_dos), ("dos2", t_dos2), ("dosfull", t_dosfull), ("dosgfx", t_dosgfx),
                     ("desktop", t_desktop), ("win31", t_win31), ("smp", t_smp), ("winkeys", t_winkeys), ("termui", t_termui), ("applets", t_applets), ("dpmi", t_dpmi), ("native", t_native), ("tcc", t_tcc), ("screens", t_screens),
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
    # a game that turns with the mouse: a click captures the pointer, and the
    # mouse's motion all goes to it -- far more than the window is wide --
    # until Ctrl+Alt lets go; after that, moving doesn't turn it
    vm2.cmd("cd \\dosapps")
    wins, (px, py) = vm2.windows()
    sx, sy, sw, sh = wins["Shell"]
    vm2.mon(f"mouse_move {sx + sw // 2 - px} {sy + sh // 2 - py}")
    start = len(vm2.log)
    vm2.send("turn\r")
    time.sleep(3)
    vm2.mon("mouse_button 1")
    vm2.mon("mouse_button 0")
    time.sleep(1)
    for _ in range(30):
        vm2.mon("mouse_move 100 0")
    time.sleep(1)
    vm2.mon("sendkey ctrl-alt")
    time.sleep(1)
    for _ in range(5):
        vm2.mon("mouse_move -100 0")
    time.sleep(1)
    vm2.keys("spc")
    vm2.wait_for("TURN: mickeys", 15, start)
    out = vm2.settle(start)
    mx = int(re.search(r"TURN: mickeys (-?\d+)", out).group(1))
    assert mx >= 2500, out[-300:]                  # 3000 to the right, captured
    vm2.cmd("cd \\")


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
