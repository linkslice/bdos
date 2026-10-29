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
        p = subprocess.Popen([BTERM, "-c", f"127.0.0.1:{PORT}"], stdin=s, stdout=s, stderr=s, cwd=SHARE)
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
        run("ver", "bdos version 0.14")
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
    check("bterm -c: a shell, T: both ways", text_session)

    def cd_session():
        # bterm -d: two discs in one changer at D:, another drive at E:
        isos = os.path.join(tmp, "isos")
        subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), "mktestiso.py"), isos], check=True)
        d1, d2 = os.path.join(isos, "DISC1.ISO"), os.path.join(isos, "DISC2.ISO")
        m, s = pty.openpty()
        p = subprocess.Popen([BTERM, "-c", "-d", f"{d1},{d2}", "-d", d2, f"127.0.0.1:{PORT}"],
                             stdin=s, stdout=s, stderr=s, cwd=SHARE)
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

        def run(cmd, want, secs=10):
            start = len(out)
            os.write(m, (cmd + "\r").encode())
            end = time.time() + secs
            while time.time() < end:
                pump(0.2)
                text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", out[start:].decode(errors="replace"))
                if re.search(want, text) and text.rstrip().endswith(">"):
                    return text
            raise AssertionError(f"{cmd!r}: wanted {want!r}, got:\n{out[start:].decode(errors='replace')}")

        pump(3)
        assert b"Connected to" in out, out
        run("disc", r"(?s)D:.*DISC1\.ISO")
        run("type D:\\readme.txt", "This is disc one")
        run("vol E:", "BDOS_DISC2")
        run("dir \"D:\\Game Data\"", "A Long File Name.txt")
        run("disc D: 2", "DISC2")
        run("type D:\\readme.txt", "This is disc two")
        run("copy \"D:\\Game Data\\A Long File Name.txt\" T:\\long.txt", "1 file")
        run("disc D: eject", r"\(empty\)")
        run("dir D:", "Not ready")
        os.write(m, b"exit\r")
        pump(2)
        p.wait(timeout=10)
        assert os.path.getsize(os.path.join(SHARE, "long.txt")) == 72000
        # the session's drives were its own: the machine's console never had them
        assert "BDOS_DISC" not in vm.cmd("vol E:")
    check("bterm -d: CD images in a changer", cd_session)

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

    def tone_seconds(path):
        """seconds of a ~1000 Hz tone in bterm's --audio-out recording"""
        import struct
        raw = open(path, "rb").read()[44:]
        n = len(raw) // 4
        left = struct.unpack(f"<{n * 2}h", raw[:n * 4])[0::2]
        secs = 0.0
        step = 4800                                  # 0.1 s
        for i in range(0, n - step, step):
            w = left[i:i + step]
            rms = (sum(v * v for v in w) / step) ** 0.5
            hz = sum(1 for a, b in zip(w, w[1:]) if (a < 0) != (b < 0)) * 5
            if rms > 1000 and 900 < hz < 1100:
                secs += 0.1
        return secs

    def session_sound(*extra):
        wav = os.path.abspath(os.path.join(OUTDIR, "tone.wav"))
        if os.path.exists(wav):
            os.remove(wav)
        env = dict(os.environ, BTERM_NO_SOUND_CARD="1", BTERM_STATS="1")
        r = bterm("-c", "--audio-out", wav, "--type", "\\\\dosapps\\\\tone\\r", "--type-after", "2",
                  "--quit-after", "9", *extra, f"127.0.0.1:{PORT}", env=env)
        time.sleep(1)
        return r, wav

    def sound():
        # TONE.COM's PC speaker in our session: 2 s of 1000 Hz, played here
        r, wav = session_sound()
        err = r.stderr.decode(errors="replace")
        secs = tone_seconds(wav)
        assert 1.6 <= secs <= 2.6, f"{secs:.1f} s of tone; {err[-300:]}"
        # a bad network: lost packets are filled in, and it still plays
        r, wav = session_sound("--loss", "10", "--delay", "30")
        err = r.stderr.decode(errors="replace")
        secs = tone_seconds(wav)
        assert 1.5 <= secs <= 2.6 and re.search(r"AUDIO: \d+ packets, [1-9]", err), f"{secs:.1f} s; {err[-300:]}"
        # -q: nothing asked for, nothing sent
        r, wav = session_sound("-q")
        assert not os.path.exists(wav) and "TONE done" in r.stdout.decode(errors="replace"), r.stdout[-300:]
        vm.cmd("ver", "bdos version")
    check("bterm: the session's sound plays here", sound)

    def screen():
        # bterm -t: the machine's screen in a window. The prompt works (and T:),
        # a DOS game takes the whole window, WIN starts a desktop in it and
        # Alt+X comes back to the prompt, and EXIT closes the window
        snap = os.path.abspath(os.path.join(OUTDIR, "screen.ppm"))
        out = os.path.join(SHARE, "screen.txt")
        r = bterm("-t", "--type", "ver > T:\\\\screen.txt\\r\\W\\Wcd \\\\dosapps\\rmode13\\r", "--type-after", "2",
                  "--quit-after", "12", "--snap", snap, f"127.0.0.1:{PORT}", env=SDL_ENV)
        assert r.returncode == 0, r.stderr.decode()
        assert "bdos version" in open(out).read()
        im = Image.open(snap).convert("RGB")
        assert im.size == (640, 480), im.size
        colours = len(set(im.getdata()))
        assert colours > 100, f"MODE13 in the window: {colours} colours"
        time.sleep(1)
        r = bterm("-t", "--type", "win\\r\\W\\W\\W", "--type-after", "2", "--quit-after", "11", "--snap", snap,
                  f"127.0.0.1:{PORT}", env=SDL_ENV)
        im = Image.open(snap).convert("RGB")
        teal = sum(1 for p in im.getdata() if p == (0x2F, 0x6F, 0x73))
        assert teal > 640 * 480 // 20, f"the desktop in the window: {teal} pixels of its colour"
        time.sleep(1)
        start = time.time()
        r = bterm("-t", "--type", "win\\r\\W\\W\\ax\\W\\r\\W\\Wexit\\r", "--type-after", "2", "--quit-after", "40",
                  f"127.0.0.1:{PORT}", env=SDL_ENV)
        assert r.returncode == 0 and time.time() - start < 30, f"EXIT after WIN: {time.time() - start:.1f} s"
        time.sleep(1)
        vm.cmd("ver", "bdos version")
    check("bterm -t: the machine's screen in a window", screen)

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
        r = bterm("-c", "-n", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"wants a password" in r.stderr, r.stderr
        r = bterm("-c", "-n", "-p", "wrong", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"password was wrong" in r.stderr, r.stderr
        r = bterm("-c", "-n", "-p", "swordfish", "--type", "ver\\r", "--type-after", "1",
                  "--quit-after", "4", f"127.0.0.1:{PORT}")
        assert r.returncode == 0 and b"bdos version" in r.stdout, r.stdout[-300:]
        vm.cmd("control net.password=")
        vm.cmd("control net.udp=0")
        r = bterm("-c", "-n", "--quit-after", "3", f"127.0.0.1:{PORT}")
        assert r.returncode == 1 and b"no answer" in r.stderr, r.stderr
        vm.cmd("control net.udp=1")
    check("bterm -l, net.password, net.udp", list_and_password)
finally:
    vm.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
