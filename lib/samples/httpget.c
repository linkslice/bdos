/*
 * HTTPGET.EXE - fetch a web page with bdos's TCP/IP, from a native program.
 *
 *   HTTPGET host[:port] [/path]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bdos.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		printf("Usage: HTTPGET host[:port] [/path]\n");
		return 1;
	}
	char host[128];
	strncpy(host, argv[1], sizeof host - 1);
	host[sizeof host - 1] = 0;
	int port = 80;
	char *colon = strchr(host, ':');
	if (colon) {
		*colon = 0;
		port = atoi(colon + 1);
	}
	const char *path = argc > 2 ? argv[2] : "/";
	long addr = bdos_resolve(host);
	if (addr < 0) {
		printf("%s: %s\n", host, bdos_strerror(addr));
		return 1;
	}
	int s = bdos_tcp_connect(addr, port, 10000);
	if (s < 0) {
		printf("connect: %s\n", bdos_strerror(s));
		return 1;
	}
	char req[512];
	int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	bdos_tcp_send(s, req, n);
	char buf[4096];
	long got, total = 0;
	while ((got = bdos_tcp_recv(s, buf, sizeof buf, 20000)) > 0) {
		fwrite(buf, 1, (size_t)got, stdout);
		total += got;
	}
	bdos_tcp_close(s);
	fprintf(stderr, "\n[%ld bytes]\n", total);
	return 0;
}
