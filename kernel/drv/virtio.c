/*
 * virtio.c - VirtIO 1.0 block and network devices over PCI (QEMU's and
 * Proxmox's virtio-blk and virtio-net), the modern interface found through
 * the PCI capabilities. One split virtqueue per direction, polled like the
 * other drivers here.
 */
#include "../kernel.h"
#include "drv.h"
#include "../net/net.h"

#define VIRTIO_VENDOR 0x1AF4

/* device status */
#define S_ACK		1
#define S_DRIVER	2
#define S_DRIVER_OK	4
#define S_FEATURES_OK	8
#define S_FAILED	128

/* common configuration, from the capability with cfg_type 1 */
#define C_DFSELECT	0x00
#define C_DF		0x04
#define C_GFSELECT	0x08
#define C_GF		0x0C
#define C_NUMQ		0x12
#define C_STATUS	0x14
#define C_QSELECT	0x16
#define C_QSIZE		0x18
#define C_QMSIX		0x1A
#define C_QENABLE	0x1C
#define C_QNOTIFY	0x1E
#define C_QDESC		0x20
#define C_QDRIVER	0x28
#define C_QDEVICE	0x30

#define F_VERSION_1	32

struct vdesc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags, next;
} __attribute__((packed));
#define D_NEXT	1
#define D_WRITE	2

struct vq {
	int size, index;
	struct vdesc *desc;
	volatile uint16_t *avail;	/* flags, idx, ring[size] */
	volatile uint8_t *used;		/* flags, idx, {id32, len32}[size] */
	uint16_t last_used, next_avail;
	volatile uint16_t *notify;
};

struct vio {
	struct pci_dev *pd;
	volatile uint8_t *cfg, *dev, *notify_base;
	uint32_t notify_mul;
};

static uint8_t cfg8(struct pci_dev *d, int off)
{
	return (uint8_t)(pci_read32(d, (uint8_t)(off & ~3)) >> ((off & 3) * 8));
}

static uint32_t cfg32(struct pci_dev *d, int off)
{
	return pci_read32(d, (uint8_t)off);
}

static volatile uint8_t *cap_map(struct pci_dev *d, int cap)
{
	int bar = cfg8(d, cap + 4);
	uint32_t off = cfg32(d, cap + 8), len = cfg32(d, cap + 12);
	if (bar > 5)
		return NULL;
	volatile uint8_t *base = pci_map_bar(d, bar, (uint64_t)off + len);
	return base ? base + off : NULL;
}

/* find the structures through the vendor capabilities */
static int vio_probe(struct vio *v, struct pci_dev *d)
{
	v->pd = d;
	if (!(cfg32(d, 0x04) & (1u << 20)))	/* no capability list */
		return -1;
	volatile uint8_t *isr = NULL;
	for (int cap = cfg8(d, 0x34) & ~3, guard = 0; cap && guard < 48; cap = cfg8(d, cap + 1) & ~3, guard++) {
		if (cfg8(d, cap) != 0x09)
			continue;
		int type = cfg8(d, cap + 3);
		if (type == 1 && !v->cfg)
			v->cfg = cap_map(d, cap);
		else if (type == 2 && !v->notify_base) {
			v->notify_base = cap_map(d, cap);
			v->notify_mul = cfg32(d, cap + 16);
		} else if (type == 3 && !isr)
			isr = cap_map(d, cap);
		else if (type == 4 && !v->dev)
			v->dev = cap_map(d, cap);
	}
	return v->cfg && v->notify_base ? 0 : -1;
}

static inline uint8_t c8(struct vio *v, int o) { return v->cfg[o]; }
static inline uint16_t c16(struct vio *v, int o) { return *(volatile uint16_t *)(v->cfg + o); }
static inline uint32_t c32(struct vio *v, int o) { return *(volatile uint32_t *)(v->cfg + o); }
static inline void w8(struct vio *v, int o, uint8_t x) { v->cfg[o] = x; }
static inline void w16(struct vio *v, int o, uint16_t x) { *(volatile uint16_t *)(v->cfg + o) = x; }
static inline void w32(struct vio *v, int o, uint32_t x) { *(volatile uint32_t *)(v->cfg + o) = x; }
static inline void w64(struct vio *v, int o, uint64_t x)
{
	w32(v, o, (uint32_t)x);
	w32(v, o + 4, (uint32_t)(x >> 32));
}

