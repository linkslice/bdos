/*
 * ahci.c - SATA disks through an AHCI controller. Polled, one command
 * slot per port: simple and plenty fast for a single-user system.
 */
#include "../kernel.h"
#include "drv.h"

/* HBA registers */
#define HBA_CAP		0x00
#define HBA_GHC		0x04
#define HBA_PI		0x0C
#define GHC_AE		(1u << 31)

/* port registers (offset from the port base) */
#define P_CLB	0x00
#define P_CLBU	0x04
#define P_FB	0x08
#define P_FBU	0x0C
#define P_IS	0x10
#define P_IE	0x14
#define P_CMD	0x18
#define P_TFD	0x20
#define P_SIG	0x24
#define P_SSTS	0x28
#define P_SERR	0x30
#define P_CI	0x38

#define CMD_ST	(1u << 0)
#define CMD_FRE	(1u << 4)
#define CMD_FR	(1u << 14)
#define CMD_CR	(1u << 15)
#define TFD_BSY	0x80
#define TFD_DRQ	0x08
#define TFD_ERR	0x01
#define IS_TFES	(1u << 30)

#define ATA_READ_DMA_EXT	0x25
#define ATA_WRITE_DMA_EXT	0x35
#define ATA_FLUSH_EXT		0xEA
#define ATA_IDENTIFY		0xEC

#define MAX_XFER_SECTORS	8192	/* 4 MiB: one PRD entry */

struct cmd_header {
	uint16_t flags;		/* CFL in bits 0-4, W = bit 6 */
	uint16_t prdtl;
	uint32_t prdbc;
	uint64_t ctba;
	uint32_t rsvd[4];
};

struct prd {
	uint64_t dba;
	uint32_t rsvd;
	uint32_t dbc;		/* byte count - 1, bit 31 = interrupt */
};

struct cmd_table {
	uint8_t cfis[64];
	uint8_t acmd[16];
	uint8_t rsvd[48];
	struct prd prdt[1];
};

struct port {
	volatile uint32_t *regs;
	struct cmd_header *clist;
	struct cmd_table *table;
	uint8_t *bounce;	/* for odd-aligned buffers */
	struct blkdev blk;
};

static inline uint32_t rd(struct port *p, int off) { return p->regs[off / 4]; }
static inline void wr(struct port *p, int off, uint32_t v) { p->regs[off / 4] = v; }

static int wait_clear(struct port *p, int off, uint32_t bits, uint64_t ms)
{
	uint64_t until = uptime_ms() + ms;
	while (rd(p, off) & bits) {
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
	return 0;
}

static int issue(struct port *p, uint8_t cmd, uint64_t lba, uint32_t count,
    void *buf, uint32_t bytes, int write)
{
	if (wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, 2000))
		return -1;

	struct cmd_header *h = &p->clist[0];
	memset(h, 0, sizeof *h);
	h->flags = (uint16_t)(5 | (write ? 1 << 6 : 0));	/* 5-dword FIS */
	h->prdtl = bytes ? 1 : 0;
	h->ctba = (uint64_t)p->table;

	struct cmd_table *t = p->table;
	memset(t, 0, sizeof *t);
	if (bytes) {
		t->prdt[0].dba = (uint64_t)buf;
		t->prdt[0].dbc = bytes - 1;
	}
	uint8_t *f = t->cfis;
	f[0] = 0x27;		/* register FIS, host to device */
	f[1] = 0x80;		/* this is a command */
	f[2] = cmd;
	f[4] = (uint8_t)lba;
	f[5] = (uint8_t)(lba >> 8);
	f[6] = (uint8_t)(lba >> 16);
	f[7] = 1 << 6;		/* LBA mode */
	f[8] = (uint8_t)(lba >> 24);
	f[9] = (uint8_t)(lba >> 32);
	f[10] = (uint8_t)(lba >> 40);
	f[12] = (uint8_t)count;
	f[13] = (uint8_t)(count >> 8);

	wr(p, P_IS, 0xFFFFFFFF);
	mmio_barrier();
	wr(p, P_CI, 1);

	uint64_t until = uptime_ms() + BLK_TIMEOUT_MS;
	while (rd(p, P_CI) & 1) {
		if (rd(p, P_IS) & IS_TFES)
			return -1;
		if (uptime_ms() > until)
			return blk_timeout();
		cpu_pause();
	}
	mmio_barrier();
	if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & TFD_ERR))
		return -1;
	return 0;
}

