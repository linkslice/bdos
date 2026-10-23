/*
 * net.h - bdos networking: BDNET, a small LAN protocol straight on
 * Ethernet (ethertype 0x88B5), in the spirit of NetBEUI.
 *
 *   names       WHO/IAM broadcasts find "\\BOX2" on the local network
 *   connections reliable, ordered message streams (go-back-N)
 *   services    TTY (CTTY \\HOST), DESK (WIN \\HOST), FILE (NET USE)
 *
 * No IP: it works on a switched LAN, not across routers.
 */
#ifndef BDOS_NET_H
#define BDOS_NET_H

#include <stdint.h>
#include <stddef.h>

struct netif {
	char name[8];
	uint8_t mac[6];
	int (*send)(struct netif *n, const void *frame, size_t len);
	int (*recv)(struct netif *n, void *buf, size_t cap);	/* >0 len, 0 none, -1 retry */
	int (*link_up)(struct netif *n);
};

void net_register(struct netif *n);
void net_init(void);
int net_present(void);
const char *net_adapter(void);
int net_link(void);
const char *net_hostname(void);
void net_set_hostname(const char *name);
void net_mac(uint8_t mac[6]);

/* name lookup: 0 on success */
int net_resolve(const char *name, uint8_t mac[6]);
/* broadcast discovery: calls cb for each machine heard within ms */
void net_view(int ms, void (*cb)(const char *name, const uint8_t mac[6], void *arg), void *arg);

/* connections carry messages: [u32 length][u8 type][payload] */
enum { SVC_TTY = 'T', SVC_DESK = 'D', SVC_FILE = 'F' };

struct netmsg {
	struct netmsg *next;
	int type;
	uint32_t len;
	uint8_t data[];
};

struct conn;

struct conn *conn_connect(const char *host, int svc, const void *hello, size_t hlen, int *err);
int conn_send(struct conn *c, int type, const void *data, size_t len);
int conn_send2(struct conn *c, int type, const void *a, size_t alen, const void *b, size_t blen);
/* take the first queued message whose type bit is set in mask (or NULL) */
struct netmsg *conn_take(struct conn *c, uint32_t mask);
/* same, waiting up to ms (or forever if ms < 0) */
struct netmsg *conn_wait(struct conn *c, uint32_t mask, int ms);
int conn_alive(struct conn *c);
void conn_close(struct conn *c);
const char *conn_peer(struct conn *c);
void *conn_user(struct conn *c);
void conn_set_user(struct conn *c, void *u);

/* TCP/IP (ip.c): enough to carry BDNET over UDP for bterm and routed peers */
struct ip_info {
	uint32_t ip, mask, gw, dns, server;
	int dhcp, bound, udp, port, password;
	uint64_t lease_left;	/* seconds */
};
void ip_init(struct netif *n);
int ip_input(const uint8_t *frame, int len);	/* 1 if it was ARP/IP */
void ip_timers(void);
int ip_is_pseudo(const uint8_t *mac);
int ip_bdnet_send(const uint8_t *mac, const void *frame, size_t len);
int ip_peer_addr(const uint8_t *mac, char *out, size_t n);
void ip_get(struct ip_info *info);
void ip_renew(void);
void ip_release(void);
int ip_ping(uint32_t dst, int seq, int timeout_ms);	/* ms, or -1 */
int ip_parse(const char *s, uint32_t *out);
void ip_fmt(uint32_t ip, char *out);			/* out: 16 bytes */
void bdnet_input(const uint8_t *frame, int len);	/* a BDNET frame from any wire */

/* IP for TCP and the resolver */
int ip_output(uint32_t dst, int proto, const void *payload, size_t len);
uint32_t ip_local(void);
uint32_t ip_dns(void);
uint16_t ip_csum_pseudo(uint32_t src, uint32_t dst, int proto, const uint8_t *p, size_t n);
typedef void (*udp_fn)(uint32_t src, uint16_t sport, const uint8_t *d, size_t n, void *arg);
int udp_bind(uint16_t port, udp_fn fn, void *arg);
void udp_unbind(uint16_t port);
int ip_udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, size_t len);

/* TCP (tcp.c). Everything here runs under the big kernel lock. */
struct tcp;
struct tcp *tcp_connect(uint32_t ip, uint16_t port, int timeout_ms, int *err);
struct tcp *tcp_listen(uint16_t port, int *err);
struct tcp *tcp_accept(struct tcp *l, int timeout_ms);	/* NULL on timeout */
long tcp_send(struct tcp *t, const void *buf, size_t n);	/* all of it, or an error */
long tcp_recv(struct tcp *t, void *buf, size_t n, int timeout_ms);	/* 0 at the end, E_TIMEDOUT */
int tcp_readable(struct tcp *t);	/* bytes waiting, or -1 at the end */
void tcp_close(struct tcp *t);
int tcp_peer(struct tcp *t, uint32_t *ip, uint16_t *port);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *p, size_t n);
void tcp_timers(void);
void tcp_list(void (*cb)(const char *line, void *arg), void *arg);

/* DNS (tcp.c): a name, or a dotted address, to an address; 0 on success */
int dns_resolve(const char *name, uint32_t *ip);

/* servers: called in a fresh task for each accepted connection */
typedef void (*service_fn)(struct conn *c, const uint8_t *hello, size_t hlen);
void net_service(int svc, service_fn fn);

#define MSG(t) (1u << (t))

#endif
