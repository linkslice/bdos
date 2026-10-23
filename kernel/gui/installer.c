/*
 * installer.c - the Install bdos window: a wizard over install.c.
 *
 *   Welcome -> Disk -> Computer & network -> Time -> Startup -> Summary
 *   -> Installing (a progress bar) -> Done
 *
 * Every page's widgets are made up front and shown or hidden as you go
 * Back and Next. The install itself runs in a task of its own, so the
 * desktop stays alive; a timer brings its progress to the window.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../settings.h"
#include "../disk.h"
#include "../install.h"
#include "../net/net.h"
#include "../net/ntp.h"
#include "gfx.h"
#include "form.h"
#include "themes.h"

enum { P_WELCOME, P_DISK, P_NET, P_TIME, P_START, P_SUMMARY, P_WORK, P_DONE, NPAGES };

#define MAXW 24
#define CW 560
#define CH 420

struct inst {
	int page;
	int w[NPAGES][MAXW], nw[NPAGES];
	int back, next, cancel, heading;
	/* welcome */
	int mode;
	/* disk */
	int disks, bar, diskinfo, label;
	int disk_idx[16], ndisks;
	/* network */
	int name, addr, ip, mask, gw, dns, bterm, pass, telnet;
	/* time */
	int zone, ntp_on, ntp, sync, syncmsg;
	/* startup */
	int desktop, theme;
	char themes[24][48];
	int nthemes;
	const char *zone_names[64], *theme_names[24];	/* form_options keeps the arrays */
	/* summary, progress, done */
	int summary, what, progress, done_msg, reboot;
	struct install_opts o;
	volatile int pct, finished, failed;
	char status[64], err[160];
	int closed;
};

static struct inst *running;

/* the band across the top: the page's name, and where you are */
static void draw_heading(struct form *f, int id, struct surface *s, struct rect r);	/* the one whose install task is going */

static const char *const titles[NPAGES] = {
	"Welcome", "Choose a disk", "Computer and network", "Time", "Starting up", "Ready to install",
	"Installing", "Done",
};

static int add(struct form *f, struct inst *in, int page, int kind, int x, int y, int w, int h, const char *t)
{
	int id = form_add(f, kind, x, y, w, h, t);
	if (in->nw[page] < MAXW)
		in->w[page][in->nw[page]++] = id;
	return id;
}

static int chosen_disk(struct form *f, struct inst *in)
{
	long sel = form_val(f, in->disks);
	return sel >= 0 && sel < in->ndisks ? in->disk_idx[sel] : -1;
}

/* the partitions there now, and what install will make */
static void draw_bar(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct inst *in = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	outline(s, r.x, r.y, r.w, r.h, col(0x808080));
	int d = chosen_disk(f, in);
	if (d < 0) {
		text(s, r.x + 8, r.y + 8, "Pick a disk above", col(0x808080));
		return;
	}
	struct blkdev *b = blk_get(d);
	int upgrade = form_val(f, in->mode) == 1;
	int x = r.x + 2, w = r.w - 4, y = r.y + 2, h = r.h - 4;
	if (upgrade) {
		struct part p[MAX_PARTS];
		int n = disk_partitions(b, p, MAX_PARTS);
		for (int i = 0; i < n; i++) {
			int px = x + (int)((uint64_t)w * p[i].start / b->sectors);
			int pw = MAX(3, (int)((uint64_t)w * p[i].count / b->sectors));
			uint32_t c = p[i].type == PART_ESP ? 0x6080C0 : p[i].type == PART_BDOS ? 0x40A060 : 0xA0A0A0;
			fill(s, px, y, pw, h, col(c));
		}
		text(s, x + 8, y + 6, "Kept as they are: bdos is renewed in place", col(0xFFFFFF));
		return;
	}
	/* the ESP gets a sliver at least, so it shows */
	int ew = MAX(40, (int)((uint64_t)w * INSTALL_ESP_MB * 2048 / b->sectors));
	fill(s, x, y, ew, h, col(0x6080C0));
	text(s, x + 6, y + 6, "EFI", col(0xFFFFFF));
	fill(s, x + ew + 2, y, w - ew - 2, h, col(0x40A060));
	char sz[16], t[48];
	install_size(b->sectors - (uint64_t)INSTALL_ESP_MB * 2048, sz, sizeof sz);
	snprintf(t, sizeof t, "bdos (C:)  %s", sz);
	text(s, x + ew + 10, y + 6, t, col(0xFFFFFF));
}

