/*
 * ctlpanel.c - the Control Panel, its applets, and the Clock.
 *
 * Every applet is a form (form.h) and every change goes through the
 * settings store (settings.h), so what you set here is exactly what
 * CONTROL key=value and OS_SetSetting set, and it's saved in
 * C:\BDOS\SETTINGS.INI.
 */
#include "../kernel.h"
#include "../vfs.h"
#include "../settings.h"
#include "../audio.h"
#include "../net/net.h"
#include "../net/ntp.h"
#include "gfx.h"
#include "form.h"
#include "themes.h"

static void open_date(struct wm *wm);
static void open_network(struct wm *wm);
static void open_desktop(struct wm *wm);
static void open_themes(struct wm *wm);
static void open_mouse(struct wm *wm);
static void open_keyboard(struct wm *wm);
static void open_system(struct wm *wm);
static void open_dos(struct wm *wm);
static void open_sound(struct wm *wm);

/* bring an already-open applet forward instead of opening it twice */
static int raise_existing(struct wm *wm, const char *title)
{
	struct form *f = form_find(wm, title);
	if (!f)
		return 0;
	form_raise(f);
	return 1;
}

/* buttons along the bottom right: returns the id of the first */
static int ok_cancel(struct form *f, int cw, int ch, int apply)
{
	int bw = 76, gap = 8, n = apply ? 3 : 2;
	int x = cw - n * bw - (n - 1) * gap - 12, y = ch - 36;
	int ok = form_add(f, FW_BUTTON, x, y, bw, 26, "OK");
	form_default(f, ok);
	form_add(f, FW_BUTTON, x + bw + gap, y, bw, 26, "Cancel");
	if (apply)
		form_add(f, FW_BUTTON, x + 2 * (bw + gap), y, bw, 26, "Apply");
	return ok;
}

/* ---- the Control Panel window ---------------------------------------------------------- */

static const struct applet {
	const char *name, *icon, *desc, *key;
	void (*open)(struct wm *wm);
} applets[] = {
	{ "Date/Time", "calendar", "Set the date and time, and how the clock looks", "date", open_date },
	{ "Network", "network", "Name this computer and see who else is on the network", "network", open_network },
	{ "Desktop", "desktop", "Background colour, and starting the desktop at boot", "desktop", open_desktop },
	{ "Themes", "palette", "Window colours, desktop colour and pattern, all at once", "themes", open_themes },
	{ "Mouse", "mouse", "Pointer speed, double-click speed, button swap", "mouse", open_mouse },
	{ "Keyboard", "keyboard", "How soon and how fast keys repeat", "keyboard", open_keyboard },
	{ "DOS", "game", "How fast DOS programs run, and how their screens fit", "dos", open_dos },
	{ "Sound", "speaker", "Volume, where this session's sound is heard, and what's playing", "sound", open_sound },
	{ "System", "computer", "What this machine is and how it's doing", "system", open_system },
};

#define CP_CELL_W 92
#define CP_CELL_H 70

struct cpanel {
	int icon0, status;
};

static void cp_layout(struct form *f)
{
	struct cpanel *cp = form_priv(f);
	int w, h;
	form_size(f, &w, &h);
	int per_row = MAX(1, (w - 16) / CP_CELL_W);
	for (int i = 0; i < (int)NELEM(applets); i++)
		form_move(f, cp->icon0 + i, 8 + (i % per_row) * CP_CELL_W, 8 + (i / per_row) * CP_CELL_H,
		    CP_CELL_W, 58);
	form_move(f, cp->status, 0, h - 24, w, 24);
}

static void cp_status(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct cpanel *cp = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(ui_colour(0)));
	hline(s, r.x, r.y, r.w, col(0x808080));
	hline(s, r.x, r.y + 1, r.w, col(0xFFFFFF));
	int i = form_focused(f) - cp->icon0;
	if (i >= 0 && i < (int)NELEM(applets))
		text(s, r.x + 8, r.y + 5, applets[i].desc, col(0));
}

static void cp_event(struct form *f, int id, int ev)
{
	struct cpanel *cp = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(cp);
		return;
	}
	if (ev == FE_RESIZE) {
		cp_layout(f);
		return;
	}
	int i = id - cp->icon0;
	if (ev == FE_DBLCLICK && i >= 0 && i < (int)NELEM(applets))
		applets[i].open(form_wm(f));
}

static void open_panel(struct wm *wm)
{
	if (raise_existing(wm, "Control Panel"))
		return;
	struct cpanel *cp = kzalloc(sizeof *cp);
	if (!cp)
		return;
	int cw = 4 * CP_CELL_W + 16, ch = 2 * CP_CELL_H + 40;
	struct form *f = form_new("Control Panel", cw, ch, cp_event, cp);
	if (!f) {
		kfree(cp);
		return;
	}
	for (int i = 0; i < (int)NELEM(applets); i++) {
		int id = form_add(f, FW_ICON, 0, 0, CP_CELL_W, 58, applets[i].name);
		form_icon(f, id, applets[i].icon);
		if (!i)
			cp->icon0 = id;
	}
	cp->status = form_add(f, FW_CUSTOM, 0, 0, cw, 24, NULL);
	form_custom(f, cp->status, cp_status);
	form_resizable(f, CP_CELL_W + 16, CP_CELL_H + 40);
	cp_layout(f);
	form_show(wm, f, 60, 60);
}

int control_open(void *wmv, const char *applet)
{
	struct wm *wm = wmv;
	if (!applet || !*applet) {
		open_panel(wm);
		return 0;
	}
	static const struct { const char *alias, *key; } aliases[] = {
		{ "time", "date" }, { "datetime", "date" }, { "clock", "date" }, { "net", "network" },
		{ "colours", "themes" }, { "color", "themes" }, { "colour", "themes" }, { "colors", "themes" },
		{ "theme", "themes" }, { "kbd", "keyboard" },
		{ "sys", "system" }, { "display", "desktop" }, { "audio", "sound" }, { "volume", "sound" },
	};
	const char *key = applet;
	for (size_t i = 0; i < NELEM(aliases); i++)
		if (!strcasecmp(applet, aliases[i].alias))
			key = aliases[i].key;
	for (size_t i = 0; i < NELEM(applets); i++)
		if (!strcasecmp(key, applets[i].key) || !strcasecmp(key, applets[i].name)) {
			applets[i].open(wm);
			return 0;
		}
	return -1;
}

/* ---- Date/Time -------------------------------------------------------------------------------- */

static const char *const month_names[] = {
	"January", "February", "March", "April", "May", "June", "July", "August", "September",
	"October", "November", "December",
};

struct dt {
	int year, month, day, hour, min, sec, h24, secs, cal, now, ok, cancel, apply;
	int zone, ntp_on, ntp, sync, syncmsg;
	int edited;		/* the user has changed something: stop following the clock */
	const char *zones[64];
};

/* Sync now runs here, so the desktop carries on while the server answers */
static struct {
	volatile int busy, done;
	char msg[96];
} dt_sync;

static void dt_sync_task(void *arg)
{
	(void)arg;
	ntp_sync_now(dt_sync.msg, sizeof dt_sync.msg);
	dt_sync.done = 1;
	dt_sync.busy = 0;
}

/* the time zone and server as the form has them, into the settings */
static void dt_save_ntp(struct form *f)
{
	struct dt *d = form_priv(f);
	int z = (int)form_val(f, d->zone);
	if (z >= 0 && z < tz_nzones) {
		setting_set("clock.tz", tz_zones[z].tz);
		setting_set("clock.dst", tz_zones[z].dst);
	}
	setting_set("net.ntp", form_val(f, d->ntp_on) ? form_text(f, d->ntp) : "");
}

static void dt_load_clock(struct form *f)
{
	struct dt *d = form_priv(f);
	int y, mo, dd, h, mi, s;
	arch_rtc(&y, &mo, &dd, &h, &mi, &s);
	form_set_val(f, d->year, y);
	form_set_val(f, d->month, mo - 1);
	form_range(f, d->day, 1, clock_days_in_month(y, mo));
	form_set_val(f, d->day, dd);
	form_set_val(f, d->hour, h);
	form_set_val(f, d->min, mi);
	form_set_val(f, d->sec, s);
}

