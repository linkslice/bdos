/*
 * install.c - putting bdos on a disk.
 *
 * A new install wipes the disk and gives it a GPT: a 64 MB EFI system
 * partition holding the loader and the kernel (from C:\BDOS\BOOT), and
 * bdosfs over the rest, which gets a copy of C: and the settings chosen in
 * the installer. The firmware finds \EFI\BOOT\BOOTX64.EFI by itself, so no
 * boot entry needs writing.
 *
 * An upgrade keeps the partitions, your files and settings: the EFI
 * partition is written afresh, and bdos's own folders are copied over the
 * old ones.
 *
 * The INSTALL command and the Install bdos window both drive install_run().
 */
#include "kernel.h"
#include "vfs.h"
#include "disk.h"
#include "install.h"
#include "settings.h"
#include "net/net.h"
#include "../include/bootinfo.h"

#define BOOT_DIR "C:\\BDOS\\BOOT"

int install_is_live(void)
{
	struct vstat st;
	struct blkdev *c = vfs_drive_disk('C');
	return c && !strcmp(c->model, "RAM disk") && !vfs_stat("C:\\BDOS\\LIVE.TXT", &st);
}

void install_size(uint64_t sectors, char *out, size_t n)
{
	uint64_t mb = sectors / 2048;
	if (mb >= 10240)
		snprintf(out, n, "%llu GB", (unsigned long long)(mb / 1024));
	else if (mb >= 1024)
		snprintf(out, n, "%llu.%llu GB", (unsigned long long)(mb / 1024), (unsigned long long)(mb % 1024 * 10 / 1024));
	else
		snprintf(out, n, "%llu MB", (unsigned long long)mb);
}

int install_check_disk(int disk, char *err, size_t n)
{
	if (disk < 0 || disk >= blk_count()) {
		strncpy0(err, "There's no such disk.", n);
		return E_NOENT;
	}
	struct blkdev *b = blk_get(disk);
	if (!strcmp(b->model, "RAM disk")) {
		strncpy0(err, "That's the RAM disk bdos is running from.", n);
		return E_ACCES;
	}
	for (char l = 'A'; l <= 'Z'; l++)
		if (vfs_drive_disk(l) == b && l == 'C') {
			strncpy0(err, "bdos is running from that disk (it's C:).", n);
			return E_ACCES;
		}
	if (b->sectors < (uint64_t)(INSTALL_ESP_MB + 32) * 2048) {
		strncpy0(err, "That disk is too small: bdos needs at least 96 MB.", n);
		return E_NOSPC;
	}
	return 0;
}

void install_defaults(struct install_opts *o)
{
	memset(o, 0, sizeof *o);
	o->disk = -1;
	strncpy0(o->label, "BDOS", sizeof o->label);
	strncpy0(o->hostname, net_hostname(), sizeof o->hostname);
	strncpy0(o->ip, setting("net.ip"), sizeof o->ip);
	strncpy0(o->mask, setting("net.mask"), sizeof o->mask);
	strncpy0(o->gateway, setting("net.gateway"), sizeof o->gateway);
	strncpy0(o->dns, setting("net.dns"), sizeof o->dns);
	strncpy0(o->password, setting("net.password"), sizeof o->password);
	o->bterm = setting_int("net.udp") != 0;
	o->telnet = setting_int("net.telnet") != 0;
	strncpy0(o->ntp, setting("net.ntp"), sizeof o->ntp);
	strncpy0(o->tz, setting("clock.tz"), sizeof o->tz);
	strncpy0(o->dst, setting("clock.dst"), sizeof o->dst);
	o->desktop = 1;
	strncpy0(o->theme, setting("desktop.theme"), sizeof o->theme);
}

/* ---- copying C: across ------------------------------------------------------------------ */

struct copy {
	uint64_t total, done;
	int last_pct;
	void (*progress)(void *arg, int pct, const char *what);
	void *arg;
	int pct_from, pct_to;
	int err;
	const char *skip[8];		/* paths under the source not to copy */
	char *buf;
};

struct ents {
	struct vstat *v;
	int n, cap;
};

static int collect(void *arg, struct vstat *st)
{
	struct ents *e = arg;
	if (!strcmp(st->name, ".") || !strcmp(st->name, ".."))
		return 0;
	if (e->n == e->cap) {
		int nc = e->cap ? e->cap * 2 : 32;
		struct vstat *nv = krealloc(e->v, sizeof *nv * (size_t)nc);
		if (!nv)
			return 1;
		e->v = nv;
		e->cap = nc;
	}
	e->v[e->n++] = *st;
	return 0;
}

