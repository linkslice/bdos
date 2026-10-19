/* pci.c - PCI configuration space through the legacy 0xCF8/0xCFC ports */
#include "../kernel.h"
#include "drv.h"

static struct pci_dev devs[64];
static int ndevs;

uint32_t pci_read32(struct pci_dev *d, uint8_t off)
{
	outl(0xCF8, 0x80000000u | (uint32_t)d->bus << 16 | (uint32_t)d->slot << 11 |
	    (uint32_t)d->func << 8 | (off & 0xFC));
	return inl(0xCFC);
}

void pci_write32(struct pci_dev *d, uint8_t off, uint32_t v)
{
	outl(0xCF8, 0x80000000u | (uint32_t)d->bus << 16 | (uint32_t)d->slot << 11 |
	    (uint32_t)d->func << 8 | (off & 0xFC));
	outl(0xCFC, v);
}

static uint32_t raw_read(int bus, int slot, int func, uint8_t off)
{
	struct pci_dev d = { .bus = (uint8_t)bus, .slot = (uint8_t)slot, .func = (uint8_t)func };
	return pci_read32(&d, off);
}

void pci_init(void)
{
	for (int bus = 0; bus < 256; bus++) {
		for (int slot = 0; slot < 32; slot++) {
			uint32_t id = raw_read(bus, slot, 0, 0);
			if ((id & 0xFFFF) == 0xFFFF)
				continue;
			int funcs = (raw_read(bus, slot, 0, 0x0C) & 0x00800000) ? 8 : 1;
			for (int f = 0; f < funcs && ndevs < (int)NELEM(devs); f++) {
				id = raw_read(bus, slot, f, 0);
				if ((id & 0xFFFF) == 0xFFFF)
					continue;
				struct pci_dev *d = &devs[ndevs++];
				d->bus = (uint8_t)bus;
				d->slot = (uint8_t)slot;
				d->func = (uint8_t)f;
				d->vendor = id & 0xFFFF;
				d->device = id >> 16;
				uint32_t cls = pci_read32(d, 0x08);
				d->class_code = cls >> 24;
				d->subclass = (cls >> 16) & 0xFF;
				d->prog_if = (cls >> 8) & 0xFF;
				for (int b = 0; b < 6; b++)
					d->bar[b] = pci_read32(d, (uint8_t)(0x10 + b * 4));
				d->irq = pci_read32(d, 0x3C) & 0xFF;
			}
		}
	}
}

struct pci_dev *pci_find_class(int cls, int sub, int prog_if, int index)
{
	for (int i = 0; i < ndevs; i++) {
		struct pci_dev *d = &devs[i];
		if (d->class_code == cls && d->subclass == sub &&
		    (prog_if < 0 || d->prog_if == prog_if) && index-- == 0)
			return d;
	}
	return NULL;
}

struct pci_dev *pci_find_id(uint16_t vendor, uint16_t device, int index)
{
	for (int i = 0; i < ndevs; i++)
		if (devs[i].vendor == vendor && devs[i].device == device && index-- == 0)
			return &devs[i];
	return NULL;
}

void pci_enable(struct pci_dev *d)
{
	uint32_t cmd = pci_read32(d, 0x04);
	cmd |= 0x7;		/* I/O space, memory space, bus master */
	cmd &= ~(1u << 10);	/* legacy INTx allowed */
	pci_write32(d, 0x04, cmd);
}

uint64_t pci_bar_mem(struct pci_dev *d, int i)
{
	uint32_t lo = d->bar[i];
	if (lo & 1)
		return 0;	/* I/O BAR */
	uint64_t addr = lo & ~0xFULL;
	if (((lo >> 1) & 3) == 2 && i < 5)	/* 64-bit BAR */
		addr |= (uint64_t)d->bar[i + 1] << 32;
	return addr;
}

uint16_t pci_bar_io(struct pci_dev *d, int i)
{
	return (d->bar[i] & 1) ? (uint16_t)(d->bar[i] & ~3u) : 0;
}

void pci_list(void)
{
	kprintf("\n  Bus:Dev.F  Vendor Device  Class\n");
	for (int i = 0; i < ndevs; i++) {
		struct pci_dev *d = &devs[i];
		const char *what = "";
		if (d->class_code == 1 && d->subclass == 6) what = "SATA (AHCI) controller";
		else if (d->class_code == 1) what = "Storage controller";
		else if (d->class_code == 2) what = "Network controller";
		else if (d->class_code == 3) what = "Display controller";
		else if (d->class_code == 6) what = "Bridge";
		else if (d->class_code == 0xC && d->subclass == 3) what = "USB controller";
		else if (d->class_code == 0xC && d->subclass == 5) what = "SMBus";
		kprintf("  %02x:%02x.%x    %04x   %04x    %02x%02x  %s\n", d->bus, d->slot,
		    d->func, d->vendor, d->device, d->class_code, d->subclass, what);
	}
	kprintf("\n");
}