static void dt_show_now(struct form *f)
{
	struct dt *d = form_priv(f);
	int y, mo, dd, h, mi, s;
	arch_rtc(&y, &mo, &dd, &h, &mi, &s);
	char t[32], line[64];
	clock_format_time(t, sizeof t, h, mi, s, 1);
	static const char *const dn[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	char mon[4];
	memcpy(mon, month_names[mo - 1], 3);
	mon[3] = 0;
	snprintf(line, sizeof line, "%s %d %s %d, %s", dn[clock_dow(y, mo, dd)], dd, mon, y, t);
	form_set_text(f, d->now, line);
}

static void dt_calendar(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct dt *d = form_priv(f);
	int y = (int)form_val(f, d->year), mo = (int)form_val(f, d->month) + 1, sel = (int)form_val(f, d->day);
	fill(s, r.x, r.y, r.w, r.h, col(0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	static const char *const hdr[] = { "Su", "Mo", "Tu", "We", "Th", "Fr", "Sa" };
	int cw = (r.w - 4) / 7;
	fill(s, r.x + 2, r.y + 2, r.w - 4, 16, col(ui_colour(1)));
	for (int c = 0; c < 7; c++)
		text(s, r.x + 2 + c * cw + (cw - 16) / 2, r.y + 2, hdr[c], col(0xFFFFFF));
	int first = clock_dow(y, mo, 1), days = clock_days_in_month(y, mo);
	for (int dd = 1; dd <= days; dd++) {
		int cell = first + dd - 1, c = cell % 7, row = cell / 7;
		int x = r.x + 2 + c * cw, yy = r.y + 19 + row * 15;
		char n[4];
		snprintf(n, sizeof n, "%d", dd);
		int tw = (int)strlen(n) * FONT_W;
		if (dd == sel) {
			fill(s, x + 1, yy, cw - 2, 15, col(ui_colour(2)));
			text(s, x + (cw - tw) / 2, yy - 1, n, col(0xFFFFFF));
		} else {
			text(s, x + (cw - tw) / 2, yy - 1, n, col(c == 0 ? 0xB03030 : 0));
		}
	}
}

static void dt_event(struct form *f, int id, int ev)
{
	struct dt *d = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(d);
		return;
	}
	if (ev == FE_TICK) {
		if (!d->edited)
			dt_load_clock(f);
		dt_show_now(f);
		if (dt_sync.done) {
			dt_sync.done = 0;
			form_set_text(f, d->syncmsg, dt_sync.msg);
			form_enable(f, d->sync, 1);
		}
		return;
	}
	if (id == d->sync && ev == FE_CLICK) {
		if (dt_sync.busy)
			return;
		dt_save_ntp(f);
		if (!*setting("net.ntp")) {
			form_set_text(f, d->syncmsg, "Tick the box and name a server first.");
			return;
		}
		dt_sync.busy = 1;
		dt_sync.done = 0;
		d->edited = 0;
		form_set_text(f, d->syncmsg, "Asking the time server...");
		form_enable(f, d->sync, 0);
		task_spawn("ntpsync", dt_sync_task, NULL, NULL);
		return;
	}
	if (id == d->ntp_on && ev == FE_CHANGE) {
		form_enable(f, d->ntp, (int)form_val(f, d->ntp_on));
		return;
	}
	if (id == d->zone || id == d->ntp)
		return;
	if (id == d->cal && ev == FE_CLICK) {
		int px, py;
		form_point(f, &px, &py);
		int y = (int)form_val(f, d->year), mo = (int)form_val(f, d->month) + 1;
		int cw = (174 - 4) / 7, c = (px - 2) / cw, row = (py - 19) / 15;
		int dd = row * 7 + c - clock_dow(y, mo, 1) + 1;
		if (py >= 19 && c >= 0 && c < 7 && dd >= 1 && dd <= clock_days_in_month(y, mo)) {
			form_set_val(f, d->day, dd);
			d->edited = 1;
		}
		return;
	}
	if (ev == FE_CHANGE) {
		if (id == d->h24 || id == d->secs) {
			setting_set_int(id == d->h24 ? "clock.24h" : "clock.seconds", form_val(f, id));
			dt_show_now(f);
			return;
		}
		d->edited = 1;
		if (id == d->year || id == d->month) {	/* keep the day valid */
			int y = (int)form_val(f, d->year), mo = (int)form_val(f, d->month) + 1;
			form_range(f, d->day, 1, clock_days_in_month(y, mo));
		}
		return;
	}
	if (ev != FE_CLICK)
		return;
	if (id == d->ok || id == d->apply) {
		dt_save_ntp(f);
		if (d->edited) {
			int y = (int)form_val(f, d->year), mo = (int)form_val(f, d->month) + 1;
			int dd = (int)MIN(form_val(f, d->day), clock_days_in_month(y, mo));
			if (clock_set(y, mo, dd, (int)form_val(f, d->hour), (int)form_val(f, d->min),
			    (int)form_val(f, d->sec))) {
				wm_message(form_wm(f), "Date/Time", "That isn't a date and time bdos can set.");
				return;
			}
			d->edited = 0;
		}
		dt_show_now(f);
		if (id == d->ok)
			form_close(f);
	} else if (id == d->cancel) {
		form_close(f);
	}
}

static void open_date(struct wm *wm)
{
	if (raise_existing(wm, "Date/Time"))
		return;
	struct dt *d = kzalloc(sizeof *d);
	if (!d)
		return;
	int cw = 424, ch = 420;
	struct form *f = form_new("Date/Time", cw, ch, dt_event, d);
	if (!f) {
		kfree(d);
		return;
	}
	form_add(f, FW_GROUP, 8, 4, 196, 248, "Date");
	form_add(f, FW_LABEL, 20, 30, 0, 0, "Year");
	d->year = form_add(f, FW_SPIN, 84, 26, 108, 22, NULL);
	form_range(f, d->year, 2000, 2099);
	form_add(f, FW_LABEL, 20, 56, 0, 0, "Month");
	d->month = form_add(f, FW_CHOICE, 84, 52, 108, 22, NULL);
	form_options(f, d->month, month_names, 12);
	form_add(f, FW_LABEL, 20, 82, 0, 0, "Day");
	d->day = form_add(f, FW_SPIN, 84, 78, 108, 22, NULL);
	form_range(f, d->day, 1, 31);
	d->cal = form_add(f, FW_CUSTOM, 16, 108, 174, 112, NULL);
	form_custom(f, d->cal, dt_calendar);
	form_add(f, FW_LABEL, 18, 226, 0, 0, "Click a day to pick it");

	form_add(f, FW_GROUP, 212, 4, 204, 128, "Time");
	d->hour = form_add(f, FW_SPIN, 224, 28, 52, 22, "0");
	form_range(f, d->hour, 0, 23);
	form_add(f, FW_LABEL, 280, 31, 0, 0, ":");
	d->min = form_add(f, FW_SPIN, 290, 28, 52, 22, "0");
	form_range(f, d->min, 0, 59);
	form_add(f, FW_LABEL, 346, 31, 0, 0, ":");
	d->sec = form_add(f, FW_SPIN, 356, 28, 52, 22, "0");
	form_range(f, d->sec, 0, 59);
	d->h24 = form_add(f, FW_CHECK, 224, 62, 180, 20, "24-hour clock");
	form_set_val(f, d->h24, setting_int("clock.24h"));
	d->secs = form_add(f, FW_CHECK, 224, 86, 180, 20, "Show seconds on Clock");
	form_set_val(f, d->secs, setting_int("clock.seconds"));

	form_add(f, FW_GROUP, 212, 140, 204, 64, "The clock reads");
	d->now = form_add(f, FW_LABEL, 224, 166, 0, 0, "");
	form_add(f, FW_LABEL, 214, 214, 0, 0, "Changes take effect on OK.");

	form_add(f, FW_GROUP, 8, 258, cw - 16, 116, "Internet time");
	d->zone = form_add(f, FW_CHOICE, 18, 280, cw - 36, 22, NULL);
	for (int i = 0; i < tz_nzones && i < 64; i++)
		d->zones[i] = tz_zones[i].name;
	form_options(f, d->zone, d->zones, MIN(tz_nzones, 64));
	form_set_val(f, d->zone, tz_find(setting("clock.tz"), setting("clock.dst")));
	d->ntp_on = form_add(f, FW_CHECK, 18, 310, 120, 20, "Server:");
	form_set_val(f, d->ntp_on, *setting("net.ntp") != 0);
	d->ntp = form_add(f, FW_EDIT, 112, 308, 190, 22, *setting("net.ntp") ? setting("net.ntp") : "pool.ntp.org");
	form_enable(f, d->ntp, *setting("net.ntp") != 0);
	d->sync = form_add(f, FW_BUTTON, 310, 306, 98, 26, "Sync now");
	d->syncmsg = form_add(f, FW_LABEL, 18, 342, 0, 0, ntp_status());
	d->ok = ok_cancel(f, cw, ch, 1);
	d->cancel = d->ok + 1;
	d->apply = d->ok + 2;
	dt_load_clock(f);
	dt_show_now(f);
	form_tick(f, 500);
	form_show(wm, f, -1, -1);
}

/* ---- Network --------------------------------------------------------------------------------- */

struct netapp {
	int name, info, list, browse, shell, desk, ok, cancel, apply;
	int addr, ip, mask, gw, dns, bterm, pass, telnet, now;
	char orig[32];
};

/* NET VIEW runs in its own task so the desktop doesn't freeze while it listens */
static struct {
	int busy, done;
	char names[32][24];
	int n;
} browse;

static void browse_cb(const char *name, const uint8_t mac[6], void *arg)
{
	(void)mac; (void)arg;
	for (int i = 0; i < browse.n; i++)
		if (!strcasecmp(browse.names[i], name))
			return;
	if (browse.n < 32)
		strncpy0(browse.names[browse.n++], name, sizeof browse.names[0]);
}

static void browse_task(void *arg)
{
	(void)arg;
	browse.n = 0;
	net_view(1500, browse_cb, NULL);
	browse.done = 1;
	browse.busy = 0;
}

static void net_start_browse(struct form *f)
{
	struct netapp *n = form_priv(f);
	if (browse.busy || !net_present())
		return;
	browse.busy = 1;
	browse.done = 0;
	form_list_clear(f, n->list);
	form_set_text(f, n->list, "Looking...");
	task_spawn("netbrowse", browse_task, NULL, NULL);
	form_tick(f, 200);
}

/* what the address is right now, under the settings */
static void net_show_now(struct form *f)
{
	struct netapp *n = form_priv(f);
	char line[96] = "No network adapter found.";
	if (net_present()) {
		struct ip_info in;
		ip_get(&in);
		char a[16], g[16];
		ip_fmt(in.ip, a);
		ip_fmt(in.gw, g);
		snprintf(line, sizeof line, "Now: %s%s, gateway %s", in.ip ? a : "no address yet",
		    in.dhcp ? " (DHCP)" : "", in.gw ? g : "none");
	}
	form_set_text(f, n->now, line);
}

/* OK and Apply: check what's typed, then the settings (which take effect at once) */
static int net_apply(struct form *f)
{
	struct netapp *n = form_priv(f);
	struct wm *wm = form_wm(f);
	const char *nm = form_text(f, n->name);
	for (const char *p = nm; *p; p++)
		if (!isalnum((unsigned char)*p) && *p != '-' && *p != '_') {
			wm_message(wm, "Network", "A computer name can have letters, digits, - and _.");
			return 0;
		}
	int fixed = form_val(f, n->addr) == 1;
	uint32_t a;
	if (fixed && (ip_parse(form_text(f, n->ip), &a) || ip_parse(form_text(f, n->mask), &a) ||
	    (*form_text(f, n->gw) && ip_parse(form_text(f, n->gw), &a)))) {
		wm_message(wm, "Network", "A fixed address needs an IP address and a mask like 192.168.1.50 and 255.255.255.0 (and the gateway in the same form, if there is one).");
		return 0;
	}
	if (net_present() && *nm && strcasecmp(nm, n->orig)) {
		net_set_hostname(nm);
		vfs_spew("C:\\BDOS\\HOSTNAME.TXT", net_hostname(), strlen(net_hostname()));
		strncpy0(n->orig, net_hostname(), sizeof n->orig);
	}
	if (fixed) {
		setting_set("net.mask", form_text(f, n->mask));
		setting_set("net.gateway", form_text(f, n->gw));
		setting_set("net.ip", form_text(f, n->ip));
	} else {
		setting_set("net.ip", "dhcp");
	}
	setting_set("net.dns", form_text(f, n->dns));
	setting_set_int("net.udp", form_val(f, n->bterm));
	setting_set("net.password", form_text(f, n->pass));
	setting_set_int("net.telnet", form_val(f, n->telnet));
	net_show_now(f);
	return 1;
}

static void net_event(struct form *f, int id, int ev)
{
	struct netapp *n = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(n);
		return;
	}
	if (ev == FE_TICK) {
		net_show_now(f);
		if (browse.done) {
			browse.done = 0;
			form_tick(f, 1000);	/* on with the address line */
			form_list_clear(f, n->list);
			for (int i = 0; i < browse.n; i++) {
				char line[48];
				int me = !strcasecmp(browse.names[i], net_hostname());
				snprintf(line, sizeof line, "\\\\%s%s", browse.names[i], me ? "  (this one)" : "");
				form_list_add(f, n->list, line);
			}
			form_set_text(f, n->list, "Nobody answered");
		}
		return;
	}
	const char *pick = NULL;
	char host[32] = "";
	long sel = form_val(f, n->list);
	if (sel >= 0 && sel < browse.n)
		strncpy0(host, browse.names[sel], sizeof host);
	pick = host[0] ? host : NULL;
	if (id == n->browse && ev == FE_CLICK) {
		net_start_browse(f);
	} else if ((id == n->shell && ev == FE_CLICK) || (id == n->list && ev == FE_DBLCLICK)) {
		if (pick) {
			char cmd[64];
			snprintf(cmd, sizeof cmd, "CTTY \\\\%s", pick);
			wm_launch(form_wm(f), cmd, cmd);
		}
	} else if (id == n->desk && ev == FE_CLICK) {
		if (pick) {
			char cmd[64];
			snprintf(cmd, sizeof cmd, "WIN \\\\%s", pick);
			wm_launch(form_wm(f), cmd, cmd);
		}
	} else if (id == n->addr && ev == FE_CHANGE) {
		int fixed = form_val(f, n->addr) == 1;
		form_enable(f, n->ip, fixed);
		form_enable(f, n->mask, fixed);
		form_enable(f, n->gw, fixed);
	} else if (id == n->bterm && ev == FE_CHANGE) {
		form_enable(f, n->pass, (int)form_val(f, n->bterm));
	} else if ((id == n->ok || id == n->apply) && ev == FE_CLICK) {
		if (!net_apply(f))
			return;
		if (id == n->ok)
			form_close(f);
	} else if (id == n->cancel && ev == FE_CLICK) {
		form_close(f);
	}
}

static void open_network(struct wm *wm)
{
	if (raise_existing(wm, "Network"))
		return;
	struct netapp *n = kzalloc(sizeof *n);
	if (!n)
		return;
	int cw = 620, ch = 430;
	struct form *f = form_new("Network", cw, ch, net_event, n);
	if (!f) {
		kfree(n);
		return;
	}
	int up = net_present();
	form_add(f, FW_LABEL, 14, 14, 0, 0, "Computer name:");
	n->name = form_add(f, FW_EDIT, 136, 10, 200, 24, up ? net_hostname() : "");
	strncpy0(n->orig, up ? net_hostname() : "", sizeof n->orig);
	form_enable(f, n->name, up);
	form_add(f, FW_LABEL, 346, 14, 0, 0, "Others reach it as \\\\NAME.");
	char info[96];
	if (up) {
		uint8_t mac[6];
		net_mac(mac);
		snprintf(info, sizeof info, "Adapter: %s  %02x:%02x:%02x:%02x:%02x:%02x  link %s", net_adapter(),
		    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], net_link() ? "up" : "down");
	} else {
		strcpy(info, "No network adapter found. bdos has drivers for Intel E1000 cards.");
	}
	n->info = form_add(f, FW_LABEL, 14, 42, 0, 0, info);
	n->now = form_add(f, FW_LABEL, 14, 62, 0, 0, "");

	/* the address */
	form_add(f, FW_GROUP, 8, 88, 300, 176, "Address");
	n->addr = form_add(f, FW_CHOICE, 18, 112, 280, 22, NULL);
	static const char *const addrs[] = { "Automatic (DHCP)", "Fixed address" };
	form_options(f, n->addr, addrs, 2);
	int fixed = strcasecmp(setting("net.ip"), "dhcp") != 0;
	form_set_val(f, n->addr, fixed);
	form_add(f, FW_LABEL, 18, 146, 0, 0, "IP address:");
	n->ip = form_add(f, FW_EDIT, 132, 142, 166, 22, fixed ? setting("net.ip") : "");
	form_add(f, FW_LABEL, 18, 174, 0, 0, "Subnet mask:");
	n->mask = form_add(f, FW_EDIT, 132, 170, 166, 22, setting("net.mask"));
	form_add(f, FW_LABEL, 18, 202, 0, 0, "Gateway:");
	n->gw = form_add(f, FW_EDIT, 132, 198, 166, 22, setting("net.gateway"));
	form_add(f, FW_LABEL, 18, 234, 0, 0, "DNS server:");
	n->dns = form_add(f, FW_EDIT, 132, 230, 166, 22, setting("net.dns"));
	form_enable(f, n->ip, fixed);
	form_enable(f, n->mask, fixed);
	form_enable(f, n->gw, fixed);

	/* who may come in */
	form_add(f, FW_GROUP, 8, 272, 300, 102, "Others connecting");
	n->bterm = form_add(f, FW_CHECK, 18, 294, 270, 20, "bterm (shells, the desktop, T:)");
	form_set_val(f, n->bterm, setting_int("net.udp") != 0);
	form_add(f, FW_LABEL, 36, 322, 0, 0, "Password:");
	n->pass = form_add(f, FW_EDIT, 132, 318, 166, 22, setting("net.password"));
	form_enable(f, n->pass, setting_int("net.udp") != 0);
	n->telnet = form_add(f, FW_CHECK, 18, 348, 270, 20, "Telnet server on port 23");
	form_set_val(f, n->telnet, setting_int("net.telnet") != 0);

	/* the LAN */
	form_add(f, FW_GROUP, 316, 88, cw - 324, 286, "Machines on the network");
	n->list = form_add(f, FW_LIST, 326, 112, cw - 344, 196, up ? "Press Browse" : "");
	n->browse = form_add(f, FW_BUTTON, 326, 318, 88, 26, "Browse");
	n->shell = form_add(f, FW_BUTTON, 418, 318, 88, 26, "Shell...");
	n->desk = form_add(f, FW_BUTTON, 510, 318, 88, 26, "Desktop...");
	form_add(f, FW_LABEL, 326, 350, 0, 0, "T: there is your folder here.");
	form_enable(f, n->browse, up);
	form_enable(f, n->shell, up);
	form_enable(f, n->desk, up);
	n->ok = ok_cancel(f, cw, ch, 1);
	n->cancel = n->ok + 1;
	n->apply = n->ok + 2;
	net_show_now(f);
	form_show(wm, f, -1, -1);
	form_tick(f, 1000);
	if (up)
		net_start_browse(f);
}

