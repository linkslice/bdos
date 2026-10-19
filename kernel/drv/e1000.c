/*
 * e1000.c - Intel 8254x/82574 gigabit Ethernet (QEMU's e1000, many real
 * PCs). Legacy descriptors, polled by the network task.
 */
#include "../kernel.h"
#include "drv.h"
#include "../net/net.h"

#define CTRL	0x0000
#define STATUS	0x0008
#define EERD	0x0014
#define ICR	0x00C0
#define IMC	0x00D8
#define RCTL	0x0100
#define TCTL	0x0400
#define TIPG	0x0410
#define RDBAL	0x2800
#define RDBAH	0x2804
#define RDLEN	0x2808
#define RDH	0x2810
#define RDT	0x2818
#define TDBAL	0x3800
#define TDBAH	0x3804
#define TDLEN	0x3808
#define TDH	0x3810
#define TDT	0x3818
#define MTA	0x5200
#define RAL0	0x5400
#define RAH0	0x5404

#define NRX	64
#define NTX	64
#define BUFSZ	2048

struct rxd {
	uint64_t addr;
	uint16_t len, csum;
	uint8_t status, errors;
	uint16_t special;
} __attribute__((packed));

struct txd {
	uint64_t addr;
	uint16_t len;
	uint8_t cso, cmd, status, css;
	uint16_t special;
} __attribute__((packed));

struct e1000 {
	struct netif nif;
	volatile uint32_t *regs;
	struct rxd *rx;
	struct txd *tx;
	uint8_t *rxbuf, *txbuf;
	int rx_next, tx_next;
};

static inline uint32_t rd(struct e1000 *e, int r) { return e->regs[r / 4]; }
static inline void wr(struct e1000 *e, int r, uint32_t v) { e->regs[r / 4] = v; }

static int e1000_send(struct netif *nif, const void *frame, size_t len)
{
	struct e1000 *e = (struct e1000 *)nif;
	if (len > BUFSZ)
		return -1;
	struct txd *d = &e->tx[e->tx_next];
	/* wait for the slot to be free (DD set, or never used) */
	uint64_t until = uptime_ms() + 100;
	while (d->cmd && !(d->status & 1)) {
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
	uint8_t *buf = e->txbuf + (size_t)e->tx_next * BUFSZ;
	memcpy(buf, frame, len);
	if (len < 60) {
		memset(buf + len, 0, 60 - len);
		len = 60;
	}
	d->addr = (uint64_t)buf;
	d->len = (uint16_t)len;
	d->status = 0;
	d->cmd = 0x01 | 0x02 | 0x08;	/* EOP, insert FCS, report status */
	e->tx_next = (e->tx_next + 1) % NTX;
	mmio_barrier();
	wr(e, TDT, (uint32_t)e->tx_next);
	return 0;
}

static int e1000_recv(struct netif *nif, void *buf, size_t cap)
{
	struct e1000 *e = (struct e1000 *)nif;
	struct rxd *d = &e->rx[e->rx_next];
	if (!(d->status & 1))
		return 0;
	int len = 0;
	if ((d->status & 2) && !d->errors) {	/* complete, good frame */
		len = MIN(d->len, (int)cap);
		memcpy(buf, e->rxbuf + (size_t)e->rx_next * BUFSZ, (size_t)len);
	}
	d->status = 0;
	int idx = e->rx_next;
	e->rx_next = (e->rx_next + 1) % NRX;
	mmio_barrier();
	wr(e, RDT, (uint32_t)idx);
	return len ? len : -1;	/* -1: consumed a bad frame, try again */
}

static int link_up(struct netif *nif)
{
	struct e1000 *e = (struct e1000 *)nif;
	return (rd(e, STATUS) & 2) != 0;
}

static const uint16_t ids[] = { 0x100E, 0x100F, 0x1004, 0x10D3, 0x153A, 0x10EA };

void e1000_init(void)
{
	struct pci_dev *pd = NULL;
	for (size_t i = 0; i < NELEM(ids) && !pd; i++)
		pd = pci_find_id(0x8086, ids[i], 0);
	if (!pd)
		return;
	pci_enable(pd);
	struct e1000 *e = kzalloc(sizeof *e);
	if (!e)
		return;
	e->regs = (volatile uint32_t *)pci_bar_mem(pd, 0);

	wr(e, IMC, 0xFFFFFFFF);		/* no interrupts: we poll */
	wr(e, CTRL, rd(e, CTRL) | (1u << 26));	/* reset */
	uint64_t until = uptime_ms() + 50;
	while (uptime_ms() < until)
		cpu_pause();
	wr(e, IMC, 0xFFFFFFFF);
	(void)rd(e, ICR);
	wr(e, CTRL, (rd(e, CTRL) | (1u << 6) | (1u << 5)) & ~(1u << 3));	/* SLU, ASDE, no LRST */

	uint32_t ral = rd(e, RAL0), rah = rd(e, RAH0);
	for (int i = 0; i < 4; i++)
		e->nif.mac[i] = (uint8_t)(ral >> (i * 8));
	e->nif.mac[4] = (uint8_t)rah;
	e->nif.mac[5] = (uint8_t)(rah >> 8);
	wr(e, RAH0, rah | (1u << 31));	/* address valid */
	for (int i = 0; i < 128; i++)
		wr(e, MTA + i * 4, 0);

	e->rx = pmm_alloc(1);
	e->tx = pmm_alloc(1);
	e->rxbuf = pmm_alloc(NRX * BUFSZ / PAGE_SIZE);
	e->txbuf = pmm_alloc(NTX * BUFSZ / PAGE_SIZE);
	if (!e->rx || !e->tx || !e->rxbuf || !e->txbuf)
		return;
	for (int i = 0; i < NRX; i++)
		e->rx[i].addr = (uint64_t)(e->rxbuf + (size_t)i * BUFSZ);
	wr(e, RDBAL, (uint32_t)(uint64_t)e->rx);
	wr(e, RDBAH, (uint32_t)((uint64_t)e->rx >> 32));
	wr(e, RDLEN, NRX * sizeof(struct rxd));
	wr(e, RDH, 0);
	wr(e, RDT, NRX - 1);
	/* enable, accept broadcast, 2 KiB buffers, strip CRC */
	wr(e, RCTL, (1u << 1) | (1u << 15) | (1u << 26));

	wr(e, TDBAL, (uint32_t)(uint64_t)e->tx);
	wr(e, TDBAH, (uint32_t)((uint64_t)e->tx >> 32));
	wr(e, TDLEN, NTX * sizeof(struct txd));
	wr(e, TDH, 0);
	wr(e, TDT, 0);
	wr(e, TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));
	wr(e, TIPG, 0x0060200A);

	strcpy(e->nif.name, "e1000");
	e->nif.send = e1000_send;
	e->nif.recv = e1000_recv;
	e->nif.link_up = link_up;
	net_register(&e->nif);
}
