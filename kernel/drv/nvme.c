/*
 * nvme.c - NVM Express disks: what most PCs since about 2016 boot from.
 *
 * One admin queue pair to set things up, one I/O queue pair to read and
 * write, polled. Every namespace becomes a block device in 512-byte
 * sectors; 4 KB-sector namespaces are read and written through a bounce
 * buffer a block at a time.
 */
#include "../kernel.h"
#include "drv.h"

#define CAP	0x00
#define VS	0x08
#define CC	0x14
#define CSTS	0x1C
#define AQA	0x24
#define ASQ	0x28
#define ACQ	0x30

#define QSIZE	32
#define MAX_XFER (64 * 1024)		/* bytes per command: PRP list of 16 pages */

struct sqe {
	uint8_t opc, flags;
	uint16_t cid;
	uint32_t nsid;
	uint64_t rsvd;
	uint64_t mptr;
	uint64_t prp1, prp2;
	uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
	uint32_t dw0, dw1;
	uint16_t sq_head, sq_id;
	uint16_t cid, status;
};

struct queue {
	struct sqe *sq;
	volatile struct cqe *cq;
	uint16_t sq_tail, cq_head;
	int phase;
	volatile uint32_t *sq_db, *cq_db;
};

struct nvme {
	volatile uint8_t *regs;
	struct queue admin, io;
	uint16_t cid;
	uint8_t *bounce;		/* MAX_XFER bytes */
	uint64_t *prp_list;		/* one page */
	int timeout_ms;
};

struct nvns {
	struct blkdev blk;
	struct nvme *c;
	uint32_t nsid;
	uint32_t lba_shift;		/* 9 or 12 */
};

static uint32_t r32(struct nvme *c, int o) { return *(volatile uint32_t *)(c->regs + o); }
static uint64_t r64(struct nvme *c, int o) { return *(volatile uint64_t *)(c->regs + o); }
static void w32(struct nvme *c, int o, uint32_t v) { *(volatile uint32_t *)(c->regs + o) = v; }
static void w64(struct nvme *c, int o, uint64_t v) { *(volatile uint64_t *)(c->regs + o) = v; }

/* submit one command and wait for it; returns the NVMe status (0 is success) */
static int submit(struct nvme *c, struct queue *q, struct sqe *cmd, uint32_t *dw0)
{
	cmd->cid = ++c->cid;
	q->sq[q->sq_tail] = *cmd;
	q->sq_tail = (uint16_t)((q->sq_tail + 1) % QSIZE);
	mmio_barrier();
	*q->sq_db = q->sq_tail;
	uint64_t until = uptime_ms() + (uint64_t)c->timeout_ms;
	for (;;) {
		volatile struct cqe *e = &q->cq[q->cq_head];
		if ((e->status & 1) == q->phase) {
			int st = (e->status >> 1) & 0x7FFF;
			if (dw0)
				*dw0 = e->dw0;
			q->cq_head = (uint16_t)((q->cq_head + 1) % QSIZE);
			if (!q->cq_head)
				q->phase ^= 1;
			*q->cq_db = q->cq_head;
			return st;
		}
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
}

static int queue_init(struct nvme *c, struct queue *q, int qid)
{
	uint8_t *mem = pmm_alloc(2);
	if (!mem)
		return -1;
	q->sq = (struct sqe *)mem;
	q->cq = (volatile struct cqe *)(mem + 4096);
	q->sq_tail = q->cq_head = 0;
	q->phase = 1;
	int stride = 4 << ((r64(c, CAP) >> 32) & 0xF);
	q->sq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * qid) * stride);
	q->cq_db = (volatile uint32_t *)(c->regs + 0x1000 + (2 * qid + 1) * stride);
	return 0;
}

/* PRPs for a transfer from the bounce buffer */
static void prps(struct nvme *c, struct sqe *cmd, size_t bytes)
{
	cmd->prp1 = (uint64_t)c->bounce;
	if (bytes <= 4096) {
		cmd->prp2 = 0;
	} else if (bytes <= 8192) {
		cmd->prp2 = (uint64_t)c->bounce + 4096;
	} else {
		size_t pages = (bytes + 4095) / 4096;
		for (size_t i = 1; i < pages; i++)
			c->prp_list[i - 1] = (uint64_t)c->bounce + i * 4096;
		cmd->prp2 = (uint64_t)c->prp_list;
	}
}

static int rw_blocks(struct nvns *ns, int write, uint64_t lba, uint32_t blocks)
{
	struct sqe cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.opc = write ? 0x01 : 0x02;
	cmd.nsid = ns->nsid;
	prps(ns->c, &cmd, (size_t)blocks << ns->lba_shift);
	cmd.cdw10 = (uint32_t)lba;
	cmd.cdw11 = (uint32_t)(lba >> 32);
	cmd.cdw12 = blocks - 1;
	return submit(ns->c, &ns->c->io, &cmd, NULL) ? -1 : 0;
}

/* 512-byte sectors in and out of whatever the namespace's blocks are */
static int nv_io(struct blkdev *b, int write, uint64_t sector, uint32_t count, uint8_t *buf)
{
	struct nvns *ns = b->priv;
	struct nvme *c = ns->c;
	uint32_t per = 1u << (ns->lba_shift - 9);	/* sectors per block */
	while (count) {
		uint64_t lba = sector / per;
		uint32_t skip = (uint32_t)(sector % per);
		uint32_t max_sectors = MAX_XFER / 512;
		uint32_t n = MIN(count, max_sectors - skip);
		uint32_t blocks = (skip + n + per - 1) / per;
		size_t bytes = (size_t)blocks << ns->lba_shift;
		if (write) {
			if (skip || (n % per)) {	/* partial blocks: read them first */
				if (rw_blocks(ns, 0, lba, blocks))
					return -1;
			}
			memcpy(c->bounce + skip * 512, buf, (size_t)n * 512);
			if (rw_blocks(ns, 1, lba, blocks))
				return -1;
		} else {
			if (rw_blocks(ns, 0, lba, blocks))
				return -1;
			memcpy(buf, c->bounce + skip * 512, (size_t)n * 512);
		}
		(void)bytes;
		buf += (size_t)n * 512;
		sector += n;
		count -= n;
	}
	return 0;
}

