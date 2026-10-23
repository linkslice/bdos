/* drv.h - device driver interfaces */
#ifndef BDOS_DRV_H
#define BDOS_DRV_H

#include <stdint.h>

struct pci_dev {
	uint8_t bus, slot, func;
	uint16_t vendor, device;
	uint8_t class_code, subclass, prog_if;
	uint32_t bar[6];
	uint8_t irq;
};

void pci_init(void);
struct pci_dev *pci_find_class(int cls, int sub, int prog_if, int index);
struct pci_dev *pci_find_id(uint16_t vendor, uint16_t device, int index);
uint32_t pci_read32(struct pci_dev *d, uint8_t off);
void pci_write32(struct pci_dev *d, uint8_t off, uint32_t v);
void pci_enable(struct pci_dev *d);
uint64_t pci_bar_mem(struct pci_dev *d, int i);
uint16_t pci_bar_io(struct pci_dev *d, int i);
volatile void *pci_map_bar(struct pci_dev *d, int i, uint64_t size);	/* mapped for us */
void pci_list(void);

/* Block devices, in 512-byte sectors */
struct blkdev {
	char name[16];
	uint64_t sectors;
	int (*read)(struct blkdev *, uint64_t lba, uint32_t count, void *buf);
	int (*write)(struct blkdev *, uint64_t lba, uint32_t count, const void *buf);
	int (*flush)(struct blkdev *);
	void *priv;
	char model[41];
};

void blk_register(struct blkdev *b);
struct blkdev *blk_get(int i);
int blk_count(void);

void ahci_init(void);
void nvme_init(void);
void virtio_blk_init(void);
void virtio_scsi_init(void);
void ata_init(void);
struct blkdev *ramdisk_create(const char *name, const void *image, uint64_t bytes);

#endif
