# bterm: a drawterm for bdos

bterm connects to a bdos machine from Linux or macOS. It shows that machine's
desktop in a window, or with `-t` its shell in your terminal. The directory you
start bterm from becomes drive **T:** over there, the way drawterm's
`/mnt/term` works with Plan 9: `copy T:\notes.txt C:\` pulls a file from your
Mac, and `copy C:\HELLO.C T:\` hands one back.

```sh
make                      # needs SDL2: brew install sdl2 / apt install libsdl2-dev
./bterm 192.168.1.50      # the desktop, in a window
./bterm -t 192.168.1.50   # a shell in this terminal (Ctrl-] hangs up)
./bterm -l                # who's out there? (broadcasts on the LAN)
```

If SDL2 isn't installed, `make` builds a text-only bterm, and `-t` still works.

## Options

| | |
|---|---|
| `-t` | text mode: a bdos shell in this terminal |
| `-g WxH` | desktop size (default 1024x768) |
| `-s dir` | share `dir` as T: (default: the current directory) |
| `-n` | don't share anything |
| `-p password` | the machine's `net.password`, if it has one (or set `$BTERM_PASSWORD`) |
| `-z N` | scale the window N times |
| `-l [host]` | list bdos machines: by broadcast, or ask one host |
| `-v` | log what T: is asked for |

The host can be a name or an address, with an optional `:port`; the default
port is 17979.

## Keys

- Keys pass straight through.
- Alt+letter, or Cmd+letter on a Mac, reaches bdos as Alt, so Alt+N (new
  shell), Alt+W (next window) and Alt+X (leave the desktop) work.
- Closing the window hangs up. Your bdos session ends, but the machine and
  its own screen carry on.

## Under QEMU

QEMU's user networking needs the UDP port forwarded to the guest:

```sh
qemu-system-x86_64 ... -nic user,model=e1000,hostfwd=udp:127.0.0.1:17979-:17979
./bterm localhost
```

The `run.sh` in the bdos QEMU bundle already does this.

## On the bdos side

| | |
|---|---|
| `IPCONFIG` | the address (DHCP by default). `IPCONFIG /RENEW`, `/RELEASE` |
| `CONTROL net.ip=192.168.1.50` | a fixed address (also `net.mask`, `net.gateway`) |
| `CONTROL net.password=...` | make bterm ask for a password |
| `CONTROL net.udp=0` | turn bterm access off altogether |

## How it works

bterm speaks BDNET, the protocol bdos machines use between themselves for
`CTTY \\HOST`, `WIN \\HOST` and `NET USE`. It carries it inside UDP:

```
"BDNU" version password-length password  BDNET-frame
```

so it works through QEMU, NAT and routers. bdos gives each UDP peer a made-up
MAC address, and the rest of its network code can't tell bterm from another
bdos box.

Connections are reliable, ordered message streams. On top of them:
- the desktop arrives as run-length-encoded pixels;
- keys and mouse events go back as events;
- T: is a small file protocol (stat, list, read, write, create, rename...)
  answered from the shared directory.

Names are matched case-insensitively, since DOS users type `TYPE T:\README.TXT`.
`..` can't escape the shared directory.

There's no encryption. The password is sent as plain text and only keeps out
casual visitors. Use bterm on networks you trust.