static int xfer(struct blkdev *b, uint64_t lba, uint32_t count, void *buf, int write)
{
	struct port *p = b->priv;
	uint8_t *ptr = buf;
	while (count) {
		uint32_t n = MIN(count, MAX_XFER_SECTORS);
		int odd = ((uint64_t)ptr & 1) != 0;
		uint8_t *io = ptr;
		if (odd) {
			n = MIN(n, 8);	/* bounce buffer is one page */
			io = p->bounce;
			if (write)
				memcpy(io, ptr, n * 512);
		}
		if (issue(p, write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT, lba, n,
		    io, n * 512, write))
			return -1;
		if (odd && !write)
			memcpy(ptr, io, n * 512);
		lba += n;
		count -= n;
		ptr += n * 512;
	}
	return 0;
}

static int ahci_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
	if (lba + count > b->sectors)
		return -1;
	return xfer(b, lba, count, buf, 0);
}

static int ahci_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
	if (lba + count > b->sectors)
		return -1;
	return xfer(b, lba, count, (void *)buf, 1);
}

static int ahci_flush(struct blkdev *b)
{
	return issue(b->priv, ATA_FLUSH_EXT, 0, 0, NULL, 0, 0);
}

static void port_stop(struct port *p)
{
	wr(p, P_CMD, rd(p, P_CMD) & ~CMD_ST);
	wait_clear(p, P_CMD, CMD_CR, 500);
	wr(p, P_CMD, rd(p, P_CMD) & ~CMD_FRE);
	wait_clear(p, P_CMD, CMD_FR, 500);
}

static void port_start(struct port *p)
{
	wait_clear(p, P_CMD, CMD_CR, 500);
	wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
	wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
}

static int ndisks;

