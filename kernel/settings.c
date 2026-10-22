/*
 * settings.c - machine settings, kept in C:\BDOS\SETTINGS.INI.
 *
 * One "key=value" per line. The Control Panel, the CONTROL command and the
 * OS_Setting SYS calls all read and write through here, and every change is
 * saved straight away and announced to whoever is watching (the mouse
 * driver, the desktop...), so it takes effect without a restart.
 */
#include "kernel.h"
#include "vfs.h"
#include "sysapi.h"
#include "settings.h"

#define SETTINGS_PATH "C:\\BDOS\\SETTINGS.INI"

static const struct setting_info known[] = {
	{ "desktop.color", "2f6f73", "colour of the top-level desktop (rrggbb)" },
	{ "desktop.theme", "bdos Classic", "theme from C:\\BDOS\\THEMES (Hot Dog Stand, Plan 9...)" },
	{ "desktop.timeofday", "0", "1: the desktop colour follows the time of day" },
	{ "mouse.speed", "5", "pointer speed, 1 (slow) to 10 (fast)" },
	{ "mouse.dclick", "450", "double-click time in milliseconds" },
	{ "mouse.swap", "0", "1 swaps the left and right buttons" },
	{ "kbd.delay", "500", "key repeat delay in milliseconds (250-1000)" },
	{ "kbd.rate", "10", "key repeats per second (2-30)" },
	{ "clock.24h", "1", "1 for a 24-hour clock, 0 for am/pm" },
	{ "net.ip", "dhcp", "dhcp, or a fixed address like 192.168.1.50" },
	{ "net.mask", "255.255.255.0", "subnet mask (fixed address only)" },
	{ "net.gateway", "", "default gateway (fixed address only)" },
	{ "net.dns", "", "DNS server (fixed address only)" },
	{ "net.udp", "1", "1 lets bterm and routed peers in over UDP" },
	{ "net.port", "17979", "UDP port for bterm and BDNET over UDP" },
	{ "net.password", "", "password bterm must give (empty: none)" },
	{ "clock.seconds", "1", "1 to show seconds on the clock" },
};

static struct {
	char key[32];
	char val[96];
} kv[64];
static int nkv;

static void (*watchers[8])(const char *key);
static int nwatchers;

const struct setting_info *settings_known(int *n)
{
	*n = (int)NELEM(known);
	return known;
}

static int find(const char *key)
{
	for (int i = 0; i < nkv; i++)
		if (!strcasecmp(kv[i].key, key))
			return i;
	return -1;
}

const char *setting(const char *key)
{
	int i = find(key);
	if (i >= 0)
		return kv[i].val;
	for (size_t k = 0; k < NELEM(known); k++)
		if (!strcasecmp(known[k].key, key))
			return known[k].def;
	return "";
}

long setting_int(const char *key)
{
	const char *v = setting(key);
	if (!*v)
		return 0;
	return strtol(v, NULL, 0);
}

uint32_t setting_rgb(const char *key)
{
	const char *v = setting(key);
	while (*v == '#' || *v == '&' || *v == ' ')
		v++;
	if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
		v += 2;
	return (uint32_t)strtol(v, NULL, 16) & 0xFFFFFF;
}

static void save(void)
{
	char *buf = kmalloc(8192);
	if (!buf)
		return;
	size_t o = (size_t)snprintf(buf, 8192, "; bdos settings - edit with CONTROL or the Control Panel\n");
	for (int i = 0; i < nkv && o < 8000; i++)
		o += (size_t)snprintf(buf + o, 8192 - o, "%s=%s\n", kv[i].key, kv[i].val);
	vfs_spew(SETTINGS_PATH, buf, o);
	kfree(buf);
}

static void notify(const char *key)
{
	for (int i = 0; i < nwatchers; i++)
		watchers[i](key);
}

int setting_set(const char *key, const char *val)
{
	if (!*key || strlen(key) >= sizeof kv[0].key)
		return -1;
	int i = find(key);
	if (i >= 0 && !strcmp(kv[i].val, val))
		return 0;
	if (i < 0) {
		if (nkv >= (int)NELEM(kv))
			return -1;
		i = nkv++;
		strncpy0(kv[i].key, key, sizeof kv[i].key);
		for (char *p = kv[i].key; *p; p++)
			*p = (char)tolower(*p);
	}
	strncpy0(kv[i].val, val, sizeof kv[i].val);
	save();
	notify(kv[i].key);
	return 0;
}

int setting_set_int(const char *key, long v)
{
	char b[24];
	snprintf(b, sizeof b, "%ld", v);
	return setting_set(key, b);
}

void setting_watch(void (*fn)(const char *key))
{
	if (nwatchers < (int)NELEM(watchers))
		watchers[nwatchers++] = fn;
	fn("");		/* "" = everything: apply what's loaded now */
}

