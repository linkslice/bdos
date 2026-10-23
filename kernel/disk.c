/*
 * disk.c - what's on a disk, and making it ready for bdos.
 *
 * Reading: partitions come from a GPT when the disk has one (behind its
 * protective MBR), otherwise from the MBR. bdosfs is MBR type 0xBD, or the
 * GPT type below; an EFI system partition is 0xEF or the standard GUID.
 *
 * Writing, for the installer: a fresh GPT with an EFI system partition and
 * bdosfs after it, and a FAT32 writer that formats the ESP and puts the
 * loader, kernel and fallback root image on it. Only what a new, empty
 * volume needs: directories of one cluster, files in one contiguous run.
 */
#include "kernel.h"
#include "disk.h"
#include "mbrboot.h"
#include "vfs.h"

/* GUIDs as they sit on disk (the first three fields little-endian) */
static const uint8_t guid_esp[16] = {
	0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B,
};
/* bdosfs: BD05F500-B005-4BD0-8D05-626466736673 ("bdfsfs") */
static const uint8_t guid_bdos[16] = {
	0x00, 0xF5, 0x05, 0xBD, 0x05, 0xB0, 0xD0, 0x4B, 0x8D, 0x05, 0x62, 0x64, 0x66, 0x73, 0x66, 0x73,
};

uint32_t crc32(uint32_t crc, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
	}
	return ~crc;
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wr16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

/* ---- reading ---------------------------------------------------------------------------- */

static int read_gpt(struct blkdev *b, struct part *parts, int max)
{
	uint8_t *hdr = kmalloc(512);
	if (!hdr)
		return -1;
	int n = -1;
	if (b->read(b, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8))
		goto out;
	uint64_t elba = rd64(hdr + 72);
	uint32_t count = rd32(hdr + 80), esize = rd32(hdr + 84);
	if (esize < 128 || esize > 512 || count > 256)
		goto out;
	uint32_t bytes = count * esize, secs = (bytes + 511) / 512;
	uint8_t *ents = kmalloc(secs * 512);
	if (!ents)
		goto out;
	n = 0;
	if (!b->read(b, elba, secs, ents)) {
		static const uint8_t zero[16];
		for (uint32_t i = 0; i < count && n < max; i++) {
			uint8_t *e = ents + i * esize;
			if (!memcmp(e, zero, 16))
				continue;
			struct part *p = &parts[n++];
			p->type = !memcmp(e, guid_bdos, 16) ? PART_BDOS : !memcmp(e, guid_esp, 16) ? PART_ESP : PART_OTHER;
			p->start = rd64(e + 32);
			p->count = rd64(e + 40) - p->start + 1;
			p->index = (int)i + 1;
			p->gpt = 1;
		}
	}
	kfree(ents);
out:
	kfree(hdr);
	return n;
}

int disk_partitions(struct blkdev *b, struct part *parts, int max)
{
	uint8_t *mbr = kmalloc(512);
	if (!mbr)
		return -1;
	int n = 0;
	if (b->read(b, 0, 1, mbr)) {
		kfree(mbr);
		return -1;
	}
	if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
		int protective = 0;
		for (int i = 0; i < 4; i++)
			if (mbr[446 + i * 16 + 4] == 0xEE)
				protective = 1;
		if (protective) {
			n = read_gpt(b, parts, max);
		} else {
			for (int i = 0; i < 4 && n < max; i++) {
				uint8_t *e = mbr + 446 + i * 16;
				uint32_t start = rd32(e + 8), count = rd32(e + 12);
				if (!e[4] || !count)
					continue;
				struct part *p = &parts[n++];
				p->type = e[4] == 0xBD ? PART_BDOS : e[4] == 0xEF ? PART_ESP : PART_OTHER;
				p->start = start;
				p->count = count;
				p->index = i + 1;
				p->gpt = 0;
			}
		}
	}
	kfree(mbr);
	return n < 0 ? 0 : n;
}

