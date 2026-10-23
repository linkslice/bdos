/*
 * bdos.h - bdos's own calls for native programs: threads, the console,
 * the cores, and the SYS calls BASIC and the C interpreter know.
 *
 *   int t = bdos_thread(worker, arg);  ...  bdos_join(t);
 *   bdos_sys("Desk_Color", "i", 0x204060);
 */
#ifndef _BDOS_H
#define _BDOS_H
#include <stdint.h>

long bdos_syscall(long n, long a, long b, long c, long d);

int bdos_thread(int (*fn)(void *), void *arg);	/* -> thread id */
int bdos_join(int tid);				/* -> what fn returned */
int bdos_cores(void);
void bdos_yield(void);
long bdos_ms(void);				/* uptime */
long bdos_us(void);
int bdos_key(int wait);				/* bdos key codes; -1 if none and not waiting */
void bdos_cls(void);
void bdos_goto(int x, int y);
void bdos_color(int fg, int bg);
void bdos_cursor(int on);
void bdos_size(int *cols, int *rows);

/* a mutex that sleeps instead of spinning */
typedef struct { volatile uint32_t v; } bdos_mutex;
#define BDOS_MUTEX_INIT { 0 }
void bdos_lock(bdos_mutex *m);
void bdos_unlock(bdos_mutex *m);

/*
 * TCP/IP. Addresses are IPv4 in host order (10.0.2.2 is 0x0A000202).
 * Errors come back negative (BS_E... in bdos_sys.h).
 */
long bdos_resolve(const char *name);			/* name or a.b.c.d -> address */
int bdos_tcp_connect(long addr, int port, int timeout_ms);	/* -> socket */
int bdos_tcp_listen(int port);
int bdos_tcp_accept(int sock, int timeout_ms);		/* -1: wait forever */
long bdos_tcp_send(int sock, const void *buf, long n);
long bdos_tcp_recv(int sock, void *buf, long n, int timeout_ms);	/* 0 at the end */
int bdos_tcp_close(int sock);
const char *bdos_strerror(long err);			/* for the negative results */

/* Sound: 16-bit samples at any rate, mixed with everything else playing */
int bdos_audio_open(int rate);
long bdos_audio_write(int stream, const short *samples, long frames, int channels);	/* waits for room */
long bdos_audio_queued(int stream);
int bdos_audio_close(int stream);	/* after what's queued has played */

/*
 * SYS calls by name. types is one letter per argument: i (long long),
 * s (string). The result goes in *ret_n / ret_s (either may be NULL);
 * returns 0, or negative if the call doesn't exist or failed.
 */
int bdos_sys(const char *name, const char *types, ...);
int bdos_sysr(const char *name, long long *ret_n, char *ret_s, const char *types, ...);
#endif
