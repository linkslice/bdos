/*
 * ntp.c - setting the clock from a time server (SNTP, RFC 4330).
 *
 * bdos keeps local time in the real-time clock, as DOS did. A time server
 * gives UTC; the hours from UTC (clock.tz) and the daylight saving rule
 * (clock.dst: us, eu, none) turn it into local time.
 *
 * With net.ntp set, a task syncs once the network has an address, then
 * every six hours, and again whenever one of those settings changes. NTP at
 * the prompt shows how it went; NTP SYNC asks now.
 */
#include "../kernel.h"
#include "../settings.h"
#include "../vfs.h"
#include "net.h"
#include "ntp.h"

static volatile int got;
static uint32_t got_secs;
static uint64_t last_ok_ms;		/* uptime of the last good sync, 0 if none */
static char last_msg[96] = "not synced yet";
static volatile int want_sync;

const struct tz_zone tz_zones[] = {
	{ "UTC", "0", "none" },
	{ "Hawaii (UTC-10)", "-10", "none" },
	{ "Alaska (UTC-9)", "-9", "us" },
	{ "Pacific - Los Angeles (UTC-8)", "-8", "us" },
	{ "Mountain - Denver (UTC-7)", "-7", "us" },
	{ "Arizona (UTC-7, no DST)", "-7", "none" },
	{ "Central - Chicago (UTC-6)", "-6", "us" },
	{ "Eastern - New York (UTC-5)", "-5", "us" },
	{ "Atlantic - Halifax (UTC-4)", "-4", "us" },
	{ "Brazil - Sao Paulo (UTC-3)", "-3", "none" },
	{ "London, Lisbon (UTC+0)", "0", "eu" },
	{ "Central Europe - Berlin (UTC+1)", "+1", "eu" },
	{ "Eastern Europe - Helsinki (UTC+2)", "+2", "eu" },
	{ "Moscow, Istanbul (UTC+3)", "+3", "none" },
	{ "Dubai (UTC+4)", "+4", "none" },
	{ "India (UTC+5:30)", "+5:30", "none" },
	{ "China, Singapore (UTC+8)", "+8", "none" },
	{ "Japan, Korea (UTC+9)", "+9", "none" },
	{ "Sydney (UTC+10)", "+10", "none" },
	{ "Auckland (UTC+12)", "+12", "none" },
};
const int tz_nzones = (int)NELEM(tz_zones);

int tz_find(const char *tz, const char *dst)
{
	for (int i = 0; i < tz_nzones; i++)
		if (!strcmp(tz_zones[i].tz, tz) && !strcasecmp(tz_zones[i].dst, dst))
			return i;
	for (int i = 0; i < tz_nzones; i++)
		if (!strcmp(tz_zones[i].tz, tz))
			return i;
	return 0;
}

/* "-6", "+5:30" -> minutes */
static int tz_minutes(const char *s)
{
	int sign = 1;
	while (*s == ' ')
		s++;
	if (*s == '-' || *s == '+')
		sign = *s++ == '-' ? -1 : 1;
	int h = 0, m = 0;
	while (*s >= '0' && *s <= '9')
		h = h * 10 + (*s++ - '0');
	if (*s == ':' || *s == '.') {
		s++;
		while (*s >= '0' && *s <= '9')
			m = m * 10 + (*s++ - '0');
	}
	return sign * (h * 60 + m);
}

/* days since 1970-01-01 -> y, mo, d */
static void civil(int64_t z, int *y, int *mo, int *d)
{
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t yy = yoe + era * 400;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	*d = (int)(doy - (153 * mp + 2) / 5 + 1);
	*mo = (int)(mp < 10 ? mp + 3 : mp - 9);
	*y = (int)(yy + (*mo <= 2));
}