void disk_describe(struct blkdev *b, char *out, size_t n)
{
	struct part p[MAX_PARTS];
	int k = disk_partitions(b, p, MAX_PARTS);
	if (k < 0) {
		strncpy0(out, "can't be read", n);
		return;
	}
	if (k == 0) {
		/* nothing at all in the first sectors: a blank disk */
		uint8_t *s = kmalloc(512 * 8);
		int blank = s && !b->read(b, 0, 8, s);
		for (int i = 0; blank && i < 512 * 8; i++)
			if (s[i])
				blank = 0;
		kfree(s);
		strncpy0(out, blank ? "empty" : "no partitions", n);
		return;
	}
	int bdos = 0;
	for (int i = 0; i < k; i++)
		bdos |= p[i].type == PART_BDOS;
	if (bdos)
		snprintf(out, n, "bdos (%d partition%s)", k, k == 1 ? "" : "s");
	else
		snprintf(out, n, "%d partition%s, not bdos", k, k == 1 ? "" : "s");
}

/* ---- a new GPT ---------------------------------------------------------------------------- */

static void new_guid(uint8_t g[16])
{
	static uint64_t seq;
	uint64_t a = uptime_us() * 6364136223846793005ULL + ++seq * 1442695040888963407ULL;
	uint64_t c = (a ^ (a >> 29)) * 0xBF58476D1CE4E5B9ULL + seq;
	memcpy(g, &a, 8);
	memcpy(g + 8, &c, 8);
	g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);		/* version 4 */
	g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);		/* variant */
}

static void gpt_entry(uint8_t *e, const uint8_t *type, uint64_t first, uint64_t last, const char *name)
{
	memset(e, 0, 128);
	memcpy(e, type, 16);
	new_guid(e + 16);
	wr64(e + 32, first);
	wr64(e + 40, last);
	for (int i = 0; name[i] && i < 36; i++)
		wr16(e + 56 + i * 2, (uint16_t)name[i]);
}

static void gpt_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t first, uint64_t last,
    const uint8_t *disk_guid, uint64_t entries, uint32_t ecrc)
{
	memset(h, 0, 512);
	memcpy(h, "EFI PART", 8);
	wr32(h + 8, 0x00010000);
	wr32(h + 12, 92);
	wr64(h + 24, self);
	wr64(h + 32, other);
	wr64(h + 40, first);
	wr64(h + 48, last);
	memcpy(h + 56, disk_guid, 16);
	wr64(h + 72, entries);
	wr32(h + 80, 128);
	wr32(h + 84, 128);
	wr32(h + 88, ecrc);
	wr32(h + 16, crc32(0, h, 92));
}