static uint64_t tree_bytes(const char *dir)
{
	struct ents e = { 0 };
	vfs_list(dir, collect, &e);
	uint64_t t = 0;
	for (int i = 0; i < e.n; i++) {
		char p[VFS_PATH_MAX];
		if (vfs_join(dir, e.v[i].name, p, sizeof p))
			continue;
		t += e.v[i].type == VFS_DIR ? tree_bytes(p) : e.v[i].size;
	}
	kfree(e.v);
	return t;
}

static int skipped(struct copy *c, const char *path)
{
	for (int i = 0; i < (int)NELEM(c->skip) && c->skip[i]; i++)
		if (!strcasecmp(c->skip[i], path))
			return 1;
	return 0;
}

static void tick(struct copy *c, const char *what)
{
	int pct = c->pct_from + (int)(c->total ? c->done * (uint64_t)(c->pct_to - c->pct_from) / c->total : 0);
	if (pct != c->last_pct && c->progress) {
		c->last_pct = pct;
		c->progress(c->arg, pct, what);
	}
}

static int copy_file(struct copy *c, const char *src, const char *dst, uint64_t mtime, uint8_t attr)
{
	int err = 0;
	struct vfile *in = vfs_open(src, O_READ, &err);
	if (!in)
		return err;
	struct vfile *out = vfs_open(dst, O_WRITE | O_CREATE | O_TRUNC, &err);
	if (!out) {
		vfs_close(in);
		return err;
	}
	long r;
	while ((r = vfs_read(in, c->buf, 65536)) > 0) {
		long w = vfs_write(out, c->buf, (size_t)r);
		if (w != r) {
			err = w < 0 ? (int)w : E_NOSPC;
			break;
		}
		c->done += (uint64_t)r;
		char what[64];
		snprintf(what, sizeof what, "Copying %s", vfs_basename(src));
		tick(c, what);
	}
	if (r < 0)
		err = (int)r;
	vfs_close(out);
	vfs_close(in);
	(void)mtime;
	if (!err && attr)
		vfs_setattr(dst, attr);
	/* between files, with nothing half-done: let everything else have a turn
	 * (the desktop redraws, the network answers) */
	task_yield();
	return err;
}

static int copy_tree(struct copy *c, const char *src, const char *dst)
{
	struct ents e = { 0 };
	int r = vfs_list(src, collect, &e);
	if (r)
		return r;
	for (int i = 0; i < e.n && !r; i++) {
		char s[VFS_PATH_MAX], d[VFS_PATH_MAX];
		if (vfs_join(src, e.v[i].name, s, sizeof s) || vfs_join(dst, e.v[i].name, d, sizeof d)) {
			r = E_NAMETOOLONG;
			break;
		}
		if (skipped(c, s))
			continue;
		if (e.v[i].type == VFS_DIR) {
			struct vstat st;
			if (vfs_stat(d, &st))
				r = vfs_mkdir(d);
			if (!r)
				r = copy_tree(c, s, d);
		} else if (e.v[i].type == VFS_FILE) {
			r = copy_file(c, s, d, e.v[i].mtime, e.v[i].attr);
		}
	}
	kfree(e.v);
	return r;
}

/* ---- the EFI system partition ---------------------------------------------------------- */

static int esp_put(struct fatw *w, const char *fat_path, const char *src, char *err, size_t n)
{
	size_t len;
	int e = 0;
	char *data = vfs_slurp(src, &len, &e);
	if (!data) {
		snprintf(err, n, "Can't read %s: %s", src, vfs_strerror(e));
		return e ? e : E_IO;
	}
	e = fat_add(w, fat_path, data, (uint32_t)len);
	kfree(data);
	if (e)
		snprintf(err, n, "Can't write %s to the EFI partition: %s", fat_path, vfs_strerror(e));
	return e;
}