/* ---- Desktop -------------------------------------------------------------------------------- */

static const uint32_t palette[] = {
	0x2F6F73, 0x3A3A6E, 0x5B3F72, 0x404850, 0x1D3A8A, 0x0F5F7F, 0x2F6F3F, 0x6E4A2F,
	0x8A2F3F, 0x000000, 0x505050, 0x808080, 0x008080, 0x204060, 0x603020, 0xA0A0C0,
};

struct deskapp {
	int tod, play, playing, play_min;
	int sw0, r, g, b, preview, boot, ok, cancel, apply;
	uint32_t orig, cur;
};

static void desk_sync(struct form *f)
{
	struct deskapp *d = form_priv(f);
	for (int i = 0; i < (int)NELEM(palette); i++)
		form_set_val(f, d->sw0 + i, palette[i] == d->cur);
	form_set_val(f, d->r, (d->cur >> 16) & 255);
	form_set_val(f, d->g, (d->cur >> 8) & 255);
	form_set_val(f, d->b, d->cur & 255);
}

static uint32_t now_colour(void)
{
	int y, mo, dd, h, mi, sec;
	arch_rtc(&y, &mo, &dd, &h, &mi, &sec);
	return time_of_day_colour(h * 60 + mi);
}

static void desk_preview(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct deskapp *d = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0x303030));
	int m = 6;
	uint32_t c = d->playing ? time_of_day_colour(d->play_min) : form_val(f, d->tod) ? now_colour() : d->cur;
	fill(s, r.x + m, r.y + m, r.w - 2 * m, r.h - 2 * m - 10, col(c));
	if (d->playing) {
		char t[16];
		snprintf(t, sizeof t, "%02d:%02d", d->play_min / 60, d->play_min % 60);
		text(s, r.x + r.w - 52, r.y + r.h - 34, t, col(0xFFFFFF));
	}
	/* a little window on it */
	int wx = r.x + 18, wy = r.y + 16, ww = r.w - 50, wh = r.h - 54;
	fill(s, wx, wy, ww, wh, col(ui_colour(0)));
	fill(s, wx + 2, wy + 2, ww - 4, 8, col(ui_colour(1)));
	fill(s, wx + 3, wy + 12, ww - 6, wh - 15, col(0xFFFFFF));
	fill(s, r.x + r.w / 2 - 12, r.y + r.h - 10, 24, 8, col(0x303030));
	bevel(s, r.x, r.y, r.w, r.h - 6, 1);
}