/* reset, say hello, agree on features (want: the bits we can use) */
static int vio_start(struct vio *v, uint64_t want, uint64_t *got)
{
	w8(v, C_STATUS, 0);
	uint64_t until = uptime_ms() + 200;
	while (c8(v, C_STATUS) && uptime_ms() < until)
		cpu_pause();
	w8(v, C_STATUS, S_ACK);
	w8(v, C_STATUS, S_ACK | S_DRIVER);
	w32(v, C_DFSELECT, 0);
	uint64_t f = c32(v, C_DF);
	w32(v, C_DFSELECT, 1);
	f |= (uint64_t)c32(v, C_DF) << 32;
	if (!(f & (1ULL << F_VERSION_1))) {
		w8(v, C_STATUS, S_FAILED);
		return -1;
	}
	f &= want | (1ULL << F_VERSION_1);
	w32(v, C_GFSELECT, 0);
	w32(v, C_GF, (uint32_t)f);
	w32(v, C_GFSELECT, 1);
	w32(v, C_GF, (uint32_t)(f >> 32));
	w8(v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK);
	if (!(c8(v, C_STATUS) & S_FEATURES_OK)) {
		w8(v, C_STATUS, S_FAILED);
		return -1;
	}
	*got = f;
	return 0;
}

static int vq_setup(struct vio *v, struct vq *q, int index, int max)
{
	w16(v, C_QSELECT, (uint16_t)index);
	int size = c16(v, C_QSIZE);
	if (!size)
		return -1;
	size = MIN(size, max);
	/* a power of two, as split queues want */
	while (size & (size - 1))
		size &= size - 1;
	w16(v, C_QSIZE, (uint16_t)size);
	q->size = size;
	q->index = index;
	/* descriptors and the driver ring in one page, the device ring in another */
	uint8_t *p = pmm_alloc(1), *u = pmm_alloc(1);
	if (!p || !u)
		return -1;
	q->desc = (struct vdesc *)p;
	q->avail = (volatile uint16_t *)(p + 16 * size);
	q->used = u;
	w16(v, C_QMSIX, 0xFFFF);	/* no interrupts */
	w64(v, C_QDESC, (uint64_t)q->desc);
	w64(v, C_QDRIVER, (uint64_t)q->avail);
	w64(v, C_QDEVICE, (uint64_t)q->used);
	q->notify = (volatile uint16_t *)(v->notify_base + (uint32_t)c16(v, C_QNOTIFY) * v->notify_mul);
	w16(v, C_QENABLE, 1);
	return 0;
}

/* offer the chain starting at descriptor head */
static void vq_push(struct vq *q, uint16_t head)
{
	q->avail[2 + q->next_avail % q->size] = head;
	mmio_barrier();
	q->next_avail++;
	q->avail[1] = q->next_avail;
	mmio_barrier();
	*q->notify = (uint16_t)q->index;
}

static uint16_t vq_used_idx(struct vq *q)
{
	mmio_barrier();
	return *(volatile uint16_t *)(q->used + 2);
}

/* the next finished chain: its head and length, or -1 */
static int vq_pop(struct vq *q, uint32_t *len)
{
	if (q->last_used == vq_used_idx(q))
		return -1;
	volatile uint32_t *e = (volatile uint32_t *)(q->used + 4 + 8 * (q->last_used % q->size));
	int id = (int)e[0];
	if (len)
		*len = e[1];
	q->last_used++;
	return id;
}

/* ---- block ------------------------------------------------------------------------------ */

#define BLK_F_RO	5
#define BLK_F_FLUSH	9
#define BLK_MAX		128	/* sectors a request */

struct vblk {
	struct blkdev blk;
	struct vio v;
	struct vq q;
	uint8_t *hdr;		/* the request header, and the status byte at +16 */
	uint8_t *data;		/* BLK_MAX sectors */
	int flush, ro;
};

