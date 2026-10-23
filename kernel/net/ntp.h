/* ntp.h - setting the clock from a time server, and time zones */
#ifndef BDOS_NTP_H
#define BDOS_NTP_H
#include <stdint.h>
#include <stddef.h>

struct tz_zone {
	const char *name;	/* "Central - Chicago (UTC-6)" */
	const char *tz;		/* clock.tz */
	const char *dst;	/* clock.dst */
};
extern const struct tz_zone tz_zones[];
extern const int tz_nzones;
int tz_find(const char *tz, const char *dst);	/* the closest zone, 0 for UTC */

void ntp_init(void);
int ntp_query(const char *server, int64_t *utc, char *err, size_t n);
int ntp_sync_now(char *msg, size_t n);		/* 0, and what happened in msg */
const char *ntp_status(void);
void ntp_request(void);				/* the background task syncs soon */
void ntp_utc_to_local(int64_t utc, int *y, int *mo, int *d, int *h, int *mi, int *s);
int ntp_cmd(int argc, char **argv);
#endif