static void draw_progress(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct inst *in = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	outline(s, r.x, r.y, r.w, r.h, col(0x808080));
	int w = (r.w - 4) * MAX(0, MIN(100, in->pct)) / 100;
	fill(s, r.x + 2, r.y + 2, w, r.h - 4, col(in->failed ? 0xC04040 : ui_colour(1)));
	char t[8];
	snprintf(t, sizeof t, "%d%%", in->pct);
	text(s, r.x + r.w / 2 - 12, r.y + (r.h - 16) / 2, t, col(w > r.w / 2 ? 0xFFFFFF : 0));
}

static void fill_disks(struct form *f, struct inst *in)
{
	form_list_clear(f, in->disks);
	in->ndisks = 0;
	for (int i = 0; i < blk_count() && in->ndisks < 16; i++) {
		struct blkdev *b = blk_get(i);
		if (!strcmp(b->model, "RAM disk"))
			continue;
		char sz[16], what[40], line[96];
		install_size(b->sectors, sz, sizeof sz);
		disk_describe(b, what, sizeof what);
		char model[23];
		strncpy0(model, b->model[0] ? b->model : "", sizeof model);
		snprintf(line, sizeof line, "%-6s %8s  %-22s %s", b->name, sz, model, what);
		form_list_add(f, in->disks, line);
		in->disk_idx[in->ndisks++] = i;
	}
	form_set_text(f, in->disks, "No disks found. bdos can use SATA and NVMe disks.");
	if (in->ndisks == 1)
		form_set_val(f, in->disks, 0);
}

static void disk_info(struct form *f, struct inst *in)
{
	int d = chosen_disk(f, in);
	char msg[160] = "";
	if (d >= 0) {
		char err[96];
		struct blkdev *b = blk_get(d);
		if (install_check_disk(d, err, sizeof err))
			snprintf(msg, sizeof msg, "%s", err);
		else if (form_val(f, in->mode) == 1)
			snprintf(msg, sizeof msg, "bdos on %s is renewed. Your files and settings stay.", b->name);
		else
			snprintf(msg, sizeof msg, "Everything on %s (%s) will be erased.", b->name, b->model);
	}
	form_set_text(f, in->diskinfo, msg);
	form_redraw(f);
}

static int is_upgrade(struct form *f, struct inst *in)
{
	return form_val(f, in->mode) == 1;
}

static void show_page(struct form *f, struct inst *in, int page)
{
	in->page = page;
	for (int p = 0; p < NPAGES; p++)
		for (int i = 0; i < in->nw[p]; i++)
			form_hide(f, in->w[p][i], p != page);
	form_redraw(f);
	form_enable(f, in->back, page > P_WELCOME && page < P_WORK);
	form_hide(f, in->back, page >= P_WORK);
	form_hide(f, in->cancel, page >= P_WORK);
	form_set_text(f, in->next, page == P_SUMMARY ? (is_upgrade(f, in) ? "Upgrade" : "Install") :
	    page == P_DONE ? "Close" : "Next >");
	form_hide(f, in->next, page == P_WORK);
	if (page == P_DISK)
		disk_info(f, in);
	if (page == P_NET) {
		int fixed = form_val(f, in->addr) == 1;
		form_enable(f, in->ip, fixed);
		form_enable(f, in->mask, fixed);
		form_enable(f, in->gw, fixed);
	}
	form_redraw(f);
}