int disk_make_gpt(struct blkdev *b, uint32_t esp_mb, struct part out[2])
{
	uint64_t total = b->sectors;
	uint64_t esp_start = 2048, esp_count = (uint64_t)esp_mb * 2048;
	uint64_t first_usable = 34, last_usable = total - 34;
	uint64_t bfs_start = esp_start + esp_count;
	if (total < bfs_start + 16 * 2048)
		return E_NOSPC;			/* under 16 MB for bdosfs: no */
	uint64_t bfs_count = (last_usable + 1 - bfs_start) & ~7ULL;

	uint8_t *buf = kzalloc(512 * 33);
	if (!buf)
		return E_NOMEM;
	/* the entries (32 sectors) */
	uint8_t *ents = buf + 512;
	gpt_entry(ents, guid_esp, esp_start, esp_start + esp_count - 1, "EFI system partition");
	gpt_entry(ents + 128, guid_bdos, bfs_start, bfs_start + bfs_count - 1, "bdos");
	uint32_t ecrc = crc32(0, ents, 128 * 128);
	uint8_t disk_guid[16];
	new_guid(disk_guid);

	/* the protective MBR, with boot code that tells a BIOS to use UEFI */
	uint8_t *m = buf;
	memset(m, 0, 512);
	memcpy(m, mbrboot, sizeof mbrboot);
	m[446 + 1] = 0x00; m[446 + 2] = 0x02; m[446 + 3] = 0x00;	/* CHS 0/0/2 */
	m[446 + 4] = 0xEE;
	m[446 + 5] = 0xFF; m[446 + 6] = 0xFF; m[446 + 7] = 0xFF;
	wr32(m + 446 + 8, 1);
	wr32(m + 446 + 12, (uint32_t)MIN(total - 1, 0xFFFFFFFFULL));
	m[510] = 0x55;
	m[511] = 0xAA;
	int r = b->write(b, 0, 1, m);
	/* primary header and entries, then the backup at the end of the disk */
	uint8_t *h = kmalloc(512);
	if (!h) {
		kfree(buf);
		return E_NOMEM;
	}
	gpt_header(h, 1, total - 1, first_usable, last_usable, disk_guid, 2, ecrc);
	if (!r)
		r = b->write(b, 1, 1, h);
	if (!r)
		r = b->write(b, 2, 32, ents);
	gpt_header(h, total - 1, 1, first_usable, last_usable, disk_guid, total - 33, ecrc);
	if (!r)
		r = b->write(b, total - 33, 32, ents);
	if (!r)
		r = b->write(b, total - 1, 1, h);
	/* nothing left over where the new filesystems start */
	memset(buf, 0, 512 * 33);
	if (!r)
		r = b->write(b, esp_start, 33, buf);
	if (!r)
		r = b->write(b, bfs_start, 33, buf);
	if (!r && b->flush)
		r = b->flush(b);
	kfree(h);
	kfree(buf);
	if (r)
		return E_IO;
	out[0] = (struct part){ PART_ESP, esp_start, esp_count, 1, 1 };
	out[1] = (struct part){ PART_BDOS, bfs_start, bfs_count, 2, 1 };
	return 0;
}

/* ---- FAT32 -------------------------------------------------------------------------------- */

#define FAT_RESERVED	32
#define DIR_ENTRIES	16		/* one 512-byte cluster per directory */

struct fatdir {
	char path[64];
	uint32_t cluster;
	int used;			/* entries */
};

struct fatw {
	struct blkdev *b;
	uint64_t start;
	uint32_t fatsz, clusters, next;
	uint32_t *fat;
	struct fatdir dirs[16];
	int ndirs;
	uint16_t date, time;
};

static uint64_t cluster_lba(struct fatw *w, uint32_t c)
{
	return w->start + FAT_RESERVED + 2ULL * w->fatsz + (c - 2);
}

static void dos_now(uint16_t *date, uint16_t *time)
{
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	*date = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d);
	*time = (uint16_t)((h << 11) | (mi << 5) | (s / 2));
}