/* ---- SYS calls --------------------------------------------------------------------- */

static int sys_setting(int argc, struct sysval *a, struct sysval *r)
{
	if (!argc || !a[0].is_str)
		return -2;
	r->is_str = 1;
	strncpy0(r->s, setting(a[0].s), sizeof r->s);
	return 0;
}

static int sys_set_setting(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	if (argc < 2 || !a[0].is_str)
		return -2;
	char v[96];
	if (a[1].is_str)
		strncpy0(v, a[1].s, sizeof v);
	else
		snprintf(v, sizeof v, "%lld", a[1].n);
	return setting_set(a[0].s, v);
}

static int sys_set_time(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	if (argc < 3)
		return -2;
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	return clock_set(y, mo, d, (int)a[0].n, (int)a[1].n, (int)a[2].n);
}

static int sys_set_date(int argc, struct sysval *a, struct sysval *r)
{
	(void)r;
	if (argc < 3)
		return -2;
	int y, mo, d, h, mi, s;
	arch_rtc(&y, &mo, &d, &h, &mi, &s);
	return clock_set((int)a[0].n, (int)a[1].n, (int)a[2].n, h, mi, s);
}

static const struct sys_entry calls[] = {
	{ "OS_Setting", sys_setting, "OS_Setting key$ TO value$ - read a setting (CONTROL lists them)" },
	{ "OS_SetSetting", sys_set_setting, "OS_SetSetting key$, value - change a setting (saved)" },
	{ "OS_SetTime", sys_set_time, "OS_SetTime h, m, s     - set the clock" },
	{ "OS_SetDate", sys_set_date, "OS_SetDate y, m, d     - set the date" },
};

/* ---- the clock ----------------------------------------------------------------------- */

static int days_in_month(int y, int m)
{
	static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0))
		return 29;
	return dm[(m - 1) % 12];
}

int clock_valid(int y, int mo, int d, int h, int mi, int s)
{
	return y >= 2000 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= days_in_month(y, mo) &&
	    h >= 0 && h < 24 && mi >= 0 && mi < 60 && s >= 0 && s < 60;
}

int clock_set(int y, int mo, int d, int h, int mi, int s)
{
	if (!clock_valid(y, mo, d, h, mi, s))
		return -1;
	arch_rtc_set(y, mo, d, h, mi, s);
	return 0;
}

int clock_dow(int y, int mo, int d)
{
	/* Sakamoto's method: 0 = Sunday */
	static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	if (mo < 3)
		y--;
	return (y + y / 4 - y / 100 + y / 400 + t[mo - 1] + d) % 7;
}

int clock_days_in_month(int y, int mo)
{
	return days_in_month(y, mo);
}

/* "7:05:09 pm" or "19:05:09", following the clock settings */
void clock_format_time(char *out, size_t n, int h, int mi, int s, int with_seconds)
{
	if (setting_int("clock.24h")) {
		if (with_seconds)
			snprintf(out, n, "%02d:%02d:%02d", h, mi, s);
		else
			snprintf(out, n, "%02d:%02d", h, mi);
	} else {
		int h12 = h % 12 ? h % 12 : 12;
		if (with_seconds)
			snprintf(out, n, "%d:%02d:%02d %s", h12, mi, s, h < 12 ? "am" : "pm");
		else
			snprintf(out, n, "%d:%02d %s", h12, mi, h < 12 ? "am" : "pm");
	}
}

/* ---- loading ------------------------------------------------------------------------- */

void settings_init(void)
{
	size_t len;
	int err;
	char *src = vfs_slurp(SETTINGS_PATH, &len, &err);
	if (src) {
		char *p = src;
		while (*p) {
			char *e = p;
			while (*e && *e != '\n')
				e++;
			char save_ch = *e;
			*e = 0;
			char *line = p;
			while (*line == ' ' || *line == '\t')
				line++;
			char *eq = strchr(line, '=');
			if (*line && *line != ';' && *line != '#' && eq && nkv < (int)NELEM(kv)) {
				*eq = 0;
				char *k = line, *v = eq + 1;
				size_t kl = strlen(k);
				while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t'))
					k[--kl] = 0;
				while (*v == ' ' || *v == '\t')
					v++;
				size_t vl = strlen(v);
				while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\r' || v[vl - 1] == '\t'))
					v[--vl] = 0;
				if (*k && find(k) < 0) {
					strncpy0(kv[nkv].key, k, sizeof kv[0].key);
					for (char *q = kv[nkv].key; *q; q++)
						*q = (char)tolower(*q);
					strncpy0(kv[nkv].val, v, sizeof kv[0].val);
					nkv++;
				}
			}
			*e = save_ch;
			p = *e ? e + 1 : e;
		}
		kfree(src);
	}
	for (size_t i = 0; i < NELEM(calls); i++)
		sys_register(&calls[i]);
}
