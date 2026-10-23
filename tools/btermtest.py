#!/usr/bin/env python3
"""bterm against a bdos VM: TCP/IP basics, a text session with T: both ways,
the desktop in bterm's window (SDL offscreen), discovery and passwords.

Usage: btermtest.py <bdos.img> <bterm binary> <outdir>
"""
import os, pty, re, select, shutil, subprocess, sys, time

IMG, BTERM, OUTDIR = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
sys.argv = [sys.argv[0], IMG, OUTDIR]
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])        # VM, check()
from PIL import Image

PORT = 21000 + os.getpid() % 2000
SHARE = os.path.join(tmp, "share")
os.makedirs(os.path.join(SHARE, "Sub Dir"))
open(os.path.join(SHARE, "Mine.txt"), "w").write("this file lives on the host\n")
open(os.path.join(SHARE, "Sub Dir", "deep.txt"), "w").write("deep down\n")
SDL_ENV = dict(os.environ, SDL_VIDEODRIVER="offscreen")


def bterm(*args, **kw):
    return subprocess.run([BTERM, *args], capture_output=True, timeout=60, cwd=SHARE,
                          stdin=subprocess.DEVNULL, **kw)


vm = VM("bt", ["-nic", f"user,model=e1000,hostfwd=udp:127.0.0.1:{PORT}-:17979"])
try:
    def tcpip():
        vm.wait_for("C:\\>", 90)
        for _ in range(20):                        # DHCP takes a moment
            if "10.0.2.15" in vm.cmd("ipconfig"):
                break
            time.sleep(0.5)
        vm.cmd("ipconfig", ["10.0.2.15", "255.255.255.0", "10.0.2.2", "port 17979"])
        vm.cmd("ping 10.0.2.2", "4 of 4 answered", timeout=20)
        vm.cmd("ping 1.2", "Usage")
    check("DHCP, IPCONFIG, PING", tcpip)

    def text_session():
        m, s = pty.openpty()
        p = subprocess.Popen([BTERM, "-t", f"127.0.0.1:{PORT}"], stdin=s, stdout=s, stderr=s, cwd=SHARE)
        os.close(s)
        out = b""

        def pump(secs):
            nonlocal out
            end = time.time() + secs
            while time.time() < end:
                r, _, _ = select.select([m], [], [], 0.1)
                if r:
                    try:
                        out += os.read(m, 65536)
                    except OSError:
                        return

        def run(cmd, want, secs=8):
            nonlocal out
            start = len(out)
            os.write(m, (cmd + "\r").encode())
            end = time.time() + secs
            while time.time() < end:
                pump(0.2)
                text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", out[start:].decode(errors="replace"))
                if want in text and text.rstrip().endswith(">"):
                    return text
            raise AssertionError(f"{cmd!r}: wanted {want!r}, got:\n{out[start:].decode(errors='replace')}")

        pump(3)
        assert b"Connected to" in out, out
        run("ver", "bdos version 0.11")
        run("dir T:", "Mine.txt")
        run("type t:\\mine.txt", "this file lives on the host")         # any case
        run('type "T:\\sub dir\\deep.txt"', "deep down")
        run("echo written by bdos > T:\\reply.txt", "C:\\>")
        run("copy C:\\HELLO.C T:\\", "1 file(s) copied")
        run("md T:\\newdir", "C:\\>")
        run("drives", "on \\\\")
        os.write(m, b"exit\r")
        pump(2)
        p.wait(timeout=10)
        assert open(os.path.join(SHARE, "reply.txt")).read().strip() == "written by bdos"
        assert "int main" in open(os.path.join(SHARE, "HELLO.C")).read()
        assert os.path.isdir(os.path.join(SHARE, "newdir"))
        assert b"connection closed" in out, out[-300:]
    check("bterm -t: a shell, T: both ways", text_session)

    def desktop():
        snap = os.path.join(tmp, "desk.ppm")
        r = bterm("--type", "\\rdir T:\\r", "--type-after", "3", "--click", "248,185,2",
                  "--quit-after", "10", "--snap", snap, f"127.0.0.1:{PORT}", env=SDL_ENV)
        assert r.returncode == 0, r.stderr.decode()
        img = Image.open(snap)
        img.save(os.path.join(OUTDIR, "bterm-desktop.png"))
        assert img.size == (1024, 768)
        # the desktop's teal, and a black shell window, made it across
        px = img.convert("RGB")
        assert px.getpixel((900, 700)) == (0x2F, 0x6F, 0x73), px.getpixel((900, 700))
        assert px.getpixel((300, 450)) == (0, 0, 0), px.getpixel((300, 450))
        time.sleep(1)
        vm.cmd("ver", "bdos version")             # the machine's own screen is untouched
    check("bterm desktop window", desktop)

    def desktop_resize():
        # the bterm window made bigger: the desktop grows to fill it
        snap = os.path.join(tmp, "desk2.ppm")
        r = bterm("--resize", "1280x800", "--type-after", "2", "--quit-after", "9", "--snap", snap,
                  f"127.0.0.1:{PORT}", env=SDL_ENV)
        assert r.returncode == 0, r.stderr.decode()
        img = Image.open(snap)
        img.save(os.path.join(OUTDIR, "bterm-resized.png"))
        assert img.size == (1280, 800), img.size
        px = img.convert("RGB")
        assert px.getpixel((1200, 760)) == (0x2F, 0x6F, 0x73), px.getpixel((1200, 760))
        time.sleep(1)
    check("bterm: a bigger window, a bigger desktop", desktop_resize)

    def desktop_game():
        # a DOS game in the bterm window: the keyboard arrives as make and
        # break codes, so a key held for 1.5 s is held for 1.5 s
        start = len(vm.log)
        r = bterm("--type", "\\rcd \\dosapps\\rkeyhold\\r\\W\\Ha\\e", "--type-after", "3",
                  "--quit-after", "14", f"127.0.0.1:{PORT}", env=SDL_ENV)
        assert r.returncode == 0, r.stderr.decode()
        out = vm.text(start)
        m = re.search(r"1E=(\d+)", out)
        assert m and 20 <= int(m.group(1)) <= 35, out[-600:]
        time.sleep(1)
        vm.cmd("ver", "bdos version")
    check("bterm: a DOS game sees a held key", desktop_game)

    def bench():
        # a bad network (40 ms each way, 2% lost): a dragged window still
        # catches up quickly, and the updates come squashed
        r = subprocess.run([BTERM, "--bench", "--delay", "40", "--loss", "2", f"127.0.0.1:{PORT}"],
                           capture_output=True, timeout=300, env=SDL_ENV, stdin=subprocess.DEVNULL, cwd=SHARE)
        out = r.stdout.decode(errors="replace")
        open(os.path.join(OUTDIR, "bench.txt"), "w").write(out)
        m = re.search(r"BENCH drag: (\d+) updates, (\d+) KB, settled (\d+) ms", out)
        assert m, out + r.stderr.decode(errors="replace")
        updates, kb, settle = map(int, m.groups())
        assert settle < 1000 and updates >= 20 and kb / updates < 30, out
        assert "BENCH typing" in out, out
        time.sleep(1)
        vm.cmd("ver", "bdos version")
    check("bterm on a bad network: a drag catches up quickly", bench)

    def list_and_password():
        r = bterm("-l", f"127.0.0.1:{PORT}")
        assert b"\\\\BDOS-" in r.stdout, r.stdout
        vm.cmd("control net.password=swordfish", "swordfish")
        r = bterm("-t", "-n", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"wants a password" in r.stderr, r.stderr
        r = bterm("-t", "-n", "-p", "wrong", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"password was wrong" in r.stderr, r.stderr
        r = bterm("-t", "-n", "-p", "swordfish", "--type", "ver\\r", "--type-after", "1",
                  "--quit-after", "4", f"127.0.0.1:{PORT}")
        assert r.returncode == 0 and b"bdos version" in r.stdout, r.stdout[-300:]
        vm.cmd("control net.password=")
        vm.cmd("control net.udp=0")
        r = bterm("-t", "-n", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"no answer" in r.stderr, r.stderr
        vm.cmd("control net.udp=1")
    check("bterm -l, net.password, net.udp", list_and_password)
finally:
    vm.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