int fat_format(struct blkdev *b, uint64_t start, uint64_t count, const char *label, struct fatw **out)
{
	/* one sector a cluster keeps a small ESP over the 65525 clusters FAT32 needs */
	uint32_t tmp1 = (uint32_t)(count - FAT_RESERVED), tmp2 = (256 * 1 + 2) / 2;
	uint32_t fatsz = (tmp1 + tmp2 - 1) / tmp2;
	uint32_t clusters = (uint32_t)(count - FAT_RESERVED - 2ULL * fatsz);
	if (clusters < 65525)
		return E_NOSPC;			/* too small to be FAT32 */
	struct fatw *w = kzalloc(sizeof *w);
	uint32_t *fat = w ? kzalloc((size_t)fatsz * 512) : NULL;
	uint8_t *s = kzalloc(512 * 8);
	if (!w || !fat || !s) {
		kfree(w);
		kfree(fat);
		kfree(s);
		return E_NOMEM;
	}
	w->b = b;
	w->start = start;
	w->fatsz = fatsz;
	w->clusters = clusters;
	w->fat = fat;
	dos_now(&w->date, &w->time);

	/* the boot sector */
	uint8_t *bs = s;
	bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
	memcpy(bs + 3, "BDOS    ", 8);
	wr16(bs + 11, 512);
	bs[13] = 1;				/* sectors per cluster */
	wr16(bs + 14, FAT_RESERVED);
	bs[16] = 2;				/* FATs */
	bs[21] = 0xF8;
	wr16(bs + 24, 32);
	wr16(bs + 26, 64);
	wr32(bs + 28, (uint32_t)start);
	wr32(bs + 32, (uint32_t)count);
	wr32(bs + 36, fatsz);
	wr32(bs + 44, 2);			/* root directory cluster */
	wr16(bs + 48, 1);			/* FSInfo */
	wr16(bs + 50, 6);			/* backup boot sector */
	bs[64] = 0x80;
	bs[66] = 0x29;
	wr32(bs + 67, (uint32_t)uptime_us());
	char lab[11];
	memset(lab, ' ', 11);
	for (int i = 0; label[i] && i < 11; i++)
		lab[i] = (char)toupper((unsigned char)label[i]);
	memcpy(bs + 71, lab, 11);
	memcpy(bs + 82, "FAT32   ", 8);
	bs[510] = 0x55;
	bs[511] = 0xAA;
	/* FSInfo */
	uint8_t *fi = s + 512;
	wr32(fi, 0x41615252);
	wr32(fi + 484, 0x61417272);
	wr32(fi + 488, 0xFFFFFFFF);		/* free count: unknown */
	wr32(fi + 492, 0xFFFFFFFF);
	wr32(fi + 508, 0xAA550000);
	int r = b->write(b, start, 2, s);
	if (!r)
		r = b->write(b, start + 6, 2, s);
	/* the root directory: empty but for the volume label */
	memset(s, 0, 512 * 8);
	memcpy(s, lab, 11);
	s[11] = 0x08;
	if (!r)
		r = b->write(b, cluster_lba(w, 2), 1, s);
	kfree(s);
	if (r) {
		kfree(fat);
		kfree(w);
		return E_IO;
	}
	fat[0] = 0x0FFFFFF8;
	fat[1] = 0x0FFFFFFF;
	fat[2] = 0x0FFFFFFF;
	w->next = 3;
	w->dirs[0] = (struct fatdir){ "", 2, 1 };
	w->ndirs = 1;
	*out = w;
	return 0;
}

/* "EFI/BOOT/BOOTX64.EFI" -> its directory and its 8.3 name */
static int split83(struct fatw *w, const char *path, struct fatdir **dir, char name[11])
{
	const char *slash = strrchr(path, '/');
	char parent[64] = "";
	if (slash) {
		size_t l = (size_t)(slash - path);
		if (l >= sizeof parent)
			return E_NAMETOOLONG;
		memcpy(parent, path, l);
		parent[l] = 0;
	}
	const char *base = slash ? slash + 1 : path;
	*dir = NULL;
	for (int i = 0; i < w->ndirs; i++)
		if (!strcasecmp(w->dirs[i].path, parent))
			*dir = &w->dirs[i];
	if (!*dir)
		return E_NOENT;
	memset(name, ' ', 11);
	const char *dot = strrchr(base, '.');
	size_t bl = dot ? (size_t)(dot - base) : strlen(base);
	if (!bl || bl > 8 || (dot && strlen(dot + 1) > 3))
		return E_INVAL;
	for (size_t i = 0; i < bl; i++)
		name[i] = (char)toupper((unsigned char)base[i]);
	for (size_t i = 0; dot && dot[1 + i]; i++)
		name[8 + i] = (char)toupper((unsigned char)dot[1 + i]);
	return 0;
}

static int add_entry(struct fatw *w, struct fatdir *d, const char name[11], uint8_t attr, uint32_t cluster,
    uint32_t size)
{
	if (d->used >= DIR_ENTRIES)
		return E_NOSPC;
	uint8_t *s = kmalloc(512);
	if (!s)
		return E_NOMEM;
	uint64_t lba = cluster_lba(w, d->cluster);
	int r = w->b->read(w->b, lba, 1, s);
	uint8_t *e = s + d->used * 32;
	memset(e, 0, 32);
	memcpy(e, name, 11);
	e[11] = attr;
	wr16(e + 14, w->time);
	wr16(e + 16, w->date);
	wr16(e + 18, w->date);
	wr16(e + 20, (uint16_t)(cluster >> 16));
	wr16(e + 22, w->time);
	wr16(e + 24, w->date);
	wr16(e + 26, (uint16_t)cluster);
	wr32(e + 28, size);
	if (!r)
		r = w->b->write(w->b, lba, 1, s);
	kfree(s);
	if (r)
		return E_IO;
	d->used++;
	return 0;
}

