/*
 * sysapi.h - the SYS call table shared by BASIC and C programs.
 *
 *   BASIC:  SYS "Desk_Color", &204060
 *   C:      desk_color(0x204060);   (which calls the same entry)
 *
 * Every entry takes numbers and strings and may return one value.
 */
#ifndef BDOS_SYSAPI_H
#define BDOS_SYSAPI_H

struct sysval {
	int is_str;
	long long n;		/* integer argument / result */
	double f;		/* the same as a real, for languages that have them */
	char s[256];		/* string argument / result */
};

#define SYS_MAX_ARGS 8

typedef int (*sys_fn)(int argc, struct sysval *argv, struct sysval *ret);

struct sys_entry {
	const char *name;
	sys_fn fn;
	const char *help;	/* "Desk_Color colour  - repaint the desktop" */
};

void sys_register(const struct sys_entry *e);
const struct sys_entry *sys_find(const char *name);
/* 0 on success; negative: unknown name (-1) or the call's own error */
int sys_call(const char *name, int argc, struct sysval *argv, struct sysval *ret);
void sys_list(void (*cb)(const struct sys_entry *e, void *arg), void *arg);
void sysapi_init(void);

/* helpers for implementations */
static inline long long sys_int(struct sysval *v) { return v->is_str ? 0 : v->n; }

#endif
