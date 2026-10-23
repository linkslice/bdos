#!/usr/bin/env python3
"""How bterm feels: drag a window, open one, type, on a good and on a bad
network (bterm --loss and --delay), and print the numbers.

Usage: btermbench.py <bdos.img> <bterm binary> <outdir> [profile ...]
Profiles: lan, slow (40 ms), lossy (1%), bad (40 ms and 2%)
"""
import os, sys, subprocess, time

IMG, BTERM, OUTDIR = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3]
WANT = sys.argv[4:] or ["lan", "slow", "lossy", "bad"]
sys.argv = [sys.argv[0], IMG, OUTDIR]
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])        # VM

PORT = 23000 + os.getpid() % 2000
SDL_ENV = dict(os.environ, SDL_VIDEODRIVER="offscreen")
PROFILES = {"lan": [], "slow": ["--delay", "40"], "lossy": ["--loss", "1"],
            "bad": ["--delay", "40", "--loss", "2"]}

vm = VM("bench", ["-nic", f"user,model=e1000,hostfwd=udp:127.0.0.1:{PORT}-:17979"])
results = {}
try:
    vm.wait_for("C:\\>", 90)
    for _ in range(20):
        if "10.0.2.15" in vm.cmd("ipconfig"):
            break
        time.sleep(0.5)
    for name in WANT:
        r = subprocess.run([BTERM, "--bench", *PROFILES[name], f"127.0.0.1:{PORT}"], capture_output=True,
                           timeout=400, env=SDL_ENV, stdin=subprocess.DEVNULL)
        out = r.stdout.decode(errors="replace")
        results[name] = [l for l in out.splitlines() if l.startswith("BENCH")]
        if r.returncode:
            results[name].append("exit %d: %s" % (r.returncode, r.stderr.decode(errors="replace")[-300:]))
        time.sleep(2)
finally:
    vm.close()
for name in WANT:
    print(f"--- {name} {' '.join(PROFILES[name])}")
    for l in results.get(name, []):
        print("   ", l)