/* AUTOEXEC.BAT: a "WIN" line starts the desktop at boot; "REM WIN" doesn't */
static int autoexec_win_line(const char *line, int *commented)
{
	while (*line == ' ' || *line == '\t' || *line == '@')
		line++;
	*commented = 0;
	if (!strncasecmp(line, "REM ", 4)) {
		*commented = 1;
		line += 4;
		while (*line == ' ')
			line++;
	}
	if (strncasecmp(line, "WIN", 3))
		return 0;
	line += 3;
	while (*line == ' ' || *line == '\r')
		line++;
	return *line == 0 || *line == '\n';
}

static int autoexec_starts_desktop(void)
{
	size_t len;
	int err;
	char *src = vfs_slurp("C:\\AUTOEXEC.BAT", &len, &err);
	if (!src)
		return 0;
	int on = 0;
	for (char *p = src; *p; ) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		int com;
		if (autoexec_win_line(p, &com) && !com)
			on = 1;
		if (!e)
			break;
		p = e + 1;
	}
	kfree(src);
	return on;
}

static void autoexec_set_desktop(int on)
{
	size_t len = 0;
	int err;
	char *src = vfs_slurp("C:\\AUTOEXEC.BAT", &len, &err);
	char *out = kmalloc(len + 64);
	if (!out) {
		kfree(src);
		return;
	}
	size_t o = 0;
	int found = 0;
	for (char *p = src ? src : ""; *p; ) {
		char *e = strchr(p, '\n');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		char line[256];
		size_t cl = MIN(l, sizeof line - 1);
		memcpy(line, p, cl);
		line[cl] = 0;
		int com;
		if (autoexec_win_line(line, &com) && !found) {
			found = 1;
			o += (size_t)snprintf(out + o, len + 64 - o, on ? "WIN\r\n" : "REM WIN\r\n");
		} else if (autoexec_win_line(line, &com) && !com && !on) {
			o += (size_t)snprintf(out + o, len + 64 - o, "REM WIN\r\n");
		} else {
			memcpy(out + o, p, l);
			o += l;
			if (e)
				out[o++] = '\n';
		}
		if (!e)
			break;
		p = e + 1;
	}
	if (!found && on) {
		if (o && out[o - 1] != '\n')
			out[o++] = '\n';
		o += (size_t)snprintf(out + o, len + 64 - o, "WIN\r\n");
	}
	vfs_spew("C:\\AUTOEXEC.BAT", out, o);
	kfree(out);
	kfree(src);
}

static void desk_apply(struct form *f)
{
	struct deskapp *d = form_priv(f);
	struct wm *wm = form_wm(f);
	if (wm_depth(wm) == 0) {
		char hex[8];
		snprintf(hex, sizeof hex, "%06x", d->cur);
		setting_set("desktop.color", hex);	/* repaints every top-level desktop */
	} else {
		wm_set_desktop_colour(wm, d->cur);	/* nested: this one only, not saved */
	}
	d->orig = d->cur;
	if (wm_depth(wm) == 0)
		setting_set_int("desktop.timeofday", form_val(f, d->tod));
	int want = (int)form_val(f, d->boot);
	if (want != autoexec_starts_desktop())
		autoexec_set_desktop(want);
}

static void desk_event(struct form *f, int id, int ev)
{
	struct deskapp *d = form_priv(f);
	if (ev == FE_CLOSE) {
		if (d->playing)	/* put the real colour back */
			wm_set_desktop_colour(form_wm(f), setting_int("desktop.timeofday") ? now_colour() : d->orig);
		kfree(d);
		return;
	}
	if (ev == FE_TICK && d->playing) {	/* a whole day in five seconds */
		d->play_min += 12;
		if (d->play_min >= 24 * 60) {
			d->playing = 0;
			form_tick(f, 0);
			form_set_text(f, d->play, "Play a Day");
			wm_set_desktop_colour(form_wm(f), setting_int("desktop.timeofday") ? now_colour() : d->orig);
		} else {
			wm_set_desktop_colour(form_wm(f), time_of_day_colour(d->play_min));
		}
		return;
	}
	if (ev == FE_CLICK && id == d->play) {
		d->playing = !d->playing;
		d->play_min = 5 * 60;
		form_tick(f, d->playing ? 40 : 0);
		form_set_text(f, d->play, d->playing ? "Stop" : "Play a Day");
		if (!d->playing)
			wm_set_desktop_colour(form_wm(f), setting_int("desktop.timeofday") ? now_colour() : d->orig);
		return;
	}
	int i = id - d->sw0;
	if (ev == FE_CLICK && i >= 0 && i < (int)NELEM(palette)) {
		d->cur = palette[i];
		form_set_val(f, d->tod, 0);	/* a colour of your own: the sun stops choosing */
		desk_sync(f);
	} else if (ev == FE_CHANGE && (id == d->r || id == d->g || id == d->b)) {
		d->cur = (uint32_t)(form_val(f, d->r) << 16 | form_val(f, d->g) << 8 | form_val(f, d->b));
		form_set_val(f, d->tod, 0);
		for (int k = 0; k < (int)NELEM(palette); k++)
			form_set_val(f, d->sw0 + k, palette[k] == d->cur);
	} else if (ev == FE_CLICK && (id == d->ok || id == d->apply)) {
		desk_apply(f);
		if (id == d->ok)
			form_close(f);
	} else if (ev == FE_CLICK && id == d->cancel) {
		form_close(f);
	}
}