/* what the pages say, into the options */
static void gather(struct form *f, struct inst *in)
{
	struct install_opts *o = &in->o;
	o->disk = chosen_disk(f, in);
	o->upgrade = is_upgrade(f, in);
	strncpy0(o->label, form_text(f, in->label), sizeof o->label);
	strncpy0(o->hostname, form_text(f, in->name), sizeof o->hostname);
	if (form_val(f, in->addr) == 1) {
		strncpy0(o->ip, form_text(f, in->ip), sizeof o->ip);
		strncpy0(o->mask, form_text(f, in->mask), sizeof o->mask);
		strncpy0(o->gateway, form_text(f, in->gw), sizeof o->gateway);
	} else {
		strncpy0(o->ip, "dhcp", sizeof o->ip);
	}
	strncpy0(o->dns, form_text(f, in->dns), sizeof o->dns);
	o->bterm = (int)form_val(f, in->bterm);
	strncpy0(o->password, form_text(f, in->pass), sizeof o->password);
	o->telnet = (int)form_val(f, in->telnet);
	int z = (int)form_val(f, in->zone);
	if (z >= 0 && z < tz_nzones) {
		strncpy0(o->tz, tz_zones[z].tz, sizeof o->tz);
		strncpy0(o->dst, tz_zones[z].dst, sizeof o->dst);
	}
	if (form_val(f, in->ntp_on))
		strncpy0(o->ntp, form_text(f, in->ntp), sizeof o->ntp);
	else
		o->ntp[0] = 0;
	o->desktop = (int)form_val(f, in->desktop);
	int t = (int)form_val(f, in->theme);
	if (t >= 0 && t < in->nthemes)
		strncpy0(o->theme, in->themes[t], sizeof o->theme);
}

/* can we leave this page? (a message if not) */
static int page_ok(struct form *f, struct inst *in)
{
	struct wm *wm = form_wm(f);
	if (in->page == P_DISK) {
		int d = chosen_disk(f, in);
		char err[96];
		if (d < 0) {
			wm_message(wm, "Install bdos", "Choose the disk to put bdos on.");
			return 0;
		}
		if (install_check_disk(d, err, sizeof err)) {
			wm_message(wm, "Install bdos", err);
			return 0;
		}
		if (is_upgrade(f, in)) {
			struct part p[MAX_PARTS];
			int n = disk_partitions(blk_get(d), p, MAX_PARTS), ok = 0;
			for (int i = 0; i < n; i++)
				ok |= p[i].type == PART_BDOS;
			if (!ok) {
				wm_message(wm, "Install bdos", "There's no bdos on that disk to upgrade.");
				return 0;
			}
		}
	}
	if (in->page == P_NET) {
		const char *nm = form_text(f, in->name);
		for (const char *p = nm; *p; p++)
			if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
				wm_message(wm, "Install bdos", "A computer name can have letters, digits, - and _.");
				return 0;
			}
		if (form_val(f, in->addr) == 1) {
			uint32_t a;
			if (ip_parse(form_text(f, in->ip), &a) || ip_parse(form_text(f, in->mask), &a)) {
				wm_message(wm, "Install bdos", "A fixed address needs an IP address and mask like 192.168.1.50 and 255.255.255.0.");
				return 0;
			}
		}
	}
	return 1;
}