static int vblk_req(struct vblk *b, uint32_t type, uint64_t sector, uint32_t count)
{
	*(uint32_t *)b->hdr = type;
	*(uint32_t *)(b->hdr + 4) = 0;
	*(uint64_t *)(b->hdr + 8) = sector;
	b->hdr[16] = 0xFF;
	struct vdesc *d = b->q.desc;
	d[0] = (struct vdesc){ (uint64_t)b->hdr, 16, D_NEXT, 1 };
	int n = 1;
	if (count) {
		d[1] = (struct vdesc){ (uint64_t)b->data, count * 512, (uint16_t)(D_NEXT | (type == 0 ? D_WRITE : 0)), 2 };
		n = 2;
	}
	d[n - 1].next = (uint16_t)n;
	d[n] = (struct vdesc){ (uint64_t)(b->hdr + 16), 1, D_WRITE, 0 };
	vq_push(&b->q, 0);
	uint64_t until = uptime_ms() + 10000;
	int id;
	while ((id = vq_pop(&b->q, NULL)) < 0) {
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
	return b->hdr[16] == 0 ? 0 : -1;
}

static int vblk_read(struct blkdev *bd, uint64_t lba, uint32_t count, void *buf)
{
	struct vblk *b = bd->priv;
	if (lba + count > bd->sectors)
		return -1;
	uint8_t *out = buf;
	while (count) {
		uint32_t n = MIN(count, BLK_MAX);
		if (vblk_req(b, 0, lba, n))
			return -1;
		memcpy(out, b->data, (size_t)n * 512);
		out += (size_t)n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

static int vblk_write(struct blkdev *bd, uint64_t lba, uint32_t count, const void *buf)
{
	struct vblk *b = bd->priv;
	if (b->ro || lba + count > bd->sectors)
		return -1;
	const uint8_t *in = buf;
	while (count) {
		uint32_t n = MIN(count, BLK_MAX);
		memcpy(b->data, in, (size_t)n * 512);
		if (vblk_req(b, 1, lba, n))
			return -1;
		in += (size_t)n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

static int vblk_flush(struct blkdev *bd)
{
	struct vblk *b = bd->priv;
	return b->flush ? vblk_req(b, 4, 0, 0) : 0;
}

void virtio_blk_init(void)
{
	static int ndisks;
	for (int k = 0; k < 2; k++)
		for (int i = 0;; i++) {
			struct pci_dev *d = pci_find_id(VIRTIO_VENDOR, k ? 0x1042 : 0x1001, i);
			if (!d)
				break;
			pci_enable(d);
			struct vblk *b = kzalloc(sizeof *b);
			uint64_t got;
			if (!b || vio_probe(&b->v, d) || !b->v.dev ||
			    vio_start(&b->v, (1ULL << BLK_F_FLUSH) | (1ULL << BLK_F_RO), &got) ||
			    vq_setup(&b->v, &b->q, 0, 16)) {
				kfree(b);
				continue;
			}
			b->flush = (got >> BLK_F_FLUSH) & 1;
			b->ro = (got >> BLK_F_RO) & 1;
			b->hdr = pmm_alloc(1);
			b->data = pmm_alloc(BLK_MAX * 512 / PAGE_SIZE);
			if (!b->hdr || !b->data)
				continue;
			w8(&b->v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
			volatile uint32_t *cap = (volatile uint32_t *)b->v.dev;
			b->blk.sectors = (uint64_t)cap[0] | (uint64_t)cap[1] << 32;
			snprintf(b->blk.name, sizeof b->blk.name, "vd%c", 'a' + ndisks++);
			strcpy(b->blk.model, b->ro ? "VirtIO disk (read only)" : "VirtIO disk");
			b->blk.read = vblk_read;
			b->blk.write = vblk_write;
			b->blk.flush = vblk_flush;
			b->blk.priv = b;
			blk_register(&b->blk);
		}
}

/* ---- SCSI (Proxmox's default disk controller: "VirtIO SCSI single") --------------------- */

#define SCSI_CDB	32
#define SCSI_SENSE	96

struct vscsi_hba {
	struct vio v;
	struct vq q;		/* the first request queue */
	uint8_t *req, *resp;	/* one request at a time */
	uint8_t *data;		/* BLK_MAX sectors */
};

struct vscsi_disk {
	struct blkdev blk;
	struct vscsi_hba *h;
	int target, lun;
};

/* one command: 0 good, -1 failed (sense key in *key if there was sense) */
static int vscsi_cmd(struct vscsi_hba *h, int target, int lun, const uint8_t *cdb, int cdblen,
    uint32_t len, int dir_in, int *key)
{
	memset(h->req, 0, 51);
	h->req[0] = 1;
	h->req[1] = (uint8_t)target;
	h->req[2] = (uint8_t)(0x40 | (lun >> 8));
	h->req[3] = (uint8_t)lun;
	memcpy(h->req + 19, cdb, (size_t)cdblen);
	memset(h->resp, 0, 12 + SCSI_SENSE);
	h->resp[11] = 0xFF;
	struct vdesc *d = h->q.desc;
	d[0] = (struct vdesc){ (uint64_t)h->req, 19 + SCSI_CDB, D_NEXT, 1 };
	if (len && !dir_in) {	/* data out comes before the response */
		d[1] = (struct vdesc){ (uint64_t)h->data, len, D_NEXT, 2 };
		d[2] = (struct vdesc){ (uint64_t)h->resp, 12 + SCSI_SENSE, D_WRITE, 0 };
	} else if (len) {
		d[0].next = 1;
		d[1] = (struct vdesc){ (uint64_t)h->resp, 12 + SCSI_SENSE, D_WRITE | D_NEXT, 2 };
		d[2] = (struct vdesc){ (uint64_t)h->data, len, D_WRITE, 0 };
	} else {
		d[1] = (struct vdesc){ (uint64_t)h->resp, 12 + SCSI_SENSE, D_WRITE, 0 };
	}
	vq_push(&h->q, 0);
	uint64_t until = uptime_ms() + 10000;
	while (vq_pop(&h->q, NULL) < 0) {
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
	if (key)
		*key = -1;
	if (h->resp[11] != 0)	/* the transport: no such target, reset... */
		return -2;
	if (h->resp[10] == 0)	/* GOOD */
		return 0;
	if (key && *(uint32_t *)h->resp >= 3)
		*key = h->resp[12 + 2] & 15;
	return -1;
}

/* again after a UNIT ATTENTION (the first command after a reset gets one) */
static int vscsi_do(struct vscsi_hba *h, int target, int lun, const uint8_t *cdb, int cdblen, uint32_t len,
    int dir_in)
{
	int key, r = -1;
	for (int tries = 0; tries < 4; tries++) {
		r = vscsi_cmd(h, target, lun, cdb, cdblen, len, dir_in, &key);
		if (r != -1 || (key != 6 && key != 2))	/* UNIT ATTENTION, NOT READY: once more */
			break;
	}
	return r;
}

static int vscsi_rw(struct vscsi_disk *s, int write, uint64_t lba, uint32_t n)
{
	uint8_t cdb[16] = { (uint8_t)(write ? 0x8A : 0x88) };
	for (int i = 0; i < 8; i++)
		cdb[2 + i] = (uint8_t)(lba >> (56 - 8 * i));
	cdb[10] = (uint8_t)(n >> 24);
	cdb[11] = (uint8_t)(n >> 16);
	cdb[12] = (uint8_t)(n >> 8);
	cdb[13] = (uint8_t)n;
	return vscsi_do(s->h, s->target, s->lun, cdb, 16, n * 512, !write) ? -1 : 0;
}

static int vscsi_read(struct blkdev *bd, uint64_t lba, uint32_t count, void *buf)
{
	struct vscsi_disk *s = bd->priv;
	if (lba + count > bd->sectors)
		return -1;
	uint8_t *out = buf;
	while (count) {
		uint32_t n = MIN(count, BLK_MAX);
		if (vscsi_rw(s, 0, lba, n))
			return -1;
		memcpy(out, s->h->data, (size_t)n * 512);
		out += (size_t)n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

static int vscsi_write(struct blkdev *bd, uint64_t lba, uint32_t count, const void *buf)
{
	struct vscsi_disk *s = bd->priv;
	if (lba + count > bd->sectors)
		return -1;
	const uint8_t *in = buf;
	while (count) {
		uint32_t n = MIN(count, BLK_MAX);
		memcpy(s->h->data, in, (size_t)n * 512);
		if (vscsi_rw(s, 1, lba, n))
			return -1;
		in += (size_t)n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

static int vscsi_flush(struct blkdev *bd)
{
	struct vscsi_disk *s = bd->priv;
	uint8_t cdb[10] = { 0x35 };	/* SYNCHRONIZE CACHE */
	return vscsi_do(s->h, s->target, s->lun, cdb, 10, 0, 0) ? -1 : 0;
}

static int nscsi;

static void vscsi_probe_lun(struct vscsi_hba *h, int target, int lun)
{
	uint8_t inq[6] = { 0x12, 0, 0, 0, 96, 0 };	/* INQUIRY */
	if (vscsi_do(h, target, lun, inq, 6, 96, 1))
		return;
	int type = h->data[0] & 0x1F, qual = h->data[0] >> 5;
	if (qual || type != 0)		/* disks only (a CD-ROM is the firmware's business) */
		return;
	char model[41];
	int o = 0;
	for (int i = 8; i < 36 && o < 40; i++)	/* vendor and product */
		model[o++] = (char)h->data[i];
	model[o] = 0;
	while (o > 0 && model[o - 1] == ' ')
		model[--o] = 0;
	uint8_t tur[6] = { 0 };
	vscsi_do(h, target, lun, tur, 6, 0, 0);
	uint8_t rc16[16] = { 0x9E, 0x10 };	/* READ CAPACITY (16) */
	rc16[13] = 32;
	uint64_t last = 0;
	uint32_t bsize = 0;
	if (!vscsi_do(h, target, lun, rc16, 16, 32, 1)) {
		for (int i = 0; i < 8; i++)
			last = last << 8 | h->data[i];
		bsize = (uint32_t)h->data[8] << 24 | (uint32_t)h->data[9] << 16 | (uint32_t)h->data[10] << 8 | h->data[11];
	} else {
		uint8_t rc10[10] = { 0x25 };
		if (vscsi_do(h, target, lun, rc10, 10, 8, 1))
			return;
		last = (uint32_t)h->data[0] << 24 | (uint32_t)h->data[1] << 16 | (uint32_t)h->data[2] << 8 | h->data[3];
		bsize = (uint32_t)h->data[4] << 24 | (uint32_t)h->data[5] << 16 | (uint32_t)h->data[6] << 8 | h->data[7];
	}
	if (bsize != 512) {
		kprintf("virtio-scsi: target %d has %u-byte blocks: not supported\n", target, bsize);
		return;
	}
	struct vscsi_disk *s = kzalloc(sizeof *s);
	if (!s)
		return;
	s->h = h;
	s->target = target;
	s->lun = lun;
	snprintf(s->blk.name, sizeof s->blk.name, "sd%c", 'a' + nscsi++);
	s->blk.sectors = last + 1;
	strncpy0(s->blk.model, model[0] ? model : "VirtIO SCSI disk", sizeof s->blk.model);
	s->blk.read = vscsi_read;
	s->blk.write = vscsi_write;
	s->blk.flush = vscsi_flush;
	s->blk.priv = s;
	blk_register(&s->blk);
}

void virtio_scsi_init(void)
{
	for (int k = 0; k < 2; k++)
		for (int i = 0;; i++) {
			struct pci_dev *d = pci_find_id(VIRTIO_VENDOR, k ? 0x1048 : 0x1004, i);
			if (!d)
				break;
			pci_enable(d);
			struct vscsi_hba *h = kzalloc(sizeof *h);
			uint64_t got;
			/* queue 0 is control, 1 events, 2 the first for requests */
			if (!h || vio_probe(&h->v, d) || !h->v.dev || vio_start(&h->v, 0, &got) ||
			    vq_setup(&h->v, &h->q, 2, 16)) {
				kfree(h);
				continue;
			}
			h->req = pmm_alloc(1);
			h->resp = h->req + 1024;
			h->data = pmm_alloc(BLK_MAX * 512 / PAGE_SIZE);
			if (!h->req || !h->data)
				continue;
			w8(&h->v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
			volatile uint8_t *cfg = h->v.dev;
			int max_target = *(volatile uint16_t *)(cfg + 30);
			int max_lun = (int)*(volatile uint32_t *)(cfg + 32);
			/* the sizes we use are the defaults; say so in case they aren't */
			*(volatile uint32_t *)(cfg + 20) = SCSI_SENSE;
			*(volatile uint32_t *)(cfg + 24) = SCSI_CDB;
			for (int t = 0; t <= MIN(max_target, 15); t++)
				for (int l = 0; l <= MIN(max_lun, 7); l++)
					vscsi_probe_lun(h, t, l);
		}
}

/* ---- network ---------------------------------------------------------------------------- */

#define NET_F_MAC	5
#define NET_HDR		12	/* struct virtio_net_hdr, with num_buffers (VERSION_1) */
#define NBUF		2048
#define NRXQ		64
#define NTXQ		64

struct vnet {
	struct netif nif;
	struct vio v;
	struct vq rx, tx;
	uint8_t *rxbuf, *txbuf;
	int tx_free, tx_next;
};

static void rx_give(struct vnet *n, int i)
{
	n->rx.desc[i] = (struct vdesc){ (uint64_t)(n->rxbuf + (size_t)i * NBUF), NBUF, D_WRITE, 0 };
	vq_push(&n->rx, (uint16_t)i);
}

static void tx_reclaim(struct vnet *n)
{
	while (vq_pop(&n->tx, NULL) >= 0)
		n->tx_free++;
}

static int vnet_send(struct netif *nif, const void *frame, size_t len)
{
	struct vnet *n = (struct vnet *)nif;
	if (len + NET_HDR > NBUF)
		return -1;
	uint64_t until = uptime_ms() + 100;
	tx_reclaim(n);
	while (!n->tx_free) {
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
		tx_reclaim(n);
	}
	int i = n->tx_next;
	n->tx_next = (n->tx_next + 1) % n->tx.size;
	n->tx_free--;
	uint8_t *buf = n->txbuf + (size_t)i * NBUF;
	memset(buf, 0, NET_HDR);
	memcpy(buf + NET_HDR, frame, len);
	n->tx.desc[i] = (struct vdesc){ (uint64_t)buf, (uint32_t)(len + NET_HDR), 0, 0 };
	vq_push(&n->tx, (uint16_t)i);
	return 0;
}

static int vnet_recv(struct netif *nif, void *buf, size_t cap)
{
	struct vnet *n = (struct vnet *)nif;
	uint32_t len;
	int id = vq_pop(&n->rx, &len);
	if (id < 0)
		return 0;
	if (id >= n->rx.size)
		return -1;
	int got = len > NET_HDR ? (int)MIN(len - NET_HDR, cap) : 0;
	memcpy(buf, n->rxbuf + (size_t)id * NBUF + NET_HDR, (size_t)got);
	rx_give(n, id);
	return got ? got : -1;
}

static int vnet_link(struct netif *nif)
{
	(void)nif;
	return 1;	/* no STATUS feature asked for: always up */
}

void virtio_net_init(void)
{
	struct pci_dev *d = pci_find_id(VIRTIO_VENDOR, 0x1000, 0);
	if (!d)
		d = pci_find_id(VIRTIO_VENDOR, 0x1041, 0);
	if (!d)
		return;
	pci_enable(d);
	struct vnet *n = kzalloc(sizeof *n);
	uint64_t got;
	if (!n || vio_probe(&n->v, d) || vio_start(&n->v, 1ULL << NET_F_MAC, &got) ||
	    vq_setup(&n->v, &n->rx, 0, NRXQ) || vq_setup(&n->v, &n->tx, 1, NTXQ)) {
		kfree(n);
		return;
	}
	n->rxbuf = pmm_alloc((size_t)n->rx.size * NBUF / PAGE_SIZE);
	n->txbuf = pmm_alloc((size_t)n->tx.size * NBUF / PAGE_SIZE);
	if (!n->rxbuf || !n->txbuf)
		return;
	n->tx_free = n->tx.size;
	if ((got >> NET_F_MAC) & 1 && n->v.dev) {
		for (int i = 0; i < 6; i++)
			n->nif.mac[i] = n->v.dev[i];
	} else {	/* make one up: locally administered */
		uint64_t t = uptime_ms() ^ 0x5DEECE66DULL;
		n->nif.mac[0] = 0x52;
		for (int i = 1; i < 6; i++)
			n->nif.mac[i] = (uint8_t)(t >> (i * 7));
	}
	w8(&n->v, C_STATUS, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
	for (int i = 0; i < n->rx.size; i++)
		rx_give(n, i);
	strcpy(n->nif.name, "virtio");
	n->nif.send = vnet_send;
	n->nif.recv = vnet_recv;
	n->nif.link_up = vnet_link;
	net_register(&n->nif);
}