static void open_desktop(struct wm *wm)
{
	if (raise_existing(wm, "Desktop"))
		return;
	struct deskapp *d = kzalloc(sizeof *d);
	if (!d)
		return;
	int cw = 440, ch = 352;
	struct form *f = form_new("Desktop", cw, ch, desk_event, d);
	if (!f) {
		kfree(d);
		return;
	}
	d->orig = d->cur = wm_desktop_colour(wm);
	form_add(f, FW_GROUP, 8, 4, 252, 228, wm_depth(wm) ? "This nested desktop's colour" : "Background colour");
	for (int i = 0; i < (int)NELEM(palette); i++) {
		int id = form_add(f, FW_SWATCH, 22 + (i % 8) * 29, 30 + (i / 8) * 30, 22, 22, NULL);
		form_set_rgb(f, id, palette[i]);
		if (!i)
			d->sw0 = id;
	}
	static const char *const rgbl[] = { "Red", "Green", "Blue" };
	int *slots[] = { &d->r, &d->g, &d->b };
	for (int k = 0; k < 3; k++) {
		form_add(f, FW_LABEL, 20, 104 + k * 34, 0, 0, rgbl[k]);
		*slots[k] = form_add(f, FW_SLIDER, 72, 98 + k * 34, 176, 28, NULL);
		form_range(f, *slots[k], 0, 255);
	}
	form_add(f, FW_LABEL, 20, 206, 0, 0, "or pick a sample above");
	d->preview = form_add(f, FW_CUSTOM, 276, 18, 152, 120, NULL);
	form_custom(f, d->preview, desk_preview);
	if (wm_depth(wm)) {
		form_add(f, FW_LABEL, 272, 150, 0, 0, "Nested desktops keep");
		form_add(f, FW_LABEL, 272, 168, 0, 0, "their own colour.");
	}
	d->boot = form_add(f, FW_CHECK, 14, 244, 400, 22, "Start the desktop when bdos boots (AUTOEXEC.BAT)");
	d->tod = form_add(f, FW_CHECK, 14, 270, 290, 22, "Change colour with the time of day");
	form_set_val(f, d->tod, setting_int("desktop.timeofday"));
	form_enable(f, d->tod, wm_depth(wm) == 0);
	d->play = form_add(f, FW_BUTTON, cw - 124, 140, 112, 26, "Play a Day");
	form_set_val(f, d->boot, autoexec_starts_desktop());
	d->ok = ok_cancel(f, cw, ch, 1);
	d->cancel = d->ok + 1;
	d->apply = d->ok + 2;
	desk_sync(f);
	form_show(wm, f, -1, -1);
}

/* ---- Themes ------------------------------------------------------------------------------ */

struct themeapp {
	int list, preview, apply, saveas, del, ok, cancel;
	char names[40][48];
	int n;
	struct theme_file cur;
};

static void th_preview(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct themeapp *a = form_priv(f);
	struct theme_file *t = &a->cur;
	uint32_t desk = t->timeofday ? time_of_day_colour(12 * 60) : t->desktop;
	fill(s, r.x, r.y, r.w, r.h, col(desk));
	if (t->has_pattern)
		for (int y = r.y; y < r.y + r.h; y++)
			for (int x = r.x; x < r.x + r.w; x++)
				if (t->pattern[(y - r.y) & 7] & (0x80 >> ((x - r.x) & 7)))
					plot(s, x, y, col(t->pattern_ink));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	/* an inactive window behind an active one */
	int x = r.x + 12, y = r.y + 10, w = r.w - 70, h = 64;
	fill(s, x, y, w, h, col(t->face));
	bevel(s, x, y, w, h, 1);
	fill(s, x + 4, y + 4, w - 8, 18, col(t->inactive));
	text(s, x + 10, y + 5, "Inactive", col(t->title_text));
	x += 46;
	y += 36;
	w = r.w - 70;
	h = r.h - 54;
	fill(s, x, y, w, h, col(t->face));
	bevel(s, x, y, w, h, 1);
	fill(s, x + 4, y + 4, w - 8, 18, col(t->active));
	text(s, x + 10, y + 5, "Active window", col(t->title_text));
	fill(s, x + 4, y + 23, w - 8, 18, col(t->face));
	text(s, x + 10, y + 24, "File  Options", col(t->menu_text));
	fill(s, x + 6, y + 42, w - 12, h - 48, col(t->workspace));
	fill(s, x + 12, y + 50, 13 * FONT_W + 4, 16, col(t->select));
	text(s, x + 14, y + 50, "Selected text", col(0xFFFFFF));
	if (t->timeofday)
		text(s, r.x + 10, r.y + r.h - 22, "The desktop follows the sun", col(0xFFFFFF));
}

static void th_fill(struct form *f, const char *select)
{
	struct themeapp *a = form_priv(f);
	a->n = themes_list(a->names, 40);
	form_list_clear(f, a->list);
	int sel = 0;
	for (int i = 0; i < a->n; i++) {
		form_list_add(f, a->list, a->names[i]);
		if (!strcasecmp(a->names[i], select))
			sel = i;
	}
	form_set_val(f, a->list, sel);
	if (a->n)
		theme_load(a->names[sel], &a->cur);
}

static void th_saved(struct wm *wm, void *arg, const char *name)
{
	struct form *f = arg;
	if (!*name || strchr(name, '\\') || strchr(name, '/') || strchr(name, ':') || strchr(name, '.')) {
		if (*name)
			wm_message(wm, "Themes", "A theme name can't contain \\ / : or a dot.");
		return;
	}
	if (form_find(wm, "Themes") != f)
		return;	/* the applet closed meanwhile */
	struct theme_file t;
	theme_current(&t);
	strncpy0(t.name, name, sizeof t.name);
	int r = theme_save(&t);
	if (r) {
		wm_message(wm, "Themes", vfs_strerror(r));
		return;
	}
	setting_set("desktop.theme", name);
	th_fill(f, name);
	form_redraw(f);
}

static void th_deleted(struct wm *wm, void *arg, int yes)
{
	struct form *f = arg;
	if (!yes || form_find(wm, "Themes") != f)
		return;
	struct themeapp *a = form_priv(f);
	long i = form_val(f, a->list);
	if (i < 0 || i >= a->n)
		return;
	char path[VFS_PATH_MAX], fn[64];
	snprintf(fn, sizeof fn, "%s.THM", a->names[i]);
	vfs_join(THEMES_DIR, fn, path, sizeof path);
	vfs_delete(path);
	th_fill(f, setting("desktop.theme"));
	form_redraw(f);
}

static void th_event(struct form *f, int id, int ev)
{
	struct themeapp *a = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(a);
		return;
	}
	long i = form_val(f, a->list);
	const char *name = i >= 0 && i < a->n ? a->names[i] : "";
	if (ev == FE_CHANGE && id == a->list) {
		theme_load(name, &a->cur);
	} else if ((ev == FE_CLICK && (id == a->apply || id == a->ok)) || (ev == FE_DBLCLICK && id == a->list)) {
		if (*name)
			setting_set("desktop.theme", name);	/* the theme's colours, desktop and all */
		if (id == a->ok)
			form_close(f);
	} else if (ev == FE_CLICK && id == a->saveas) {
		ui_prompt(form_wm(f), "Save Theme", "Save what's on screen now as a theme called:", "My Theme", th_saved, f);
	} else if (ev == FE_CLICK && id == a->del && *name) {
		char msg[120];
		snprintf(msg, sizeof msg, "Delete the theme '%s'?%s", name,
		    theme_is_builtin(name) ? "\n(It's one of the originals.)" : "");
		ui_confirm(form_wm(f), "Delete Theme", msg, "Delete", th_deleted, f);
	} else if (ev == FE_CLICK && id == a->cancel) {
		form_close(f);
	}
}

static void open_themes(struct wm *wm)
{
	if (raise_existing(wm, "Themes"))
		return;
	struct themeapp *a = kzalloc(sizeof *a);
	if (!a)
		return;
	int cw = 520, ch = 330;
	struct form *f = form_new("Themes", cw, ch, th_event, a);
	if (!f) {
		kfree(a);
		return;
	}
	form_add(f, FW_LABEL, 12, 10, 0, 0, "Themes (files in C:\\BDOS\\THEMES):");
	a->list = form_add(f, FW_LIST, 12, 32, 180, 236, NULL);
	a->preview = form_add(f, FW_CUSTOM, 204, 32, cw - 216, 236, NULL);
	form_custom(f, a->preview, th_preview);
	a->saveas = form_add(f, FW_BUTTON, 12, ch - 38, 88, 26, "Save As...");
	a->del = form_add(f, FW_BUTTON, 106, ch - 38, 86, 26, "Delete...");
	a->ok = ok_cancel(f, cw, ch, 1);
	a->cancel = a->ok + 1;
	a->apply = a->ok + 2;
	th_fill(f, setting("desktop.theme"));
	form_focus(f, a->list);
	form_show(wm, f, -1, -1);
}