int fat_mkdir(struct fatw *w, const char *path)
{
	struct fatdir *parent;
	char name[11];
	int r = split83(w, path, &parent, name);
	if (r)
		return r;
	if (w->ndirs >= (int)NELEM(w->dirs) || w->next >= w->clusters + 2)
		return E_NOSPC;
	uint32_t c = w->next++;
	w->fat[c] = 0x0FFFFFFF;
	/* . and .. */
	uint8_t *s = kzalloc(512);
	if (!s)
		return E_NOMEM;
	r = w->b->write(w->b, cluster_lba(w, c), 1, s);
	kfree(s);
	if (r)
		return E_IO;
	struct fatdir *d = &w->dirs[w->ndirs++];
	strncpy0(d->path, path, sizeof d->path);
	d->cluster = c;
	d->used = 0;
	static const char dot[11] = { '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	static const char dotdot[11] = { '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ' };
	r = add_entry(w, d, dot, 0x10, c, 0);
	if (!r)
		r = add_entry(w, d, dotdot, 0x10, parent->cluster == 2 ? 0 : parent->cluster, 0);
	if (!r)
		r = add_entry(w, parent, name, 0x10, c, 0);
	return r;
}

int fat_add(struct fatw *w, const char *path, const void *data, uint32_t len)
{
	struct fatdir *d;
	char name[11];
	int r = split83(w, path, &d, name);
	if (r)
		return r;
	uint32_t n = (len + 511) / 512;
	if (w->next + n > w->clusters + 2)
		return E_NOSPC;
	uint32_t first = n ? w->next : 0;
	for (uint32_t i = 0; i < n; i++)
		w->fat[w->next + i] = i + 1 < n ? w->next + i + 1 : 0x0FFFFFFF;
	/* the data, a chunk at a time, the last sector padded */
	const uint8_t *p = data;
	uint32_t whole = len / 512, done = 0;
	while (done < whole) {
		uint32_t k = MIN(whole - done, 128u);
		if (w->b->write(w->b, cluster_lba(w, first + done), k, p + (size_t)done * 512))
			return E_IO;
		done += k;
	}
	if (len % 512) {
		uint8_t *s = kzalloc(512);
		if (!s)
			return E_NOMEM;
		memcpy(s, p + (size_t)whole * 512, len % 512);
		r = w->b->write(w->b, cluster_lba(w, first + whole), 1, s);
		kfree(s);
		if (r)
			return E_IO;
	}
	w->next += n;
	return add_entry(w, d, name, 0x20, first, len);
}

int fat_finish(struct fatw *w)
{
	int r = 0;
	for (int copy = 0; copy < 2 && !r; copy++) {
		uint64_t lba = w->start + FAT_RESERVED + (uint64_t)copy * w->fatsz;
		for (uint32_t done = 0; done < w->fatsz && !r; done += 128) {
			uint32_t k = MIN(w->fatsz - done, 128u);
			r = w->b->write(w->b, lba + done, k, (uint8_t *)w->fat + (size_t)done * 512);
		}
	}
	/* FSInfo: what's free now */
	uint8_t *s = kmalloc(512);
	if (!r && s && !w->b->read(w->b, w->start + 1, 1, s)) {
		wr32(s + 488, w->clusters + 2 - w->next);
		wr32(s + 492, w->next);
		r = w->b->write(w->b, w->start + 1, 1, s);
	}
	kfree(s);
	if (!r && w->b->flush)
		r = w->b->flush(w->b);
	kfree(w->fat);
	kfree(w);
	return r ? E_IO : 0;
}