static void fill_summary(struct form *f, struct inst *in)
{
	gather(f, in);
	struct install_opts *o = &in->o;
	form_list_clear(f, in->summary);
	char line[128], sz[16];
	struct blkdev *b = blk_get(o->disk);
	install_size(b->sectors, sz, sizeof sz);
	snprintf(line, sizeof line, "%s bdos on %s, %s %s", o->upgrade ? "Upgrade" : "Install", b->name, sz, b->model);
	form_list_add(f, in->summary, line);
	if (o->upgrade) {
		form_list_add(f, in->summary, "Your files, settings and Program Manager groups stay.");
	} else {
		form_list_add(f, in->summary, "The disk is erased: EFI partition 64 MB, bdos (C:) on the rest.");
		snprintf(line, sizeof line, "Computer name: %s", o->hostname);
		form_list_add(f, in->summary, line);
		if (!strcasecmp(o->ip, "dhcp"))
			snprintf(line, sizeof line, "Address: automatic (DHCP)%s%s", o->dns[0] ? ", DNS " : "", o->dns);
		else
			snprintf(line, sizeof line, "Address: %s / %s, gateway %s", o->ip, o->mask, o->gateway[0] ? o->gateway : "none");
		form_list_add(f, in->summary, line);
		snprintf(line, sizeof line, "bterm: %s%s.  Telnet server: %s", o->bterm ? "allowed" : "off",
		    o->bterm && o->password[0] ? " with a password" : "", o->telnet ? "on" : "off");
		form_list_add(f, in->summary, line);
		int z = tz_find(o->tz, o->dst);
		snprintf(line, sizeof line, "Time: %s, %s", tz_zones[z].name, o->ntp[0] ? o->ntp : "no time server");
		form_list_add(f, in->summary, line);
		snprintf(line, sizeof line, "Starts %s, theme %s", o->desktop ? "the desktop" : "at the prompt", o->theme);
		form_list_add(f, in->summary, line);
	}
}

static void progress_cb(void *arg, int pct, const char *what)
{
	struct inst *in = arg;
	in->pct = pct;
	strncpy0(in->status, what, sizeof in->status);
}

static void install_task(void *arg)
{
	struct inst *in = arg;
	int r = install_run(&in->o, progress_cb, in, in->err, sizeof in->err);
	in->failed = r != 0;
	in->finished = 1;
	if (in->closed)		/* the window went while we worked */
		kfree(in);
	running = NULL;
}

static void start_install(struct form *f, struct inst *in)
{
	gather(f, in);
	in->pct = 0;
	in->finished = in->failed = 0;
	strncpy0(in->status, "Starting", sizeof in->status);
	running = in;
	show_page(f, in, P_WORK);
	form_tick(f, 250);
	task_spawn("install", install_task, in, NULL);
}