/* ---- Mouse --------------------------------------------------------------------------------- */

struct mouseapp {
	int speed, dclick, swap, test, ok, cancel;
	long o_speed, o_dclick, o_swap;
	int lit;
};

static void mouse_test(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct mouseapp *m = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(m->lit ? 0xE8C050 : 0xFFFFFF));
	bevel(s, r.x, r.y, r.w, r.h, 0);
	const char *t = m->lit ? "Got it!" : "TEST";
	text(s, r.x + (r.w - (int)strlen(t) * FONT_W) / 2, r.y + r.h / 2 - 8, t, col(0));
}

static void mouse_event(struct form *f, int id, int ev)
{
	struct mouseapp *m = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(m);
		return;
	}
	/* settings apply as you move them, so the test area is honest */
	if (ev == FE_CHANGE && id == m->speed)
		setting_set_int("mouse.speed", form_val(f, m->speed));
	else if (ev == FE_CHANGE && id == m->dclick)
		setting_set_int("mouse.dclick", 1000 - form_val(f, m->dclick) * 100);
	else if (ev == FE_CHANGE && id == m->swap)
		setting_set_int("mouse.swap", form_val(f, m->swap));
	else if (ev == FE_DBLCLICK && id == m->test)
		m->lit = !m->lit;
	else if (ev == FE_CLICK && id == m->ok)
		form_close(f);
	else if (ev == FE_CLICK && id == m->cancel) {
		setting_set_int("mouse.speed", m->o_speed);
		setting_set_int("mouse.dclick", m->o_dclick);
		setting_set_int("mouse.swap", m->o_swap);
		form_close(f);
	}
}

static void open_mouse(struct wm *wm)
{
	if (raise_existing(wm, "Mouse"))
		return;
	struct mouseapp *m = kzalloc(sizeof *m);
	if (!m)
		return;
	int cw = 380, ch = 268;
	struct form *f = form_new("Mouse", cw, ch, mouse_event, m);
	if (!f) {
		kfree(m);
		return;
	}
	m->o_speed = setting_int("mouse.speed");
	m->o_dclick = setting_int("mouse.dclick");
	m->o_swap = setting_int("mouse.swap");
	form_add(f, FW_GROUP, 8, 4, 236, 84, "Pointer speed");
	form_add(f, FW_LABEL, 18, 60, 0, 0, "Slow");
	form_add(f, FW_LABEL, 202, 60, 0, 0, "Fast");
	m->speed = form_add(f, FW_SLIDER, 18, 28, 216, 28, NULL);
	form_range(f, m->speed, 1, 10);
	form_set_val(f, m->speed, m->o_speed);
	form_add(f, FW_GROUP, 8, 96, 236, 84, "Double-click speed");
	form_add(f, FW_LABEL, 18, 152, 0, 0, "Slow");
	form_add(f, FW_LABEL, 202, 152, 0, 0, "Fast");
	m->dclick = form_add(f, FW_SLIDER, 18, 120, 216, 28, NULL);
	form_range(f, m->dclick, 1, 8);
	form_set_val(f, m->dclick, (1000 - m->o_dclick) / 100);
	form_add(f, FW_LABEL, 256, 14, 0, 0, "Double-click");
	form_add(f, FW_LABEL, 256, 30, 0, 0, "to test:");
	m->test = form_add(f, FW_CUSTOM, 256, 52, 110, 96, NULL);
	form_custom(f, m->test, mouse_test);
	if (input_mouse_is_abs())	/* vmmouse: the pointer is wherever the host cursor is */
		form_add(f, FW_LABEL, 14, 218, 0, 0, "Pointer follows your host's cursor here.");
	m->swap = form_add(f, FW_CHECK, 14, 192, 300, 22, "Swap left and right buttons");
	form_set_val(f, m->swap, m->o_swap);
	m->ok = ok_cancel(f, cw, ch, 0);
	m->cancel = m->ok + 1;
	form_show(wm, f, -1, -1);
}

/* ---- Keyboard ----------------------------------------------------------------------------- */

struct kbdapp {
	int delay, rate, test, ok, cancel;
	long o_delay, o_rate;
};

static void kbd_event(struct form *f, int id, int ev)
{
	struct kbdapp *k = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(k);
		return;
	}
	if (ev == FE_CHANGE && id == k->delay)
		setting_set_int("kbd.delay", 1000 - form_val(f, k->delay) * 250);	/* 0..3: long..short */
	else if (ev == FE_CHANGE && id == k->rate)
		setting_set_int("kbd.rate", form_val(f, k->rate));
	else if (ev == FE_CLICK && id == k->ok)
		form_close(f);
	else if (ev == FE_CLICK && id == k->cancel) {
		setting_set_int("kbd.delay", k->o_delay);
		setting_set_int("kbd.rate", k->o_rate);
		form_close(f);
	}
}

static void open_keyboard(struct wm *wm)
{
	if (raise_existing(wm, "Keyboard"))
		return;
	struct kbdapp *k = kzalloc(sizeof *k);
	if (!k)
		return;
	int cw = 380, ch = 262;
	struct form *f = form_new("Keyboard", cw, ch, kbd_event, k);
	if (!f) {
		kfree(k);
		return;
	}
	k->o_delay = setting_int("kbd.delay");
	k->o_rate = setting_int("kbd.rate");
	form_add(f, FW_GROUP, 8, 4, cw - 16, 82, "Delay before first repeat");
	form_add(f, FW_LABEL, 18, 60, 0, 0, "Long");
	form_add(f, FW_LABEL, cw - 66, 60, 0, 0, "Short");
	k->delay = form_add(f, FW_SLIDER, 18, 28, cw - 36, 28, NULL);
	form_range(f, k->delay, 0, 3);
	form_set_val(f, k->delay, (1000 - k->o_delay + 125) / 250);
	form_add(f, FW_GROUP, 8, 94, cw - 16, 82, "Repeat rate");
	form_add(f, FW_LABEL, 18, 150, 0, 0, "Slow");
	form_add(f, FW_LABEL, cw - 58, 150, 0, 0, "Fast");
	k->rate = form_add(f, FW_SLIDER, 18, 118, cw - 36, 28, NULL);
	form_range(f, k->rate, 2, 30);
	form_set_val(f, k->rate, k->o_rate);
	form_add(f, FW_LABEL, 14, 190, 0, 0, "Test:");
	k->test = form_add(f, FW_EDIT, 64, 186, cw - 80, 24, "");
	k->ok = ok_cancel(f, cw, ch, 0);
	k->cancel = k->ok + 1;
	form_show(wm, f, -1, -1);
}

/* ---- Sound ----------------------------------------------------------------------------- */

/*
 * This session's sound: the console's is the machine's (its sound card); a
 * bterm session's is played on the bterm's machine, or nowhere if it can't.
 * The volume and mute are this session's alone.
 */
struct soundapp {
	int where, vol, mute, remote, list, test, ok, cancel;
	int o_vol, o_mute;
	long o_remote;
};

static void sound_fill(struct form *f)
{
	struct soundapp *a = form_priv(f);
	form_list_clear(f, a->list);
	int n = 0;
	for (int i = 0; i < 64; i++) {
		char name[16], line[64];
		int vol;
		uint32_t rate;
		int r = audio_stream_info(i, name, sizeof name, &vol, &rate);
		if (r < 0)
			break;
		if (!r)
			continue;
		snprintf(line, sizeof line, "%-16s %u Hz", name[0] ? name : "(a program)", rate);
		form_list_add(f, a->list, line);
		n++;
	}
	if (!n)
		form_list_add(f, a->list, "Nothing playing");
}

static void sound_test(void *arg)
{
	(void)arg;
	audio_play_file("C:\\BDOS\\SOUNDS\\CHIMES.WAV", 1);
}

static void sound_event(struct form *f, int id, int ev)
{
	struct soundapp *a = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(a);
		return;
	}
	if (ev == FE_TICK)
		sound_fill(f);
	else if (ev == FE_CHANGE && id == a->vol)
		audio_set_master((int)(form_val(f, a->vol) * 256 / 100));
	else if (ev == FE_CHANGE && id == a->mute)
		audio_set_mute((int)form_val(f, a->mute));
	else if (ev == FE_CHANGE && id == a->remote)
		setting_set_int("sound.remote", form_val(f, a->remote));
	else if (ev == FE_CLICK && id == a->test)
		task_spawn("chimes", sound_test, NULL, NULL);	/* in this session, as everything started here */
	else if (ev == FE_CLICK && id == a->ok)
		form_close(f);
	else if (ev == FE_CLICK && id == a->cancel) {
		audio_set_master(a->o_vol);
		audio_set_mute(a->o_mute);
		setting_set_int("sound.remote", a->o_remote);
		form_close(f);
	}
}

