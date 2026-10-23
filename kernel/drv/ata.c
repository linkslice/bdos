/*
 * ata.c - plain IDE (parallel ATA) disks, by PIO: the PIIX controller in
 * QEMU's and Proxmox's default i440fx machine, and old PCs. LBA48, polled,
 * a sector at a time. CD-ROMs (ATAPI) are left to the firmware.
 */
#include "../kernel.h"
#include "drv.h"

#ifdef __x86_64__

struct ata {
	struct blkdev blk;
	uint16_t io, ctl;
	int slave, lba48;
};

static int ata_wait(struct ata *a, int want_drq)
{
	uint64_t until = uptime_ms() + 5000;
	for (;;) {
		uint8_t st = inb(a->io + 7);
		if (st == 0xFF)
			return -1;		/* nothing there */
		if (!(st & 0x80)) {		/* not busy */
			if (st & 0x21)		/* ERR or DF */
				return -1;
			if (!want_drq || (st & 0x08))
				return 0;
		}
		if (uptime_ms() > until)
			return -1;
		cpu_pause();
	}
}

static void ata_select(struct ata *a, uint64_t lba, uint32_t n)
{
	outb(a->io + 6, (uint8_t)(0x40 | (a->slave << 4) | (a->lba48 ? 0 : (lba >> 24) & 0x0F)));
	if (a->lba48) {
		outb(a->io + 2, (uint8_t)(n >> 8));
		outb(a->io + 3, (uint8_t)(lba >> 24));
		outb(a->io + 4, (uint8_t)(lba >> 32));
		outb(a->io + 5, (uint8_t)(lba >> 40));
	}
	outb(a->io + 2, (uint8_t)n);
	outb(a->io + 3, (uint8_t)lba);
	outb(a->io + 4, (uint8_t)(lba >> 8));
	outb(a->io + 5, (uint8_t)(lba >> 16));
}

static int ata_rw(struct blkdev *b, int write, uint64_t lba, uint32_t count, uint8_t *buf)
{
	struct ata *a = b->priv;
	if (lba + count > b->sectors)
		return -1;
	while (count) {
		uint32_t n = MIN(count, a->lba48 ? 256u : 255u);
		if (ata_wait(a, 0))
			return -1;
		ata_select(a, lba, n == 256 && a->lba48 ? 256 : n);
		outb(a->io + 7, write ? (a->lba48 ? 0x34 : 0x30) : (a->lba48 ? 0x24 : 0x20));
		for (uint32_t i = 0; i < n; i++) {
			if (ata_wait(a, 1))
				return -1;
			uint16_t *w = (uint16_t *)(buf + (size_t)i * 512);
			if (write)
				for (int k = 0; k < 256; k++)
					outw(a->io, w[k]);
			else
				for (int k = 0; k < 256; k++)
					w[k] = inw(a->io);
		}
		if (write && ata_wait(a, 0))
			return -1;
		buf += (size_t)n * 512;
		lba += n;
		count -= n;
	}
	return 0;
}

static int ata_read(struct blkdev *b, uint64_t lba, uint32_t count, void *buf) { return ata_rw(b, 0, lba, count, buf); }
static int ata_write(struct blkdev *b, uint64_t lba, uint32_t count, const void *buf) { return ata_rw(b, 1, lba, count, (uint8_t *)buf); }

static int ata_flush(struct blkdev *b)
{
	struct ata *a = b->priv;
	if (ata_wait(a, 0))
		return -1;
	outb(a->io + 6, (uint8_t)(0xA0 | (a->slave << 4)));
	outb(a->io + 7, a->lba48 ? 0xEA : 0xE7);
	return ata_wait(a, 0);
}

static int ndisks;

static void ata_probe(uint16_t io, uint16_t ctl, int slave)
{
	if (inb(io + 7) == 0xFF)
		return;			/* a floating bus: no drives */
	outb(ctl, 0x02);		/* no interrupts: we poll */
	outb(io + 6, (uint8_t)(0xA0 | (slave << 4)));
	for (int i = 0; i < 4; i++)
		(void)inb(ctl);		/* 400 ns */
	outb(io + 2, 0);
	outb(io + 3, 0);
	outb(io + 4, 0);
	outb(io + 5, 0);
	outb(io + 7, 0xEC);		/* IDENTIFY */
	uint8_t st = inb(io + 7);
	if (!st || st == 0xFF)
		return;
	uint64_t until = uptime_ms() + 1000;
	while ((st = inb(io + 7)) & 0x80)
		if (uptime_ms() > until)
			return;
	if (inb(io + 4) || inb(io + 5))
		return;			/* ATAPI or SATA signature: not ours */
	until = uptime_ms() + 1000;
	while (!((st = inb(io + 7)) & 0x09))
		if (uptime_ms() > until)
			return;
	if (st & 1)
		return;
	uint16_t id[256];
	for (int i = 0; i < 256; i++)
		id[i] = inw(io);
	struct ata *a = kzalloc(sizeof *a);
	if (!a)
		return;
	a->io = io;
	a->ctl = ctl;
	a->slave = slave;
	a->lba48 = (id[83] >> 10) & 1;
	uint64_t sectors = a->lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 |
	    (uint64_t)id[103] << 48 : (uint64_t)id[60] | (uint64_t)id[61] << 16;
	if (!sectors) {
		kfree(a);
		return;
	}
	char model[41];
	for (int i = 0; i < 20; i++) {
		model[i * 2] = (char)(id[27 + i] >> 8);
		model[i * 2 + 1] = (char)id[27 + i];
	}
	model[40] = 0;
	for (int i = 39; i >= 0 && model[i] == ' '; i--)
		model[i] = 0;
	snprintf(a->blk.name, sizeof a->blk.name, "ide%d", ndisks++);
	a->blk.sectors = sectors;
	strncpy0(a->blk.model, model, sizeof a->blk.model);
	a->blk.read = ata_read;
	a->blk.write = ata_write;
	a->blk.flush = ata_flush;
	a->blk.priv = a;
	blk_register(&a->blk);
}

void ata_init(void)
{
	for (int i = 0;; i++) {
		struct pci_dev *d = pci_find_class(0x01, 0x01, -1, i);	/* IDE controllers */
		if (!d)
			break;
		pci_enable(d);
		/* each channel: the legacy ports, or its BARs in native mode */
		uint16_t io0 = 0x1F0, ctl0 = 0x3F6, io1 = 0x170, ctl1 = 0x376;
		if ((d->prog_if & 1) && pci_bar_io(d, 0)) {
			io0 = pci_bar_io(d, 0);
			ctl0 = (uint16_t)(pci_bar_io(d, 1) + 2);
		}
		if ((d->prog_if & 4) && pci_bar_io(d, 2)) {
			io1 = pci_bar_io(d, 2);
			ctl1 = (uint16_t)(pci_bar_io(d, 3) + 2);
		}
		for (int s = 0; s < 2; s++)
			ata_probe(io0, ctl0, s);
		for (int s = 0; s < 2; s++)
			ata_probe(io1, ctl1, s);
	}
}

#else
void ata_init(void) {}	/* no PC I/O ports here */
#endif
