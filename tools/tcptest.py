#!/usr/bin/env python3
"""TCP/IP from bdos to the outside world, through QEMU's user network:
DNS, FETCH from a web server, TELNET out to a little server here, and
telnet in to bdos's own telnet server.

Usage: tcptest.py <bdos.img> <outdir>
"""
import http.server, os, socket, socketserver, struct, sys, threading, time

sys.argv = [sys.argv[0], sys.argv[1], sys.argv[2]]
src = open(os.path.join(os.path.dirname(__file__), "test.py")).read()
exec(src.split("# ---- the tests")[0])     # VM, check()

HTTP = 41000 + os.getpid() % 1000
TEL = 42000 + os.getpid() % 1000
TELIN = 43000 + os.getpid() % 1000
BIG = bytes((i * 7) % 251 for i in range(300000))


# ---- a web server
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/hello.txt":
            body = b"Hello from the web, bdos!\n"
        elif self.path == "/big.bin":
            body = BIG
        elif self.path == "/moved":
            self.send_response(302)
            self.send_header("Location", "/hello.txt")
            self.end_headers()
            return
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


httpd = socketserver.ThreadingTCPServer(("127.0.0.1", HTTP), Handler)
threading.Thread(target=httpd.serve_forever, daemon=True).start()


# ---- a telnet server: negotiates, draws in ANSI, echoes a line back
def tel_client(s):
    s.sendall(b"\xff\xfb\x01\xff\xfb\x03\xff\xfd\x1f")          # WILL ECHO, WILL SGA, DO NAWS
    s.sendall(b"\x1b[2J\x1b[1;1H\x1b[1;33mWELCOME TO THE TEST BBS\x1b[0m\r\nType something: ")
    data = b""
    s.settimeout(20)
    try:
        while b"\r" not in data:
            chunk = s.recv(100)
            if not chunk:
                return
            data += chunk
    except socket.timeout:
        return
    # drop telnet negotiation from what came in
    text, i = b"", 0
    while i < len(data):
        if data[i] == 255:
            i += 3 if data[i + 1] != 250 else data.index(b"\xff\xf0", i) + 2 - i
            continue
        text += data[i:i + 1]
        i += 1
    s.sendall(b"\r\nYou said: " + text.strip() + b"\r\nBye.\r\n")
    s.close()


tel = socket.socket()
tel.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
tel.bind(("127.0.0.1", TEL))
tel.listen(4)
threading.Thread(target=lambda: [threading.Thread(target=tel_client, args=(tel.accept()[0],), daemon=True).start()
                                 for _ in iter(int, 1)], daemon=True).start()


# ---- a DNS server for one made-up name
def dns_server():
    try:
        u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        u.bind(("127.0.0.1", 53))
    except OSError:
        return
    while True:
        q, addr = u.recvfrom(512)
        qid = q[:2]
        name_end = q.index(b"\0", 12) + 5
        question = q[12:name_end]
        labels, i = [], 12
        while q[i]:
            labels.append(q[i + 1:i + 1 + q[i]].decode())
            i += q[i] + 1
        name = ".".join(labels).lower()
        if name == "webserver.bdos.test":
            ans = b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + bytes([10, 0, 2, 2])
            r = qid + b"\x81\x80" + struct.pack(">HHHH", 1, 1, 0, 0) + question + ans
        else:
            r = qid + b"\x81\x83" + struct.pack(">HHHH", 1, 0, 0, 0) + question
        u.sendto(r, addr)


threading.Thread(target=dns_server, daemon=True).start()

vm = VM("tcp", ["-nic", f"user,model=e1000,hostfwd=tcp:127.0.0.1:{TELIN}-:23"])
try:
    def up():
        vm.wait_for("C:\\>", 90)
        for _ in range(40):
            if "IPv4 address . . . . : 10.0.2." in vm.cmd("ipconfig"):
                return
            time.sleep(0.5)
        raise AssertionError("no DHCP address")
    check("DHCP address", up)

    def ping():
        vm.cmd("ping 10.0.2.2", "Reply from")
    check("PING the host", ping)

    def dns():
        vm.cmd("control net.dns=10.0.2.2")
        vm.cmd("nslookup webserver.bdos.test", "Address: 10.0.2.2", timeout=15)
        vm.cmd("nslookup nowhere.bdos.test", "Unknown host", timeout=15)
    check("DNS", dns)

    def fetch():
        vm.cmd(f"fetch http://webserver.bdos.test:{HTTP}/hello.txt", "Hello from the web, bdos!", timeout=30)
        vm.cmd(f"fetch http://10.0.2.2:{HTTP}/moved", ["Redirected", "Hello from the web, bdos!"], timeout=30)
        vm.cmd(f"fetch http://10.0.2.2:{HTTP}/big.bin C:\\BIG.BIN", "300000 bytes", timeout=60)
        vm.cmd("dir C:\\BIG.BIN", "300,000")
        vm.cmd(f"fetch http://10.0.2.2:{HTTP}/nothing", "404", timeout=30)
        vm.cmd("fetch http://10.0.2.2:1/", "refused", timeout=30)
    check("FETCH over HTTP", fetch)

    def native():
        vm.cmd(f"httpget webserver.bdos.test:{HTTP} /hello.txt", ["Hello from the web, bdos!", "bytes]"], timeout=30)
    check("a native program's TCP", native)

    def telnet_out():
        start = len(vm.log)
        vm.send(f"telnet 10.0.2.2 {TEL}\r")
        vm.wait_for("Type something:", 20, start)
        vm.send("hello bbs\r")
        vm.wait_for("closed by the other end", 20, start)
        out = vm.settle(start)
        assert "WELCOME TO THE TEST BBS" in out and "You said: hello bbs" in out, out
        vm.cmd("netstat", "Active connections")
    check("TELNET out", telnet_out)

    def telnet_in():
        vm.cmd("control net.telnet=1")
        time.sleep(2)
        s = socket.create_connection(("127.0.0.1", TELIN), timeout=20)
        got = b""
        def until(token, t=20):
            nonlocal got
            end = time.time() + t
            while token not in got and time.time() < end:
                try:
                    chunk = s.recv(4096)
                except socket.timeout:
                    break
                if not chunk:
                    break
                got += chunk
            assert token in got, got[-1500:]
        s.sendall(b"\xff\xfb\x1f\xff\xfa\x1f\x00\x50\x00\x18\xff\xf0")  # WILL NAWS 80x24
        until(b"EXIT to hang up")
        until(b">")
        s.sendall(b"ver\r\n")
        until(b"bdos version")
        s.sendall(b"echo telnet works\r\n")
        until(b"telnet works\r\n")
        s.sendall(b"exit\r\n")
        s.settimeout(10)
        end = time.time() + 10
        while time.time() < end:
            try:
                if not s.recv(4096):
                    break
            except socket.timeout:
                break
        s.close()
        vm.cmd("control net.telnet=0")
    check("telnet in to bdos", telnet_in)
finally:
    vm.close()
    httpd.shutdown()

print(f"\n{passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