static void port_init(volatile uint8_t *abar, int n)
{
	volatile uint32_t *regs = (volatile uint32_t *)(abar + 0x100 + n * 0x80);
	uint32_t ssts = regs[P_SSTS / 4];
	if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
		return;		/* no device, or not active */
	if (regs[P_SIG / 4] != 0x00000101)
		return;		/* not a plain SATA disk (ATAPI, port multiplier) */

	struct port *p = kzalloc(sizeof *p);
	uint8_t *mem = pmm_alloc(2);
	if (!p || !mem)
		return;
	p->regs = regs;
	port_stop(p);
	p->clist = (struct cmd_header *)mem;		/* 1 KiB aligned */
	uint8_t *fis = mem + 1024;			/* 256-byte aligned */
	p->table = (struct cmd_table *)(mem + 2048);	/* 128-byte aligned */
	p->bounce = mem + 4096;
	wr(p, P_CLB, (uint32_t)(uint64_t)p->clist);
	wr(p, P_CLBU, (uint32_t)((uint64_t)p->clist >> 32));
	wr(p, P_FB, (uint32_t)(uint64_t)fis);
	wr(p, P_FBU, (uint32_t)((uint64_t)fis >> 32));
	wr(p, P_SERR, 0xFFFFFFFF);
	wr(p, P_IS, 0xFFFFFFFF);
	wr(p, P_IE, 0);		/* we poll */
	port_start(p);

	uint16_t *id = (uint16_t *)p->bounce;
	if (issue(p, ATA_IDENTIFY, 0, 0, id, 512, 0)) {
		kprintf("ahci: port %d: IDENTIFY failed\n", n);
		return;
	}
	uint64_t sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 |
	    (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48;
	if (!sectors)
		sectors = (uint64_t)id[60] | (uint64_t)id[61] << 16;
	for (int i = 0; i < 20; i++) {		/* model: byte-swapped words */
		p->blk.model[i * 2] = (char)(id[27 + i] >> 8);
		p->blk.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
	}
	for (int i = 39; i >= 0 && p->blk.model[i] == ' '; i--)
		p->blk.model[i] = 0;

	snprintf(p->blk.name, sizeof p->blk.name, "disk%d", ndisks++);
	p->blk.sectors = sectors;
	p->blk.read = ahci_read;
	p->blk.write = ahci_write;
	p->blk.flush = ahci_flush;
	p->blk.priv = p;
	blk_register(&p->blk);
}

void ahci_init(void)
{
	for (int idx = 0;; idx++) {
		struct pci_dev *d = pci_find_class(0x01, 0x06, 0x01, idx);
		if (!d)
			break;
		pci_enable(d);
		volatile uint8_t *abar = pci_map_bar(d, 5, 0x2000);
		if (!abar)
			continue;
		volatile uint32_t *hba = (volatile uint32_t *)abar;
		hba[HBA_GHC / 4] |= GHC_AE;
		uint32_t pi = hba[HBA_PI / 4];
		for (int n = 0; n < 32; n++)
			if (pi & (1u << n))
				port_init(abar, n);
	}
}

/* ---- block device registry and RAM disks ---- */

static struct blkdev *blks[16];
static int nblks;

/* Disk requests are made one at a time (under the big lock), so one flag does. */
static int timed_out;

int blk_timeout(void)
{
	timed_out = 1;
	return -1;
}

static void gave_up(struct blkdev *b)
{
	b->failed = 1;
	kprintf("\n%s (%s) didn't answer for %d seconds, so bdos has stopped using it until\n"
	    "it restarts: nothing more is read from it or written to it. Its drive letters\n"
	    "now report errors. Check the disk (or the VM host's storage), then restart.\n",
	    b->name, b->model, BLK_TIMEOUT_MS / 1000);
}

static int g_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
	if (b->failed)
		return -1;
	timed_out = 0;
	int r = b->drv_read(b, lba, count, buf);
	if (r && timed_out)
		gave_up(b);
	return r;
}

static int g_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
	if (b->failed)
		return -1;
	timed_out = 0;
	int r = b->drv_write(b, lba, count, buf);
	if (r && timed_out)
		gave_up(b);
	return r;
}

static int g_flush(struct blkdev *b)
{
	if (b->failed)
		return -1;
	if (!b->drv_flush)
		return 0;
	timed_out = 0;
	int r = b->drv_flush(b);
	if (r && timed_out)
		gave_up(b);
	return r;
}

void blk_register(struct blkdev *b)
{
	if (nblks >= (int)NELEM(blks))
		return;
	b->drv_read = b->read;
	b->drv_write = b->write;
	b->drv_flush = b->flush;
	b->read = g_read;
	b->write = g_write;
	b->flush = g_flush;
	blks[nblks++] = b;
}

struct blkdev *blk_get(int i) { return i < nblks ? blks[i] : NULL; }
int blk_count(void) { return nblks; }

static int ram_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf)
{
	if (lba + count > b->sectors)
		return -1;
	memcpy(buf, (uint8_t *)b->priv + lba * 512, (size_t)count * 512);
	return 0;
}

static int ram_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf)
{
	if (lba + count > b->sectors)
		return -1;
	memcpy((uint8_t *)b->priv + lba * 512, buf, (size_t)count * 512);
	return 0;
}

/* A RAM disk over memory we own (the loader's ROOTFS.IMG): used in place. */
struct blkdev *ramdisk_create(const char *name, const void *image, uint64_t bytes)
{
	struct blkdev *b = kzalloc(sizeof *b);
	void *mem = (void *)image;
	if (!b)
		return NULL;
	strncpy0(b->name, name, sizeof b->name);
	strcpy(b->model, "RAM disk");
	b->sectors = bytes / 512;
	b->read = ram_read;
	b->write = ram_write;
	b->priv = mem;
	return b;
}
