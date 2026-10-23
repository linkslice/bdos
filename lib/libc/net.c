/* net.c - TCP/IP for native programs (bdos.h) */
#include <bdos.h>
#include "../../include/bdos_sys.h"

long bdos_resolve(const char *name) { return bdos_syscall(BS_RESOLVE, (long)name, 0, 0, 0); }
int bdos_tcp_connect(long addr, int port, int timeout_ms) { return (int)bdos_syscall(BS_TCP_CONNECT, addr, port, timeout_ms, 0); }
int bdos_tcp_listen(int port) { return (int)bdos_syscall(BS_TCP_LISTEN, port, 0, 0, 0); }
int bdos_tcp_accept(int sock, int timeout_ms) { return (int)bdos_syscall(BS_TCP_ACCEPT, sock, timeout_ms, 0, 0); }
long bdos_tcp_send(int sock, const void *buf, long n) { return bdos_syscall(BS_TCP_SEND, sock, (long)buf, n, 0); }
long bdos_tcp_recv(int sock, void *buf, long n, int timeout_ms) { return bdos_syscall(BS_TCP_RECV, sock, (long)buf, n, timeout_ms); }
int bdos_tcp_close(int sock) { return (int)bdos_syscall(BS_TCP_CLOSE, sock, 0, 0, 0); }

const char *bdos_strerror(long e)
{
	switch (e) {
	case BS_ENOENT: return "No such file or directory";
	case BS_ENOMEM: return "Not enough memory";
	case BS_EBADF: return "Bad handle";
	case BS_ENFILE: return "Too many open";
	case BS_ETIMEDOUT: return "Timed out";
	case BS_EREFUSED: return "Connection refused";
	case BS_ERESET: return "Connection reset";
	case BS_ENOHOST: return "Unknown host";
	case BS_ENONET: return "No network";
	case BS_EEXIST: return "Already in use";
	default: return e < 0 ? "Error" : "No error";
	}
}

int bdos_audio_open(int rate) { return (int)bdos_syscall(BS_AUDIO_OPEN, rate, 0, 0, 0); }
long bdos_audio_write(int s, const short *p, long frames, int ch) { return bdos_syscall(BS_AUDIO_WRITE, s, (long)p, frames, ch); }
long bdos_audio_queued(int s) { return bdos_syscall(BS_AUDIO_QUEUED, s, 0, 0, 0); }
int bdos_audio_close(int s) { return (int)bdos_syscall(BS_AUDIO_CLOSE, s, 0, 0, 0); }