static void open_sound(struct wm *wm)
{
	if (raise_existing(wm, "Sound"))
		return;
	struct soundapp *a = kzalloc(sizeof *a);
	if (!a)
		return;
	int cw = 400, ch = 340;
	struct form *f = form_new("Sound", cw, ch, sound_event, a);
	if (!f) {
		kfree(a);
		return;
	}
	char name[24], where[96];
	int audible;
	int sess = audio_session_info(name, sizeof name, &a->o_vol, &a->o_mute, &audible);
	if (sess == 0 && !strcmp(audio_device(), "none"))
		snprintf(where, sizeof where, "No sound card here (bterm sessions still sound)");
	else if (sess == 0)
		snprintf(where, sizeof where, "This machine's sound: %s", audio_device());
	else
		snprintf(where, sizeof where, "Your session (%s): %s", name,
		    audible ? "heard where you are" : "silent there");
	a->o_remote = setting_int("sound.remote");
	a->where = form_add(f, FW_LABEL, 14, 10, 0, 0, where);
	form_add(f, FW_GROUP, 8, 32, cw - 16, 84, "Volume");
	form_add(f, FW_LABEL, 18, 88, 0, 0, "Quiet");
	form_add(f, FW_LABEL, cw - 66, 88, 0, 0, "Loud");
	a->vol = form_add(f, FW_SLIDER, 18, 56, cw - 36, 28, NULL);
	form_range(f, a->vol, 0, 100);
	form_set_val(f, a->vol, (a->o_vol * 100 + 128) / 256);
	a->mute = form_add(f, FW_CHECK, 14, 124, 120, 22, "Mute");
	form_set_val(f, a->mute, a->o_mute);
	a->test = form_add(f, FW_BUTTON, cw - 104, 122, 90, 26, "Test");
	form_add(f, FW_LABEL, 14, 156, 0, 0, "Playing now:");
	a->list = form_add(f, FW_LIST, 14, 174, cw - 28, 82, NULL);
	a->remote = form_add(f, FW_CHECK, 14, 264, cw - 28, 22, "bterm sessions hear their own sound");
	form_set_val(f, a->remote, a->o_remote != 0);
	a->ok = ok_cancel(f, cw, ch, 0);
	a->cancel = a->ok + 1;
	sound_fill(f);
	form_tick(f, 500);
	form_show(wm, f, -1, -1);
}

/* ---- DOS ------------------------------------------------------------------------------- */

static const char *const dos_speed_names[] = { "XT (4.77 MHz)", "AT (286)", "386", "486", "Pentium",
	"As fast as possible" };
static const char *const dos_speed_keys[] = { "xt", "at", "386", "486", "pentium", "max" };
static const char *const dos_scale_names[] = { "Sharp: whole pixels, if they nearly fill it",
	"Smooth: keep the shape, fill the window", "Stretch: fill the whole window" };
static const char *const dos_scale_keys[] = { "sharp", "smooth", "stretch" };

struct dosapp {
	int speed, scale, trace, ok, cancel;
	char o_speed[16], o_scale[16], o_trace[8];
};

static void dos_apply(struct form *f)
{
	struct dosapp *d = form_priv(f);
	long v = form_val(f, d->speed);
	if (v >= 0 && v < (long)NELEM(dos_speed_keys))
		setting_set("dos.speed", dos_speed_keys[v]);
	v = form_val(f, d->scale);
	if (v >= 0 && v < (long)NELEM(dos_scale_keys))
		setting_set("dos.scale", dos_scale_keys[v]);
	/* keep a trace level of 2 or 3 someone set at the prompt, if the box stays ticked */
	int on = (int)form_val(f, d->trace), was = (int)strtol(d->o_trace, NULL, 10);
	setting_set("dos.trace", on ? (was ? d->o_trace : "1") : "0");
}

static void dos_event(struct form *f, int id, int ev)
{
	struct dosapp *d = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(d);
		return;
	}
	if (ev == FE_CHANGE && (id == d->scale))
		dos_apply(f);	/* a running game shows it at once */
	else if (ev == FE_CLICK && id == d->ok) {
		dos_apply(f);
		form_close(f);
	} else if (ev == FE_CLICK && id == d->cancel) {
		setting_set("dos.speed", d->o_speed);
		setting_set("dos.scale", d->o_scale);
		setting_set("dos.trace", d->o_trace);
		form_close(f);
	} else if (ev == FE_CLICK && id == d->ok + 2) {
		dos_apply(f);
	}
}

static void open_dos(struct wm *wm)
{
	if (raise_existing(wm, "DOS"))
		return;
	struct dosapp *d = kzalloc(sizeof *d);
	if (!d)
		return;
	int cw = 480, ch = 350;
	struct form *f = form_new("DOS", cw, ch, dos_event, d);
	if (!f) {
		kfree(d);
		return;
	}
	strncpy0(d->o_speed, setting("dos.speed"), sizeof d->o_speed);
	strncpy0(d->o_scale, setting("dos.scale"), sizeof d->o_scale);
	strncpy0(d->o_trace, setting("dos.trace"), sizeof d->o_trace);
	form_add(f, FW_GROUP, 8, 4, cw - 16, 84, "Speed");
	form_add(f, FW_LABEL, 18, 28, 0, 0, "DOS programs run at:");
	d->speed = form_add(f, FW_CHOICE, 190, 24, cw - 210, 24, NULL);
	form_options(f, d->speed, dos_speed_names, (int)NELEM(dos_speed_names));
	form_set_val(f, d->speed, 3);
	for (int i = 0; i < (int)NELEM(dos_speed_keys); i++)
		if (!strcasecmp(d->o_speed, dos_speed_keys[i]))
			form_set_val(f, d->speed, i);
	form_add(f, FW_LABEL, 18, 56, 0, 0, "A program's Properties, or its Program Manager item,");
	form_add(f, FW_LABEL, 18, 74, 0, 0, "can choose its own.");
	form_add(f, FW_GROUP, 8, 96, cw - 16, 82, "Screen, in a bigger window");
	d->scale = form_add(f, FW_CHOICE, 18, 120, cw - 36, 24, NULL);
	form_add(f, FW_LABEL, 18, 152, 0, 0, "A game shows the change at once.");
	form_options(f, d->scale, dos_scale_names, (int)NELEM(dos_scale_names));
	for (int i = 0; i < (int)NELEM(dos_scale_keys); i++)
		if (!strcasecmp(d->o_scale, dos_scale_keys[i]))
			form_set_val(f, d->scale, i);
	d->trace = form_add(f, FW_CHECK, 14, 188, cw - 28, 22, "When a program crashes, list what it last ran");
	form_set_val(f, d->trace, strtol(d->o_trace, NULL, 10) != 0);
	form_add(f, FW_GROUP, 8, 218, cw - 16, 82, "Keys in a DOS window");
	form_add(f, FW_LABEL, 18, 238, 0, 0, "Ctrl+Alt+F11   freeze it to a state file");
	form_add(f, FW_LABEL, 18, 256, 0, 0, "Ctrl+Alt+F12   go back to the frozen state");
	form_add(f, FW_LABEL, 18, 274, 0, 0, "Ctrl+Alt+F10   what the screen is doing (tracing on)");
	d->ok = ok_cancel(f, cw, ch, 1);
	d->cancel = d->ok + 1;
	form_show(wm, f, -1, -1);
}

/* ---- System ------------------------------------------------------------------------------ */

struct sysapp {
	int list, ok;
};

static void drive_line(struct vfs_drive *d, void *arg)
{
	struct form *f = arg;
	struct sysapp *s = form_priv(f);
	char line[96];
	snprintf(line, sizeof line, "  %c:  %s", d->letter, d->desc);
	form_list_add(f, s->list, line);
}

