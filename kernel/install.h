/* install.h - putting bdos on a disk: from the live CD, or onto another disk */
#ifndef BDOS_INSTALL_H
#define BDOS_INSTALL_H
#include <stddef.h>
#include "drv/drv.h"

#define INSTALL_ESP_MB	64

struct install_opts {
	int disk;			/* blk_get() index */
	int upgrade;			/* keep the partitions, files and settings; renew bdos */
	char label[12];			/* the new C:'s label */
	/* settings for the new system; empty strings leave the current ones */
	char hostname[16];
	char ip[16], mask[16], gateway[16], dns[64];	/* ip "dhcp" or an address */
	char password[32];
	int bterm;			/* net.udp */
	int telnet;
	char ntp[64];			/* "" for none */
	char tz[8];			/* hours from UTC: "-6", "+5:30" */
	char dst[8];			/* us, eu, none */
	int desktop;			/* start the desktop at boot */
	char theme[32];
};

/* is this the live CD (C: is the RAM disk and C:\BDOS\LIVE.TXT is there)? */
int install_is_live(void);
/* can this disk be installed to? 0, or why not, in err */
int install_check_disk(int disk, char *err, size_t n);
/* fill opts with the current machine's settings */
void install_defaults(struct install_opts *o);
/* do it; progress(arg, percent, what) as it goes. 0, or an E_ code and err */
int install_run(const struct install_opts *o, void (*progress)(void *arg, int pct, const char *what),
    void *arg, char *err, size_t n);
/* unmount whatever drives are on that disk (never C:) */
void install_release_disk(struct blkdev *b);
/* "34.2 GB" */
void install_size(uint64_t sectors, char *out, size_t n);

#endif