static int nv_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf) { return nv_io(b, 0, lba, count, buf); }
static int nv_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf) { return nv_io(b, 1, lba, count, (uint8_t *)buf); }

static int nv_flush(struct blkdev *b)
{
	struct nvns *ns = b->priv;
	struct sqe cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.opc = 0x00;
	cmd.nsid = ns->nsid;
	return submit(ns->c, &ns->c->io, &cmd, NULL) ? -1 : 0;
}

static int ndisks;

static void controller_init(struct pci_dev *d)
{
	pci_enable(d);
	struct nvme *c = kzalloc(sizeof *c);
	if (!c)
		return;
	c->regs = pci_map_bar(d, 0, 0x4000);
	if (!c->regs)
		return;
	uint64_t cap = r64(c, CAP);
	c->timeout_ms = (int)((cap >> 24) & 0xFF) * 500 + 1000;
	/* off, admin queues, on */
	w32(c, CC, r32(c, CC) & ~1u);
	uint64_t until = uptime_ms() + (uint64_t)c->timeout_ms;
	while ((r32(c, CSTS) & 1) && uptime_ms() < until)
		cpu_pause();
	if (queue_init(c, &c->admin, 0) || queue_init(c, &c->io, 1))
		return;
	c->bounce = pmm_alloc(MAX_XFER / 4096);
	c->prp_list = pmm_alloc(1);
	uint8_t *id = pmm_alloc(1);
	if (!c->bounce || !c->prp_list || !id)
		return;
	w32(c, AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
	w64(c, ASQ, (uint64_t)c->admin.sq);
	w64(c, ACQ, (uint64_t)c->admin.cq);
	w32(c, CC, 6 << 16 | 4 << 20 | 1);	/* 64-byte SQ entries, 16-byte CQ entries, 4 KB pages, on */
	until = uptime_ms() + (uint64_t)c->timeout_ms;
	while (!(r32(c, CSTS) & 1)) {
		if ((r32(c, CSTS) & 2) || uptime_ms() > until) {
			kprintf("nvme: controller didn't start\n");
			return;
		}
		cpu_pause();
	}
	/* who's there */
	struct sqe cmd;
	memset(&cmd, 0, sizeof cmd);
	cmd.opc = 0x06;		/* identify */
	cmd.prp1 = (uint64_t)id;
	cmd.cdw10 = 1;		/* the controller */
	if (submit(c, &c->admin, &cmd, NULL))
		return;
	char model[41];
	for (int i = 0; i < 40; i++)
		model[i] = (char)id[24 + i];
	model[40] = 0;
	for (int i = 39; i >= 0 && model[i] == ' '; i--)
		model[i] = 0;
	uint32_t nn = *(uint32_t *)(id + 516);
	/* the I/O queues */
	memset(&cmd, 0, sizeof cmd);
	cmd.opc = 0x05;		/* create I/O completion queue */
	cmd.prp1 = (uint64_t)c->io.cq;
	cmd.cdw10 = (QSIZE - 1) << 16 | 1;
	cmd.cdw11 = 1;		/* physically contiguous, no interrupts */
	if (submit(c, &c->admin, &cmd, NULL))
		return;
	memset(&cmd, 0, sizeof cmd);
	cmd.opc = 0x01;		/* create I/O submission queue */
	cmd.prp1 = (uint64_t)c->io.sq;
	cmd.cdw10 = (QSIZE - 1) << 16 | 1;
	cmd.cdw11 = 1 << 16 | 1;	/* completions to queue 1, contiguous */
	if (submit(c, &c->admin, &cmd, NULL))
		return;
	for (uint32_t nsid = 1; nsid <= nn && nsid <= 16; nsid++) {
		memset(&cmd, 0, sizeof cmd);
		cmd.opc = 0x06;
		cmd.nsid = nsid;
		cmd.prp1 = (uint64_t)id;
		cmd.cdw10 = 0;	/* a namespace */
		if (submit(c, &c->admin, &cmd, NULL))
			continue;
		uint64_t blocks = *(uint64_t *)id;
		if (!blocks)
			continue;
		int flbas = id[26] & 15;
		uint32_t lbaf = *(uint32_t *)(id + 128 + flbas * 4);
		uint32_t shift = (lbaf >> 16) & 0xFF;
		if (shift < 9 || shift > 12) {
			kprintf("nvme: namespace %u has %u-byte blocks: not supported\n", nsid, 1u << shift);
			continue;
		}
		struct nvns *ns = kzalloc(sizeof *ns);
		if (!ns)
			continue;
		ns->c = c;
		ns->nsid = nsid;
		ns->lba_shift = shift;
		snprintf(ns->blk.name, sizeof ns->blk.name, "nvme%d", ndisks++);
		ns->blk.sectors = blocks << (shift - 9);
		ns->blk.read = nv_read;
		ns->blk.write = nv_write;
		ns->blk.flush = nv_flush;
		ns->blk.priv = ns;
		strncpy0(ns->blk.model, model, sizeof ns->blk.model);
		blk_register(&ns->blk);
	}
	pmm_free(id, 1);
}

void nvme_init(void)
{
	for (int i = 0;; i++) {
		struct pci_dev *d = pci_find_class(0x01, 0x08, 0x02, i);
		if (!d)
			break;
		controller_init(d);
	}
}
