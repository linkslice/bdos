#!/usr/bin/env python3
"""Two bdos machines on one virtual LAN: \\\\BOX1 (this one) and \\\\BOX2.

Usage: nettest.py <bdos.img> <outdir>
"""
import os, shutil, sys, tempfile, time

sys.argv = [sys.argv[0], sys.argv[1], sys.argv[2]]
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])     # VM, check(), screenshot helpers

PORT = 40000 + os.getpid() % 2000


class NetVM(VM):
    def __init__(self, name, mac, role):
        global disk
        my_disk = os.path.join(tmp, name + ".img")
        shutil.copy(IMG, my_disk)
        disk = my_disk                    # VM() uses the module-level disk
        how = f"listen=127.0.0.1:{PORT}" if role == "listen" else f"connect=127.0.0.1:{PORT}"
        super().__init__(name, ["-nic", "none", "-netdev", f"socket,id=n0,{how}",
                                "-device", f"e1000,netdev=n0,mac={mac}"])


def name_box(vm, name):
    vm.wait_for("C:\\>", 90)
    vm.cmd(f"hostname {name}", f"\\\\{name}")


box2 = NetVM("box2", "52:54:00:00:00:02", "listen")
time.sleep(1)
box1 = NetVM("box1", "52:54:00:00:00:01", "connect")
try:
    def setup():
        name_box(box2, "BOX2")
        name_box(box1, "BOX1")
        box2.cmd("echo this file lives on BOX2 > C:\\WHOAMI.TXT")
        box1.cmd("md \\share")
        box1.cmd("cd \\share")
        box1.cmd("echo this file lives on BOX1 > mine.txt")
    check("two machines up", setup)

    def view():
        box1.cmd("net", ["\\\\BOX1", "BDNET"])
        box1.cmd("net view", ["\\\\BOX2", "\\\\BOX1", "2 machine(s)"], timeout=10)
    check("NET VIEW finds both", view)

    def ctty():
        start = len(box1.log)
        box1.send("ctty \\\\box2\r")
        box1.wait_for("Connected to \\\\BOX2", 15, start)
        box1.wait_for("C:\\>", 10, start)
        time.sleep(0.5)
        box1.cmd("type C:\\WHOAMI.TXT", "this file lives on BOX2")       # runs on BOX2
        box1.cmd("dir T:", ["mine.txt"])                                   # BOX1's \SHARE
        box1.cmd("type T:\\mine.txt", "this file lives on BOX1")
        box1.cmd("copy T:\\mine.txt C:\\FROMBOX1.TXT", "1 file(s) copied")
        box1.cmd("echo written by BOX2 into T: > T:\\reply.txt")
        box1.cmd("drives", ["T:", "on \\\\BOX1"])
        box1.cmd("ver", "bdos version")
        start = len(box1.log)
        box1.send("exit\r")
        box1.wait_for("Connection to \\\\box2 closed", 10, start)
        time.sleep(0.5)
        box1.cmd("type reply.txt", "written by BOX2 into T:")               # landed on BOX1
        box2.cmd("type C:\\FROMBOX1.TXT", "this file lives on BOX1")         # landed on BOX2
        box1.cmd("dir T:", "Invalid drive")                                  # T: was the session's
    check("CTTY \\\\BOX2 with a T: drive", ctty)

    def netuse():
        box1.cmd("net use E: \\\\box2\\C", "E: is now")
        box1.cmd("dir E:", ["WHOAMI.TXT", "FROMBOX1.TXT"])
        box1.cmd("type E:\\WHOAMI.TXT", "this file lives on BOX2")
        box1.cmd("copy mine.txt E:\\COPIED.TXT", "1 file(s) copied")
        box2.cmd("type C:\\COPIED.TXT", "this file lives on BOX1")
        box1.cmd("net use", "E:  \\\\box2")
        box1.cmd("net use E: /delete")
        box1.cmd("dir E:", "Invalid drive")
    check("NET USE E: \\\\BOX2\\C", netuse)

    def cpu():
        # one command on the CPU server, run from our directory (its T:)
        box1.cmd("cpu \\\\box2 type mine.txt", "this file lives on BOX1", timeout=20)
        box1.cmd("cpu \\\\box2 type C:\\WHOAMI.TXT", "this file lives on BOX2", timeout=20)
        box1.cmd("control net.cpu=\\\\box2")
        box1.cmd("cpu type CPU$", "cores", timeout=20)
        box1.cmd("cpu \\\\box2 echo made on BOX2 > made.txt", timeout=20)
        box1.cmd("type made.txt", "made on BOX2")
    check("CPU \\\\BOX2 command", cpu)

    def remote_desktop():
        box1.cmd("cd \\")
        start = len(box1.log)
        box1.send("win \\\\box2\r")
        time.sleep(6)
        box1.screenshot("net-1-remote-desktop")
        # BOX2's Program Manager has focus: Enter opens a shell on BOX2
        start2 = len(box2.log)
        box1.send("\r")
        box2.wait_for("C:\\>", 15, start2)        # terminal windows mirror to BOX2's COM1
        time.sleep(1)
        box1.send("type C:\\WHOAMI.TXT\r")
        box2.wait_for("this file lives on BOX2", 10, start2)
        box1.send("dir T:\r")
        box2.wait_for("share", 15, start2)        # T: is BOX1's C:\, where WIN was typed
        time.sleep(2)
        box1.screenshot("net-2-remote-shell")
        # leave: Alt+X on BOX2's desktop, confirm
        box1.keys("alt-x")
        time.sleep(1.5)
        box1.keys("ret")
        time.sleep(3)
        box1.cmd("ver", "bdos version")
    check("WIN \\\\BOX2 full screen", remote_desktop)

    def remote_in_window():
        # BOX1's own desktop, a shell window, and BOX2's desktop inside it
        start = len(box1.log)
        box1.send("win\r")
        time.sleep(3)
        box1.send("\r")
        box1.wait_for("C:\\>", 15, start)
        time.sleep(0.5)
        box1.send("win \\\\box2\r")
        time.sleep(6)
        box1.screenshot("net-3-remote-in-window")
        box1.keys("f12")                     # hang up the remote view
        time.sleep(2)
        box1.cmd("echo %ERRORLEVEL%", "0")
        box1.keys("alt-x")
        time.sleep(1)
        box1.keys("ret")
        time.sleep(2)
        box1.cmd("ver", "bdos version")
    check("WIN \\\\BOX2 in a desktop window", remote_in_window)
finally:
    box1.close()
    box2.close()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
