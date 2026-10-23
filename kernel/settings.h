/* settings.h - machine settings (C:\BDOS\SETTINGS.INI) and the clock */
#ifndef BDOS_SETTINGS_H
#define BDOS_SETTINGS_H

#include <stdint.h>
#include <stddef.h>

struct setting_info {
	const char *key, *def, *help;
};

void settings_init(void);
const struct setting_info *settings_known(int *n);
const char *setting(const char *key);		/* the value, or its default, or "" */
long setting_int(const char *key);
uint32_t setting_rgb(const char *key);
int setting_set(const char *key, const char *val);	/* saves and notifies */
int setting_set_int(const char *key, long v);
/* fn(key) runs after each change ("" once at registration: apply everything) */
void setting_watch(void (*fn)(const char *key));
/* write the settings, with keys[i]=vals[i] instead, to another SETTINGS.INI */
int settings_write_to(const char *path, const char *const *keys, const char *const *vals, int n);

/* the real-time clock */
int clock_valid(int y, int mo, int d, int h, int mi, int s);
int clock_set(int y, int mo, int d, int h, int mi, int s);	/* 0 or -1 if invalid */
int clock_dow(int y, int mo, int d);			/* 0 = Sunday */
int clock_days_in_month(int y, int mo);
void clock_format_time(char *out, size_t n, int h, int mi, int s, int with_seconds);

#endif