static void sync_clock(struct form *f, struct inst *in)
{
	/* try the server with the zone chosen here */
	const char *server = form_text(f, in->ntp);
	int z = (int)form_val(f, in->zone);
	setting_set("clock.tz", tz_zones[z].tz);
	setting_set("clock.dst", tz_zones[z].dst);
	int64_t utc;
	char err[96], msg[128];
	if (ntp_query(server, &utc, err, sizeof err)) {
		snprintf(msg, sizeof msg, "No: %s", err);
	} else {
		int y, mo, d, h, mi, s;
		ntp_utc_to_local(utc, &y, &mo, &d, &h, &mi, &s);
		clock_set(y, mo, d, h, mi, s);
		snprintf(msg, sizeof msg, "Clock set: %04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
	}
	form_set_text(f, in->syncmsg, msg);
}

static void inst_event(struct form *f, int id, int ev)
{
	struct inst *in = form_priv(f);
	if (ev == FE_CLOSE) {
		if (running == in && !in->finished)
			in->closed = 1;		/* the task frees it */
		else
			kfree(in);
		return;
	}
	if (ev == FE_TICK) {
		if (in->page == P_WORK) {
			form_set_text(f, in->what, in->status);
			form_redraw(f);
			if (in->finished) {
				form_tick(f, 0);
				if (in->failed) {
					form_set_text(f, in->done_msg, in->err);
					form_hide(f, in->reboot, 1);
				} else {
					char m[160];
					snprintf(m, sizeof m, "bdos is %s on %s. Take the CD out, then restart.",
					    in->o.upgrade ? "upgraded" : "installed", blk_get(in->o.disk)->name);
					form_set_text(f, in->done_msg, m);
				}
				show_page(f, in, P_DONE);
				if (in->failed)
					form_hide(f, in->reboot, 1);
			}
		}
		return;
	}
	if (id == in->cancel && ev == FE_CLICK) {
		form_close(f);
		return;
	}
	if (id == in->back && ev == FE_CLICK && in->page > P_WELCOME && in->page < P_WORK) {
		int p = in->page - 1;
		if (is_upgrade(f, in) && p > P_DISK && p < P_SUMMARY)
			p = P_DISK;		/* an upgrade skips the settings pages */
		show_page(f, in, p);
		return;
	}
	if (id == in->next && ev == FE_CLICK) {
		if (in->page == P_DONE) {
			form_close(f);
			return;
		}
		if (!page_ok(f, in))
			return;
		if (in->page == P_SUMMARY) {
			start_install(f, in);
			return;
		}
		int p = in->page + 1;
		if (is_upgrade(f, in) && p > P_DISK && p < P_SUMMARY)
			p = P_SUMMARY;
		if (p == P_SUMMARY)
			fill_summary(f, in);
		show_page(f, in, p);
		return;
	}
	if (id == in->reboot && ev == FE_CLICK) {
		arch_reboot();
	}
	if ((id == in->disks && (ev == FE_CHANGE || ev == FE_CLICK)) || id == in->mode)
		disk_info(f, in);
	if (id == in->addr)
		show_page(f, in, in->page);
	if (id == in->ntp_on)
		form_enable(f, in->ntp, (int)form_val(f, in->ntp_on));
	if (id == in->sync && ev == FE_CLICK)
		sync_clock(f, in);
	if (id == in->disks && ev == FE_DBLCLICK && in->page == P_DISK && page_ok(f, in))
		show_page(f, in, is_upgrade(f, in) ? (fill_summary(f, in), P_SUMMARY) : P_NET);
}

static void draw_heading(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct inst *in = form_priv(f);
	uint32_t c = ui_colour(1);
	fill(s, r.x, r.y, r.w, r.h, col(c));
	text(s, r.x + 16, r.y + 9, titles[in->page], col(0xFFFFFF));
	/* steps: a dot per page you go through, the current one filled */
	int steps = P_WORK, x = r.x + r.w - 16 - steps * 14;
	for (int i = 0; i < steps; i++) {
		int on = i <= in->page;
		fill(s, x + i * 14, r.y + 13, 8, 8, col(on ? 0xFFFFFF : 0x000000));
		outline(s, x + i * 14, r.y + 13, 8, 8, col(0xFFFFFF));
	}
}

int installer_open(void *wmv)
{
	struct wm *wm = wmv;
	struct form *old = form_find(wm, "Install bdos");
	if (old) {
		form_raise(old);
		return 0;
	}
	struct inst *in = kzalloc(sizeof *in);
	if (!in)
		return -1;
	struct form *f = form_new("Install bdos", CW, CH, inst_event, in);
	if (!f) {
		kfree(in);
		return -1;
	}
	form_icon_name(f, "disk");
	install_defaults(&in->o);
	/* the frame: a heading, the pages below it, buttons at the bottom */
	in->heading = form_add(f, FW_CUSTOM, 0, 0, CW, 34, NULL);
	form_custom(f, in->heading, draw_heading);
	int y0 = 50;

	/* Welcome */
	add(f, in, P_WELCOME, FW_LABEL, 20, y0, 0, 0, "This puts bdos on a disk of this computer, so it starts from there.");
	add(f, in, P_WELCOME, FW_LABEL, 20, y0 + 22, 0, 0, install_is_live() ?
	    "You're running bdos from the CD now: nothing you change here is kept." :
	    "You can put bdos on another disk of this computer, or renew it there.");
	add(f, in, P_WELCOME, FW_LABEL, 20, y0 + 58, 0, 0, "What would you like to do?");
	in->mode = add(f, in, P_WELCOME, FW_CHOICE, 40, y0 + 82, 420, 24, NULL);
	static const char *const modes[] = { "Install bdos (erases the disk you choose)",
	    "Upgrade bdos on a disk (keeps your files and settings)" };
	form_options(f, in->mode, modes, 2);
	add(f, in, P_WELCOME, FW_LABEL, 20, y0 + 130, 0, 0, "To try bdos without installing, just close this window.");
	add(f, in, P_WELCOME, FW_LABEL, 20, y0 + 152, 0, 0, "On Proxmox: SATA or NVMe disk, Intel E1000 network, OVMF without Secure Boot.");

	/* Disk */
	add(f, in, P_DISK, FW_LABEL, 20, y0, 0, 0, "The disk for bdos:");
	in->disks = add(f, in, P_DISK, FW_LIST, 20, y0 + 22, CW - 40, 120, NULL);
	in->bar = add(f, in, P_DISK, FW_CUSTOM, 20, y0 + 156, CW - 40, 30, NULL);
	form_custom(f, in->bar, draw_bar);
	in->diskinfo = add(f, in, P_DISK, FW_LABEL, 20, y0 + 198, 0, 0, "");
	add(f, in, P_DISK, FW_LABEL, 20, y0 + 236, 0, 0, "Name of the new C: drive:");
	in->label = add(f, in, P_DISK, FW_EDIT, 220, y0 + 232, 140, 24, "BDOS");
	fill_disks(f, in);

	/* Computer and network */
	add(f, in, P_NET, FW_LABEL, 20, y0 + 4, 0, 0, "Computer name:");
	in->name = add(f, in, P_NET, FW_EDIT, 170, y0, 180, 24, in->o.hostname);
	add(f, in, P_NET, FW_LABEL, 20, y0 + 40, 0, 0, "Network address:");
	in->addr = add(f, in, P_NET, FW_CHOICE, 170, y0 + 36, 260, 24, NULL);
	static const char *const addrs[] = { "Automatic (DHCP)", "Fixed" };
	form_options(f, in->addr, addrs, 2);
	int fixed = in->o.ip[0] && strcasecmp(in->o.ip, "dhcp");
	form_set_val(f, in->addr, fixed);
	add(f, in, P_NET, FW_LABEL, 40, y0 + 74, 0, 0, "IP address:");
	in->ip = add(f, in, P_NET, FW_EDIT, 170, y0 + 70, 150, 24, fixed ? in->o.ip : "");
	add(f, in, P_NET, FW_LABEL, 40, y0 + 104, 0, 0, "Subnet mask:");
	in->mask = add(f, in, P_NET, FW_EDIT, 170, y0 + 100, 150, 24, in->o.mask);
	add(f, in, P_NET, FW_LABEL, 40, y0 + 134, 0, 0, "Gateway:");
	in->gw = add(f, in, P_NET, FW_EDIT, 170, y0 + 130, 150, 24, in->o.gateway);
	add(f, in, P_NET, FW_LABEL, 20, y0 + 168, 0, 0, "DNS server:");
	in->dns = add(f, in, P_NET, FW_EDIT, 170, y0 + 164, 150, 24, in->o.dns);
	add(f, in, P_NET, FW_LABEL, 330, y0 + 168, 0, 0, "(empty: from DHCP)");
	in->bterm = add(f, in, P_NET, FW_CHECK, 20, y0 + 204, 300, 22, "Let bterm connect from other computers");
	form_set_val(f, in->bterm, in->o.bterm);
	add(f, in, P_NET, FW_LABEL, 40, y0 + 236, 0, 0, "bterm password:");
	in->pass = add(f, in, P_NET, FW_EDIT, 170, y0 + 232, 150, 24, in->o.password);
	add(f, in, P_NET, FW_LABEL, 330, y0 + 236, 0, 0, "(empty: none)");
	in->telnet = add(f, in, P_NET, FW_CHECK, 20, y0 + 268, 300, 22, "Run a telnet server (port 23)");
	form_set_val(f, in->telnet, in->o.telnet);

	/* Time */
	add(f, in, P_TIME, FW_LABEL, 20, y0 + 4, 0, 0, "Time zone:");
	in->zone = add(f, in, P_TIME, FW_CHOICE, 130, y0, 380, 24, NULL);
	for (int i = 0; i < tz_nzones && i < 64; i++)
		in->zone_names[i] = tz_zones[i].name;
	form_options(f, in->zone, in->zone_names, MIN(tz_nzones, 64));
	form_set_val(f, in->zone, tz_find(in->o.tz, in->o.dst));
	in->ntp_on = add(f, in, P_TIME, FW_CHECK, 20, y0 + 44, 360, 22, "Set the clock from a time server on the Internet");
	form_set_val(f, in->ntp_on, 1);
	add(f, in, P_TIME, FW_LABEL, 40, y0 + 78, 0, 0, "Server:");
	in->ntp = add(f, in, P_TIME, FW_EDIT, 130, y0 + 74, 220, 24, in->o.ntp[0] ? in->o.ntp : "pool.ntp.org");
	in->sync = add(f, in, P_TIME, FW_BUTTON, 362, y0 + 72, 110, 26, "Try it now");
	in->syncmsg = add(f, in, P_TIME, FW_LABEL, 40, y0 + 112, 0, 0, "");
	add(f, in, P_TIME, FW_LABEL, 20, y0 + 150, 0, 0, "bdos keeps local time in the computer's clock, as DOS did, and the");
	add(f, in, P_TIME, FW_LABEL, 20, y0 + 170, 0, 0, "time server sets it by the zone above, with daylight saving.");

	/* Startup */
	in->desktop = add(f, in, P_START, FW_CHECK, 20, y0, 360, 22, "Start the desktop when bdos starts");
	form_set_val(f, in->desktop, in->o.desktop);
	add(f, in, P_START, FW_LABEL, 20, y0 + 40, 0, 0, "Theme:");
	in->theme = add(f, in, P_START, FW_CHOICE, 130, y0 + 36, 260, 24, NULL);
	in->nthemes = themes_list(in->themes, 24);
	int cur = 0;
	for (int i = 0; i < in->nthemes; i++) {
		in->theme_names[i] = in->themes[i];
		if (!strcasecmp(in->themes[i], in->o.theme))
			cur = i;
	}
	form_options(f, in->theme, in->theme_names, in->nthemes);
	form_set_val(f, in->theme, cur);
	add(f, in, P_START, FW_LABEL, 20, y0 + 80, 0, 0, "Everything here can be changed later in the Control Panel.");

	/* Summary */
	add(f, in, P_SUMMARY, FW_LABEL, 20, y0, 0, 0, "bdos is about to:");
	in->summary = add(f, in, P_SUMMARY, FW_LIST, 20, y0 + 22, CW - 40, 200, NULL);
	add(f, in, P_SUMMARY, FW_LABEL, 20, y0 + 236, 0, 0, "Press Install to go ahead. This can't be undone.");

	/* Installing */
	in->what = add(f, in, P_WORK, FW_LABEL, 20, y0 + 10, 0, 0, "");
	in->progress = add(f, in, P_WORK, FW_CUSTOM, 20, y0 + 40, CW - 40, 28, NULL);
	form_custom(f, in->progress, draw_progress);

	/* Done */
	in->done_msg = add(f, in, P_DONE, FW_LABEL, 20, y0 + 10, 0, 0, "");
	in->reboot = add(f, in, P_DONE, FW_BUTTON, 20, y0 + 50, 140, 28, "Restart now");

	/* buttons */
	in->back = form_add(f, FW_BUTTON, CW - 3 * 96 - 12, CH - 40, 88, 28, "< Back");
	in->next = form_add(f, FW_BUTTON, CW - 2 * 96 - 12, CH - 40, 88, 28, "Next >");
	in->cancel = form_add(f, FW_BUTTON, CW - 96 - 12, CH - 40, 88, 28, "Cancel");
	form_default(f, in->next);
	show_page(f, in, P_WELCOME);
	return form_show(wm, f, -1, -1);
}