static int write_esp(struct blkdev *b, const struct part *esp, char *err, size_t n)
{
	struct fatw *w;
	int e = fat_format(b, esp->start, esp->count, "BDOS-EFI", &w);
	if (e) {
		snprintf(err, n, "Can't format the EFI partition: %s", vfs_strerror(e));
		return e;
	}
	if ((e = fat_mkdir(w, "EFI")) || (e = fat_mkdir(w, "EFI/BOOT")) || (e = fat_mkdir(w, "BDOS"))) {
		snprintf(err, n, "Can't make folders on the EFI partition: %s", vfs_strerror(e));
		fat_finish(w);
		return e;
	}
	if ((e = esp_put(w, "EFI/BOOT/BOOTX64.EFI", BOOT_DIR "\\BOOTX64.EFI", err, n)) ||
	    (e = esp_put(w, "BDOS/KERNEL.SYS", BOOT_DIR "\\KERNEL.SYS", err, n))) {
		fat_finish(w);
		return e;
	}
	e = fat_finish(w);
	if (e)
		snprintf(err, n, "Can't finish the EFI partition: %s", vfs_strerror(e));
	return e;
}

/* ---- the new system's own settings ------------------------------------------------------ */

static int write_settings(const struct install_opts *o, char letter)
{
	const char *keys[16], *vals[16];
	int k = 0;
	char btbuf[4], tnbuf[4];
#define SET(key, val) do { keys[k] = key; vals[k] = val; k++; } while (0)
	if (o->ip[0])
		SET("net.ip", o->ip);
	if (o->ip[0] && strcasecmp(o->ip, "dhcp")) {
		SET("net.mask", o->mask);
		SET("net.gateway", o->gateway);
	}
	SET("net.dns", o->dns);
	SET("net.password", o->password);
	snprintf(btbuf, sizeof btbuf, "%d", o->bterm);
	SET("net.udp", btbuf);
	snprintf(tnbuf, sizeof tnbuf, "%d", o->telnet);
	SET("net.telnet", tnbuf);
	SET("net.ntp", o->ntp);
	if (o->tz[0])
		SET("clock.tz", o->tz);
	if (o->dst[0])
		SET("clock.dst", o->dst);
	if (o->theme[0])
		SET("desktop.theme", o->theme);
#undef SET
	char path[32];
	snprintf(path, sizeof path, "%c:\\BDOS\\SETTINGS.INI", letter);
	int r = settings_write_to(path, keys, vals, k);
	if (r)
		return E_IO;
	if (o->hostname[0]) {
		char hp[32], name[16];
		snprintf(hp, sizeof hp, "%c:\\BDOS\\HOSTNAME.TXT", letter);
		size_t i = 0;
		for (; o->hostname[i] && i < sizeof name - 1; i++)
			name[i] = (char)toupper((unsigned char)o->hostname[i]);
		name[i] = 0;
		vfs_spew(hp, name, strlen(name));
	}
	/* AUTOEXEC.BAT: WIN at the end, or not */
	char ap[32];
	snprintf(ap, sizeof ap, "%c:\\AUTOEXEC.BAT", letter);
	size_t len;
	int e = 0;
	char *src = vfs_slurp(ap, &len, &e);
	char *out = kmalloc(len + 64);
	if (src && out) {
		size_t o2 = 0;
		for (char *p = src; p < src + len; ) {
			char *nl = memchr(p, '\n', (size_t)(src + len - p));
			size_t l = nl ? (size_t)(nl - p + 1) : (size_t)(src + len - p);
			const char *q = p;
			while (*q == ' ' || *q == '@')
				q++;
			int is_win = !strncasecmp(q, "WIN", 3) && (q[3] == '\r' || q[3] == '\n' || q[3] == ' ' || q + 3 == src + len);
			if (!is_win) {
				memcpy(out + o2, p, l);
				o2 += l;
			}
			p += l;
		}
		if (o->desktop) {
			if (o2 && out[o2 - 1] != '\n')
				out[o2++] = '\n';
			memcpy(out + o2, "WIN\r\n", 5);
			o2 += 5;
		}
		vfs_spew(ap, out, o2);
	}
	kfree(src);
	kfree(out);
	return 0;
}

/* ---- the lot ------------------------------------------------------------------------------ */

/* let go of any drive on this disk (an installed bdos the live CD found, say) */
void install_release_disk(struct blkdev *b)
{
	for (char l = 'D'; l <= 'Z'; l++)
		if (vfs_drive_disk(l) == b)
			vfs_eject(l);
}

static char free_letter(void)
{
	for (char l = 'Y'; l > 'D'; l--)
		if (!vfs_drive(l) && l != 'T')
			return l;
	return 0;
}

