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

/* servers: called in a fresh task for each accepted connection */
typedef void (*service_fn)(struct conn *c, const uint8_t *hello, size_t hlen);
void net_service(int svc, service_fn fn);

#define MSG(t) (1u << (t))

#endif
