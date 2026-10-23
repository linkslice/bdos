#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
#define CLOCKS_PER_SEC 1000
struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
struct timespec {
	time_t tv_sec;
	long tv_nsec;
};
time_t time(time_t *t);
clock_t clock(void);
double difftime(time_t a, time_t b);
struct tm *gmtime(const time_t *t);
struct tm *localtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *r);
struct tm *localtime_r(const time_t *t, struct tm *r);
time_t mktime(struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
size_t strftime(char *s, size_t n, const char *fmt, const struct tm *tm);
int nanosleep(const struct timespec *req, struct timespec *rem);
#endif