int install_run(const struct install_opts *o, void (*progress)(void *arg, int pct, const char *what),
    void *arg, char *err, size_t n)
{
	int e = install_check_disk(o->disk, err, n);
	if (e)
		return e;
	struct blkdev *b = blk_get(o->disk);
	install_release_disk(b);
	struct stat_check { const char *path; } need[] = { { BOOT_DIR "\\BOOTX64.EFI" }, { BOOT_DIR "\\KERNEL.SYS" } };
	for (size_t i = 0; i < NELEM(need); i++) {
		struct vstat st;
		if (vfs_stat(need[i].path, &st)) {
			snprintf(err, n, "%s is missing: this copy of bdos can't install itself.", need[i].path);
			return E_NOENT;
		}
	}
	struct part p[MAX_PARTS], made[2];
	if (o->upgrade) {
		int k = disk_partitions(b, p, MAX_PARTS), have_esp = 0, have_bdos = 0;
		for (int i = 0; i < k; i++) {
			if (p[i].type == PART_ESP && !have_esp) {
				made[0] = p[i];
				have_esp = 1;
			} else if (p[i].type == PART_BDOS && !have_bdos) {
				made[1] = p[i];
				have_bdos = 1;
			}
		}
		if (!have_esp || !have_bdos) {
			strncpy0(err, "That disk doesn't have bdos on it to upgrade.", n);
			return E_NOENT;
		}
	} else {
		if (progress)
			progress(arg, 1, "Partitioning");
		e = disk_make_gpt(b, INSTALL_ESP_MB, made);
		if (e) {
			snprintf(err, n, "Can't partition the disk: %s", vfs_strerror(e));
			return e;
		}
	}
	if (progress)
		progress(arg, 4, "Writing the EFI partition");
	e = write_esp(b, &made[0], err, n);
	if (e)
		return e;
	if (!o->upgrade) {
		if (progress)
			progress(arg, 8, "Formatting");
		e = vfs_format_bdos(b, made[1].start, made[1].count, o->label[0] ? o->label : "BDOS");
		if (e) {
			snprintf(err, n, "Can't format the bdos partition: %s", vfs_strerror(e));
			return e;
		}
	}
	char letter = free_letter();
	if (!letter) {
		strncpy0(err, "No drive letter is free to put the new disk on.", n);
		return E_NOSPC;
	}
	char what[40];
	snprintf(what, sizeof what, "%s partition %d", b->name, made[1].index);
	e = vfs_mount_bdos(letter, b, made[1].start, made[1].count, what);
	if (e) {
		snprintf(err, n, "Can't open the new bdos partition: %s", vfs_strerror(e));
		return e;
	}
	struct copy c;
	memset(&c, 0, sizeof c);
	c.buf = kmalloc(65536);
	c.progress = progress;
	c.arg = arg;
	c.pct_from = 10;
	c.pct_to = 95;
	c.last_pct = -1;
	/* never the live CD's own markers; an upgrade keeps your settings and groups */
	int ns = 0;
	c.skip[ns++] = "C:\\BDOS\\LIVE.TXT";
	c.skip[ns++] = "C:\\PROGMAN\\Main\\Install bdos.lnk";
	if (o->upgrade) {
		c.skip[ns++] = "C:\\BDOS\\SETTINGS.INI";
		c.skip[ns++] = "C:\\BDOS\\HOSTNAME.TXT";
		c.skip[ns++] = "C:\\AUTOEXEC.BAT";
		c.skip[ns++] = "C:\\PROGMAN";
		c.skip[ns++] = "C:\\BDOS\\STATES";
	}
	char root[4] = { letter, ':', '\\', 0 };
	if (!c.buf) {
		e = E_NOMEM;
	} else {
		c.total = tree_bytes("C:\\");
		e = copy_tree(&c, "C:\\", root);
	}
	if (e) {
		snprintf(err, n, "Copying files failed: %s", vfs_strerror(e));
	} else if (!o->upgrade) {
		if (progress)
			progress(arg, 96, "Saving settings");
		if (write_settings(o, letter)) {
			strncpy0(err, "Can't write the settings.", n);
			e = E_IO;
		}
	}
	kfree(c.buf);
	if (progress)
		progress(arg, 98, "Finishing");
	vfs_eject(letter);
	if (!e && progress)
		progress(arg, 100, "Done");
	return e;
}