static int64_t days_from_civil(int y, int mo, int d)
{
	y -= mo <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

/* the day of the month of the nth Sunday (n = -1: the last) */
static int sunday(int y, int mo, int n)
{
	int days = clock_days_in_month(y, mo);
	if (n > 0) {
		int first = clock_dow(y, mo, 1);
		return 1 + (7 - first) % 7 + (n - 1) * 7;
	}
	int lastdow = clock_dow(y, mo, days);
	return days - lastdow;
}

/* is daylight saving time on at this UTC moment? */
static int dst_on(int64_t utc, int std_min, const char *rule)
{
	int y, mo, d;
	civil(utc / 86400 >= 0 ? utc / 86400 : (utc - 86399) / 86400, &y, &mo, &d);
	if (!strcasecmp(rule, "us")) {
		/* 2:00 local standard time on the second Sunday in March, till 2:00
		 * local daylight time on the first Sunday in November */
		int64_t start = days_from_civil(y, 3, sunday(y, 3, 2)) * 86400 + 2 * 3600 - std_min * 60;
		int64_t end = days_from_civil(y, 11, sunday(y, 11, 1)) * 86400 + 1 * 3600 - std_min * 60;
		return utc >= start && utc < end;
	}
	if (!strcasecmp(rule, "eu")) {
		/* 01:00 UTC on the last Sunday in March, till 01:00 UTC on the last in October */
		int64_t start = days_from_civil(y, 3, sunday(y, 3, -1)) * 86400 + 3600;
		int64_t end = days_from_civil(y, 10, sunday(y, 10, -1)) * 86400 + 3600;
		return utc >= start && utc < end;
	}
	return 0;
}

/* UTC seconds -> local y-m-d h:m:s, by the settings */
void ntp_utc_to_local(int64_t utc, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	int std = tz_minutes(setting("clock.tz"));
	int64_t t = utc + std * 60;
	if (dst_on(utc, std, setting("clock.dst")))
		t += 3600;
	int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
	int64_t rest = t - days * 86400;
	civil(days, y, mo, d);
	*h = (int)(rest / 3600);
	*mi = (int)(rest / 60 % 60);
	*s = (int)(rest % 60);
}

static void ntp_input(uint32_t src, uint16_t sport, const uint8_t *d, size_t n, void *arg)
{
	(void)src; (void)sport; (void)arg;
	if (n < 48 || (d[0] & 7) != 4)		/* mode 4: a server's answer */
		return;
	got_secs = (uint32_t)d[40] << 24 | (uint32_t)d[41] << 16 | (uint32_t)d[42] << 8 | d[43];
	got = 1;
}

int ntp_query(const char *server, int64_t *utc, char *err, size_t n)
{
	uint32_t ip;
	int r = dns_resolve(server, &ip);
	if (r) {
		snprintf(err, n, "%s: %s", server, vfs_strerror(r));
		return r;
	}
	uint8_t pkt[48];
	memset(pkt, 0, sizeof pkt);
	pkt[0] = 0x23;				/* version 4, client */
	uint16_t port = (uint16_t)(40000 + (uptime_us() % 9000));
	if (udp_bind(port, ntp_input, NULL)) {
		strncpy0(err, "no free port", n);
		return E_NOMEM;
	}
	r = E_TIMEDOUT;
	for (int attempt = 0; attempt < 3 && r; attempt++) {
		got = 0;
		ip_udp_send(ip, port, 123, pkt, sizeof pkt);
		uint64_t until = uptime_ms() + 2000;
		while (!got && uptime_ms() < until)
			task_idle();
		if (got && got_secs)
			r = 0;
	}
	udp_unbind(port);
	if (r) {
		snprintf(err, n, "%s didn't answer", server);
		return r;
	}
	*utc = (int64_t)got_secs - 2208988800LL;	/* 1900 -> 1970 */
	return 0;
}

int ntp_sync_now(char *msg, size_t n)
{
	const char *server = setting("net.ntp");
	if (!*server) {
		strncpy0(msg, "no time server set (net.ntp)", n);
		return -1;
	}
	struct ip_info in;
	ip_get(&in);
	if (!in.ip) {
		strncpy0(msg, "no network address yet", n);
		return -1;
	}
	int64_t utc;
	char err[80];
	int r = ntp_query(server, &utc, err, sizeof err);
	if (r) {
		snprintf(last_msg, sizeof last_msg, "failed: %s", err);
		strncpy0(msg, last_msg, n);
		return r;
	}
	int y, mo, d, h, mi, s;
	ntp_utc_to_local(utc, &y, &mo, &d, &h, &mi, &s);
	clock_set(y, mo, d, h, mi, s);
	last_ok_ms = uptime_ms();
	snprintf(last_msg, sizeof last_msg, "set from %s to %04d-%02d-%02d %02d:%02d:%02d", server, y, mo, d, h,
	    mi, s);
	strncpy0(msg, last_msg, n);
	return 0;
}

const char *ntp_status(void)
{
	return last_msg;
}

void ntp_request(void)
{
	want_sync = 1;
}

static void watch(const char *key)
{
	if (!*key || !strcmp(key, "net.ntp") || !strcmp(key, "clock.tz") || !strcmp(key, "clock.dst"))
		want_sync = 1;
}

static void ntp_task(void *arg)
{
	(void)arg;
	uint64_t next = 0;
	for (;;) {
		sleep_ms(1000);
		if (!*setting("net.ntp"))
			continue;
		struct ip_info in;
		ip_get(&in);
		if (!in.ip)
			continue;
		if (!want_sync && uptime_ms() < next)
			continue;
		want_sync = 0;
		char msg[96];
		int r = ntp_sync_now(msg, sizeof msg);
		/* six hours when it worked; a minute when it didn't */
		next = uptime_ms() + (r ? 60000ULL : 6ULL * 3600 * 1000);
	}
}

void ntp_init(void)
{
	setting_watch(watch);
	task_spawn("ntp", ntp_task, NULL, NULL);
}

/* NTP [SYNC | server] */
int ntp_cmd(int argc, char **argv)
{
	if (argc > 1 && strcasecmp(argv[1], "SYNC")) {
		setting_set("net.ntp", strcasecmp(argv[1], "OFF") ? argv[1] : "");
		if (!strcasecmp(argv[1], "OFF")) {
			kprintf("The clock won't be set from a time server.\n");
			return 0;
		}
	}
	if (argc > 1) {
		char msg[96];
		int r = ntp_sync_now(msg, sizeof msg);
		kprintf("Clock %s\n", msg);
		return r ? 1 : 0;
	}
	const char *server = setting("net.ntp");
	kprintf("Time server: %s\n", *server ? server : "none (NTP pool.ntp.org sets one)");
	kprintf("Time zone: UTC%s%s, daylight saving: %s\n", setting("clock.tz")[0] == '-' || setting("clock.tz")[0] == '+' ? "" : "+",
	    setting("clock.tz"), setting("clock.dst"));
	kprintf("Last sync: %s", last_msg);
	if (last_ok_ms)
		kprintf(" (%llu minutes ago)", (unsigned long long)((uptime_ms() - last_ok_ms) / 60000));
	kprintf("\n");
	return 0;
}