static void sys_fill(struct form *f)
{
	struct sysapp *s = form_priv(f);
	long keep = form_val(f, s->list);
	form_list_clear(f, s->list);
	char line[96], a[32], b[32];
	snprintf(line, sizeof line, "%s %s (%s) - %s", BDOS_NAME, BDOS_VERSION, ARCH_NAME, BDOS_LONGNAME);
	form_list_add(f, s->list, line);
	char cpu[64];
	arch_cpu_name(cpu, sizeof cpu);
	snprintf(line, sizeof line, "Processor: %s", cpu);
	form_list_add(f, s->list, line);
	fmt_commas(a, sizeof a, pmm_total() / 1024);
	fmt_commas(b, sizeof b, pmm_free_bytes() / 1024);
	snprintf(line, sizeof line, "Memory: %s K, %s K free", a, b);
	form_list_add(f, s->list, line);
	snprintf(line, sizeof line, "Display: %ux%u, 32-bit colour", (unsigned)bootinfo->fb.width,
	    (unsigned)bootinfo->fb.height);
	form_list_add(f, s->list, line);
	if (net_present())
		snprintf(line, sizeof line, "Network: %s as \\\\%s", net_adapter(), net_hostname());
	else
		snprintf(line, sizeof line, "Network: none");
	form_list_add(f, s->list, line);
	uint64_t up = uptime_ms() / 1000;
	snprintf(line, sizeof line, "Up for %lu:%02lu:%02lu", up / 3600, up / 60 % 60, up % 60);
	form_list_add(f, s->list, line);
	form_list_add(f, s->list, "Drives:");
	vfs_drives(drive_line, f);
	form_set_val(f, s->list, keep);
}

static void sys_event(struct form *f, int id, int ev)
{
	struct sysapp *s = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(s);
		return;
	}
	if (ev == FE_TICK)
		sys_fill(f);
	else if (ev == FE_RESIZE) {
		int w, h;
		form_size(f, &w, &h);
		form_move(f, s->list, 8, 8, w - 16, h - 56);
		form_move(f, s->ok, w - 88, h - 38, 76, 26);
	} else if (ev == FE_CLICK && id == s->ok)
		form_close(f);
}

static void open_system(struct wm *wm)
{
	if (raise_existing(wm, "System"))
		return;
	struct sysapp *s = kzalloc(sizeof *s);
	if (!s)
		return;
	int cw = 500, ch = 280;
	struct form *f = form_new("System", cw, ch, sys_event, s);
	if (!f) {
		kfree(s);
		return;
	}
	s->list = form_add(f, FW_LIST, 8, 8, cw - 16, ch - 56, NULL);
	s->ok = form_add(f, FW_BUTTON, cw - 88, ch - 38, 76, 26, "OK");
	form_default(f, s->ok);
	sys_fill(f);
	form_resizable(f, 300, 160);
	form_tick(f, 1000);
	form_show(wm, f, -1, -1);
}

/* ---- Clock --------------------------------------------------------------------------------- */

/* sin(i * 6 degrees) * 1024 for i = 0..15 */
static const int sin6[16] = { 0, 107, 213, 316, 416, 512, 602, 685, 761, 828, 887, 935, 974, 1002, 1018, 1024 };

static int isin(int i)	/* i in 60ths of a turn */
{
	i = ((i % 60) + 60) % 60;
	if (i <= 15)
		return sin6[i];
	if (i <= 30)
		return sin6[30 - i];
	if (i <= 45)
		return -sin6[i - 30];
	return -sin6[60 - i];
}

static int icos(int i) { return isin(i + 15); }

static int isqrt(int v)
{
	int r = 0;
	while ((r + 1) * (r + 1) <= v)
		r++;
	return r;
}

struct clockapp {
	int face;
	int digital;
};

static void hand(struct surface *s, int cx, int cy, int pos60, int len, int thick, uint32_t c)
{
	int ex = cx + isin(pos60) * len / 1024, ey = cy - icos(pos60) * len / 1024;
	for (int t = -thick; t <= thick; t++)
		for (int u = -thick; u <= thick; u++)
			line(s, cx + t, cy + u, ex + t, ey + u, c);
}

static void clock_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct clockapp *ca = form_priv(f);
	int y, mo, d, h, mi, sec;
	arch_rtc(&y, &mo, &d, &h, &mi, &sec);
	fill(s, r.x, r.y, r.w, r.h, col(ui_colour(0)));
	int show_s = setting_int("clock.seconds") != 0;
	if (ca->digital) {
		char t[32];
		clock_format_time(t, sizeof t, h, mi, sec, show_s);
		/* big digits: the 8x16 font, scaled up by sampling */
		int len = (int)strlen(t);
		int scale = MAX(1, MIN((r.w - 16) / (len * FONT_W), (r.h - 40) / FONT_H));
		struct surface tmp;
		if (surf_init(&tmp, len * FONT_W, FONT_H) == 0) {
			fill(&tmp, 0, 0, tmp.w, tmp.h, col(ui_colour(0)));
			text(&tmp, 0, 0, t, col(0x1D3A8A));
			int ox = r.x + (r.w - tmp.w * scale) / 2, oy = r.y + (r.h - tmp.h * scale) / 2 - 8;
			for (int j = 0; j < tmp.h; j++)
				for (int i = 0; i < tmp.w; i++) {
					uint32_t p = tmp.px[j * tmp.pitch + i];
					if (p != col(ui_colour(0)))
						fill(s, ox + i * scale, oy + j * scale, scale, scale, p);
				}
			surf_free(&tmp);
		}
		char dline[32];
		snprintf(dline, sizeof dline, "%04d-%02d-%02d", y, mo, d);
		text(s, r.x + (r.w - 10 * FONT_W) / 2, r.y + r.h - 24, dline, col(0));
		return;
	}
	int rad = MIN(r.w, r.h) / 2 - 8, cx = r.x + r.w / 2, cy = r.y + r.h / 2;
	if (rad < 10)
		return;
	for (int dy = -rad; dy <= rad; dy++) {	/* the face */
		int dx = isqrt(rad * rad - dy * dy);
		hline(s, cx - dx, cy + dy, 2 * dx + 1, col(0xFFFFFF));
	}
	for (int i = 0; i < 60; i++) {
		int big = i % 5 == 0, inner = rad - (big ? rad / 7 + 2 : rad / 14 + 1);
		int x0 = cx + isin(i) * inner / 1024, y0 = cy - icos(i) * inner / 1024;
		int x1 = cx + isin(i) * (rad - 2) / 1024, y1 = cy - icos(i) * (rad - 2) / 1024;
		line(s, x0, y0, x1, y1, col(big ? 0 : 0x808080));
		if (big && rad > 50) {
			line(s, x0 + 1, y0, x1 + 1, y1, col(0));
			line(s, x0, y0 + 1, x1, y1 + 1, col(0));
		}
	}
	int th = rad > 80 ? 2 : 1;
	hand(s, cx, cy, (h % 12) * 5 + mi / 12, rad * 5 / 10, th, col(0x202020));
	hand(s, cx, cy, mi, rad * 8 / 10, th > 1 ? 1 : 0, col(0x202020));
	if (show_s)
		hand(s, cx, cy, sec, rad * 9 / 10, 0, col(0xC03030));
	fill(s, cx - 3, cy - 3, 7, 7, col(0x202020));
}

/* minimized: a small face that keeps time (the hour and minute hands) */
static void clock_icon(struct form *f, struct surface *s, int x, int y)
{
	(void)f;
	int yy, mo, d, h, mi, sec;
	arch_rtc(&yy, &mo, &d, &h, &mi, &sec);
	int rad = 15, cx = x + 16, cy = y + 16;
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = isqrt(rad * rad - dy * dy);
		hline(s, cx - dx, cy + dy, 2 * dx + 1, col(0x000000));
	}
	rad -= 1;
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = isqrt(rad * rad - dy * dy);
		hline(s, cx - dx, cy + dy, 2 * dx + 1, col(0xFFFFFF));
	}
	for (int i = 0; i < 60; i += 5) {
		int px = cx + isin(i) * 12 / 1024, py = cy - icos(i) * 12 / 1024;
		fill(s, px, py, 1 + (i % 15 == 0), 1 + (i % 15 == 0), col(0x404040));
	}
	hand(s, cx, cy, (h % 12) * 5 + mi / 12, 7, 1, col(0x202020));
	hand(s, cx, cy, mi, 11, 0, col(0x202020));
	if (setting_int("clock.seconds"))
		hand(s, cx, cy, sec, 12, 0, col(0xC03030));
}

static void clock_event(struct form *f, int id, int ev)
{
	struct clockapp *ca = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(ca);
		return;
	}
	if (ev == FE_RESIZE) {
		int w, h;
		form_size(f, &w, &h);
		form_move(f, ca->face, 0, 0, w, h);
	} else if (ev == FE_CLICK && id == ca->face) {
		ca->digital = !ca->digital;	/* click to switch analogue/digital */
	}
}

void clock_open(struct wm *wm)
{
	if (raise_existing(wm, "Clock"))
		return;
	struct clockapp *ca = kzalloc(sizeof *ca);
	if (!ca)
		return;
	struct form *f = form_new("Clock", 200, 200, clock_event, ca);
	if (!f) {
		kfree(ca);
		return;
	}
	ca->face = form_add(f, FW_CUSTOM, 0, 0, 200, 200, NULL);
	form_custom(f, ca->face, clock_draw);
	form_resizable(f, 90, 90);
	form_icon_name(f, "clock");
	form_icon_draw(f, clock_icon);
	form_tick(f, 1000);
	form_show(wm, f, -1, 40);
}
