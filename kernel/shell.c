/*
 * shell.c - COMMAND.COM for bdos.
 *
 * DOS commands with Unix aliases. A command remembers which name it was
 * invoked by: as DIR, '/' starts a switch and '\' separates paths; as ls,
 * '-' starts a switch and '/' separates paths.
 */
#include "kernel.h"

#define LINE_MAX	256
#define MAX_ARGS	16
#define HIST_MAX	16

struct ctx {
	int argc;
	char *argv[MAX_ARGS];
	int unix_style;		/* invoked through a Unix alias */
	const char *flags;	/* alias-supplied default flags, e.g. ls -> "W" */
};

typedef void (*cmd_fn)(struct ctx *);

struct command {
	const char *name;
	cmd_fn fn;
	const char *help;
};

struct alias {
	const char *name;
	const char *target;
	int unix_style;
};

static struct node *cwd;
static int echo_on = 1;
static char build_date[11];	/* YYYY-MM-DD, from __DATE__ */
static char build_time[6];	/* HH:MM */

/* ---- helpers ------------------------------------------------------------ */

static int is_switch(const char *arg, int unix_style)
{
	return unix_style ? (arg[0] == '-' && arg[1]) : arg[0] == '/';
}

/* Does the command line carry switch letter c (case-insensitive)? */
static int has_flag(struct ctx *c, char f)
{
	if (c->flags)
		for (const char *p = c->flags; *p; p++)
			if (toupper(*p) == toupper(f))
				return 1;
	for (int i = 1; i < c->argc; i++)
		if (is_switch(c->argv[i], c->unix_style))
			for (const char *p = c->argv[i] + 1; *p; p++)
				if (toupper(*p) == toupper(f))
					return 1;
	return 0;
}

/* First argument that isn't a switch, or NULL. */
static const char *first_arg(struct ctx *c)
{
	for (int i = 1; i < c->argc; i++)
		if (!is_switch(c->argv[i], c->unix_style))
			return c->argv[i];
	return NULL;
}

static void set_build_date(void)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char *d = __DATE__;	/* "Oct 19 2026" */
	int m = 1;
	for (int i = 0; i < 12; i++)
		if (!memcmp(months + i * 3, d, 3))
			m = i + 1;
	int day = (d[4] == ' ' ? 0 : d[4] - '0') * 10 + (d[5] - '0');
	snprintf(build_date, sizeof build_date, "%c%c%c%c-%02d-%02d",
	    d[7], d[8], d[9], d[10], m, day);
	strncpy0(build_time, __TIME__, sizeof build_time);
}

/* ---- CMOS clock --------------------------------------------------------- */

static uint8_t cmos(uint8_t reg)
{
	outb(0x70, reg);
	return inb(0x71);
}

static int bcd(uint8_t v, int is_bcd)
{
	return is_bcd ? (v & 0x0F) + (v >> 4) * 10 : v;
}

static void read_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	while (cmos(0x0A) & 0x80)	/* wait out an update in progress */
		cpu_pause();
	uint8_t regb = cmos(0x0B);
	int b = !(regb & 0x04);
	*s = bcd(cmos(0x00), b);
	*mi = bcd(cmos(0x02), b);
	uint8_t hr = cmos(0x04);
	*h = bcd(hr & 0x7F, b);
	if (!(regb & 0x02) && (hr & 0x80))	/* 12-hour clock, PM */
		*h = (*h % 12) + 12;
	*d = bcd(cmos(0x07), b);
	*mo = bcd(cmos(0x08), b);
	*y = 2000 + bcd(cmos(0x09), b);
}

/* ---- commands ----------------------------------------------------------- */

static void cmd_help(struct ctx *c);
static void cmd_alias(struct ctx *c);
static int run_line(char *line, int from_batch);

static void cmd_ver(struct ctx *c)
{
	(void)c;
	kprintf("\n%s version %s (%s)\n%s\n\n", BDOS_NAME, BDOS_VERSION, BDOS_ARCH,
	    BDOS_LONGNAME);
}

static void cmd_cls(struct ctx *c)
{
	(void)c;
	con_clear();
}

static void cmd_echo(struct ctx *c)
{
	if (c->argc == 1) {
		kprintf("ECHO is %s\n", echo_on ? "on" : "off");
		return;
	}
	if (c->argc == 2 && !strcasecmp(c->argv[1], "on")) {
		echo_on = 1;
		return;
	}
	if (c->argc == 2 && !strcasecmp(c->argv[1], "off")) {
		echo_on = 0;
		return;
	}
	if (c->argc == 2 && !strcmp(c->argv[1], ".")) {
		kprintf("\n");
		return;
	}
	for (int i = 1; i < c->argc; i++)
		kprintf("%s%s", c->argv[i], i + 1 < c->argc ? " " : "");
	kprintf("\n");
}

static void cmd_rem(struct ctx *c)
{
	(void)c;
}

static void list_long(struct node *dir, int show_hidden, int unix_style)
{
	char size[32], path[256];
	int files = 0, dirs = 0;
	uint64_t bytes = 0;

	if (!unix_style) {
		rd_path(dir, path, sizeof path, 0);
		kprintf("\n Volume in drive C is BDOS\n");
		kprintf(" Volume Serial Number is 2026-1019\n");
		kprintf(" Directory of %s\n\n", path);
	}
	for (struct node *n = dir->child; n; n = n->next) {
		if ((n->attr & ATTR_HIDDEN) && !show_hidden)
			continue;
		if (n->is_dir) {
			dirs++;
			if (unix_style)
				kprintf("d%s %13s  %s %s  %s/\n",
				    (n->attr & ATTR_RO) ? "r-" : "rw", "-",
				    build_date, build_time, n->name);
			else
				kprintf("%-24s %-13s  %s  %s\n", n->name, "<DIR>",
				    build_date, build_time);
		} else {
			files++;
			bytes += n->size;
			fmt_commas(size, sizeof size, n->size);
			if (unix_style)
				kprintf("-%s %13s  %s %s  %s\n",
				    (n->attr & ATTR_RO) ? "r-" : "rw", size,
				    build_date, build_time, n->name);
			else
				kprintf("%-24s %13s  %s  %s\n", n->name, size,
				    build_date, build_time);
		}
	}
	fmt_commas(size, sizeof size, bytes);
	kprintf("%16d file(s) %16s bytes\n", files, size);
	fmt_commas(size, sizeof size, mem_free());
	kprintf("%16d dir(s)  %16s bytes free\n\n", dirs, size);
}

static void list_wide(struct node *dir, int show_hidden, int unix_style)
{
	int colw = 20, percol = con_cols() / colw, col = 0;
	if (percol < 1)
		percol = 1;
	if (!unix_style) {
		char path[256];
		rd_path(dir, path, sizeof path, 0);
		kprintf("\n Volume in drive C is BDOS\n Directory of %s\n\n", path);
	}
	for (struct node *n = dir->child; n; n = n->next) {
		if ((n->attr & ATTR_HIDDEN) && !show_hidden)
			continue;
		char name[80];
		if (n->is_dir)
			snprintf(name, sizeof name, unix_style ? "%s/" : "[%s]", n->name);
		else
			snprintf(name, sizeof name, "%s", n->name);
		int len = (int)strlen(name);
		int span = (len + colw) / colw;	/* long names take extra columns */
		if (col && col + span > percol) {
			kprintf("\n");
			col = 0;
		}
		kprintf("%s", name);
		for (int i = len; i < span * colw; i++)
			kprintf(" ");
		col += span;
	}
	kprintf("\n");
	if (!unix_style)
		kprintf("\n");
}

static void cmd_dir(struct ctx *c)
{
	const char *arg = first_arg(c);
	struct node *n = arg ? rd_lookup(cwd, arg, c->unix_style) : cwd;
	if (!n) {
		kprintf(c->unix_style ? "ls: %s: No such file or directory\n" :
		    "File not found\n", arg);
		return;
	}
	int hidden = has_flag(c, 'A');
	int wide = c->unix_style ? !has_flag(c, 'L') : has_flag(c, 'W');

	if (!n->is_dir) {	/* listing a single file */
		char size[32];
		fmt_commas(size, sizeof size, n->size);
		kprintf("%-24s %13s  %s  %s\n", n->name, size, build_date, build_time);
		return;
	}
	if (wide)
		list_wide(n, hidden, c->unix_style);
	else
		list_long(n, hidden, c->unix_style);
}

static void cmd_cd(struct ctx *c)
{
	const char *arg = first_arg(c);
	char path[256];
	if (!arg) {
		rd_path(cwd, path, sizeof path, c->unix_style);
		kprintf("%s\n", path);
		return;
	}
	struct node *n = rd_lookup(cwd, arg, c->unix_style);
	if (!n || !n->is_dir) {
		kprintf(c->unix_style ? "cd: %s: No such directory\n" :
		    "Invalid directory\n", arg);
		return;
	}
	cwd = n;
}

static void cmd_type(struct ctx *c)
{
	const char *arg = first_arg(c);
	if (!arg) {
		kprintf("Required parameter missing\n");
		return;
	}
	struct node *n = rd_lookup(cwd, arg, c->unix_style);
	if (!n) {
		kprintf(c->unix_style ? "cat: %s: No such file\n" :
		    "File not found - %s\n", arg);
		return;
	}
	if (n->is_dir) {
		kprintf(c->unix_style ? "cat: %s: Is a directory\n" :
		    "Access denied - %s\n", arg);
		return;
	}
	con_write(n->data);
	if (n->size && n->data[n->size - 1] != '\n')
		kprintf("\n");
}

static void cmd_attrib(struct ctx *c)
{
	const char *arg = first_arg(c);
	struct node *dir = cwd, *only = NULL;
	if (arg) {
		only = rd_lookup(cwd, arg, c->unix_style);
		if (!only) {
			kprintf("File not found - %s\n", arg);
			return;
		}
	}
	for (struct node *n = only ? only : dir->child; n; n = only ? NULL : n->next) {
		char path[256];
		rd_path(n, path, sizeof path, 0);
		kprintf("%c  %c%c%c  %s\n",
		    (n->attr & ATTR_ARCHIVE) ? 'A' : ' ',
		    (n->attr & ATTR_SYSTEM) ? 'S' : ' ',
		    (n->attr & ATTR_HIDDEN) ? 'H' : ' ',
		    (n->attr & ATTR_RO) ? 'R' : ' ', path);
	}
}

static void cmd_mem(struct ctx *c)
{
	(void)c;
	mem_report();
}

static void cmd_date(struct ctx *c)
{
	(void)c;
	int y, mo, d, h, mi, s;
	read_rtc(&y, &mo, &d, &h, &mi, &s);
	kprintf("Current date is %04d-%02d-%02d\n", y, mo, d);
}

static void cmd_time(struct ctx *c)
{
	(void)c;
	int y, mo, d, h, mi, s;
	read_rtc(&y, &mo, &d, &h, &mi, &s);
	kprintf("Current time is %02d:%02d:%02d\n", h, mi, s);
}

static void cmd_win(struct ctx *c)
{
	(void)c;
	kprintf("The bdos desktop isn't built yet. Soon: Program Manager, nested\n"
	    "sessions and WIN \\\\HOST for remote desktops.\n");
}

static void cmd_reboot(struct ctx *c)
{
	(void)c;
	kprintf("Restarting...\n");
	reboot();
}

static const struct command commands[] = {
	{ "ALIAS",  cmd_alias,  "List the Unix-style command aliases" },
	{ "ATTRIB", cmd_attrib, "Show file attributes" },
	{ "CD",     cmd_cd,     "Show or change the current directory" },
	{ "CHDIR",  cmd_cd,     NULL },
	{ "CLS",    cmd_cls,    "Clear the screen" },
	{ "DATE",   cmd_date,   "Show the date" },
	{ "DIR",    cmd_dir,    "List files (/W wide, /A include hidden)" },
	{ "ECHO",   cmd_echo,   "Display text, or turn command echo ON/OFF" },
	{ "HELP",   cmd_help,   "This list" },
	{ "MEM",    cmd_mem,    "Show memory usage" },
	{ "REBOOT", cmd_reboot, "Restart the computer" },
	{ "REM",    cmd_rem,    "Comment (in batch files)" },
	{ "TIME",   cmd_time,   "Show the time" },
	{ "TYPE",   cmd_type,   "Display a text file" },
	{ "VER",    cmd_ver,    "Show the bdos version" },
	{ "WIN",    cmd_win,    "Start the desktop" },
};

static const struct alias aliases[] = {
	{ "ls",    "DIR",  1 },
	{ "cat",   "TYPE", 1 },
	{ "clear", "CLS",  1 },
	{ "pwd",   "CD",   1 },
	{ "free",  "MEM",  1 },
	{ "uname", "VER",  1 },
	{ "man",   "HELP", 1 },
	{ "reboot", "REBOOT", 1 },
};

#define NELEM(a) (sizeof(a) / sizeof((a)[0]))

static void cmd_help(struct ctx *c)
{
	(void)c;
	kprintf("\n");
	for (size_t i = 0; i < NELEM(commands); i++)
		if (commands[i].help)
			kprintf("  %-8s %s\n", commands[i].name, commands[i].help);
	kprintf("\nRun a .BAT file by typing its name. Unix names work too: "
	    "ls, cat, pwd...\nType ALIAS to see them all.\n\n");
}

static void cmd_alias(struct ctx *c)
{
	(void)c;
	for (size_t i = 0; i < NELEM(aliases); i++)
		kprintf("  %-8s = %s\n", aliases[i].name, aliases[i].target);
}

/* ---- running things ----------------------------------------------------- */

static const struct command *find_command(const char *name)
{
	for (size_t i = 0; i < NELEM(commands); i++)
		if (!strcasecmp(commands[i].name, name))
			return &commands[i];
	return NULL;
}

static void print_prompt(void)
{
	char path[256];
	rd_path(cwd, path, sizeof path, 0);
	kprintf("%s>", path);
}

static void run_batch(struct node *f)
{
	char line[LINE_MAX];
	const char *p = f->data, *end = f->data + f->size;
	while (p < end) {
		size_t n = 0;
		while (p < end && *p != '\n') {
			if (n + 1 < sizeof line && *p != '\r')
				line[n++] = *p;
			p++;
		}
		p++;
		line[n] = 0;
		run_line(line, 1);
	}
}

/* Try NAME, NAME.BAT, NAME.BAS, NAME.C in the current directory. */
static int run_program(const char *name)
{
	static const char *const exts[] = { "", ".BAT", ".BAS", ".C" };
	for (size_t i = 0; i < NELEM(exts); i++) {
		char full[96];
		snprintf(full, sizeof full, "%s%s", name, exts[i]);
		struct node *f = rd_lookup(cwd, full, 0);
		if (!f || f->is_dir)
			continue;
		size_t len = strlen(f->name);
		if (len > 4 && !strcasecmp(f->name + len - 4, ".BAT")) {
			run_batch(f);
			return 1;
		}
		if (len > 4 && !strcasecmp(f->name + len - 4, ".BAS")) {
			kprintf("BASIC isn't built in yet - here's the program instead:\n\n");
			con_write(f->data);
			return 1;
		}
		if (len > 2 && !strcasecmp(f->name + len - 2, ".C")) {
			kprintf("The built-in C compiler isn't here yet - "
			    "here's the source instead:\n\n");
			con_write(f->data);
			return 1;
		}
	}
	return 0;
}

/*
 * Split a command line. The command word also ends at '/' or '\' so that
 * DOS habits like "dir/w" and "cd\" work. Double quotes group words
 * ("Read Me First.txt").
 */
static int tokenize(char *line, char **argv)
{
	int argc = 0;
	char *p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!*p)
		return 0;

	/* command word */
	argv[argc++] = p;
	/* ...and at "..", plus a trailing "." for the ECHO. idiom */
	while (*p && *p != ' ' && *p != '\t' && *p != '/' && *p != '\\' &&
	    !(p[0] == '.' && (p[1] == '.' || p[1] == 0 || p[1] == ' ')))
		p++;
	if (*p == '/' || *p == '\\' || *p == '.') {
		/* Copy the command word out so the rest stays intact. */
		static char cmdbuf[32];
		size_t n = (size_t)(p - argv[0]);
		if (n >= sizeof cmdbuf)
			n = sizeof cmdbuf - 1;
		memcpy(cmdbuf, argv[0], n);
		cmdbuf[n] = 0;
		argv[0] = cmdbuf;
	} else if (*p) {
		*p++ = 0;
	}

	while (*p && argc < MAX_ARGS) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '"') {
			argv[argc++] = ++p;
			while (*p && *p != '"')
				p++;
		} else {
			argv[argc++] = p;
			while (*p && *p != ' ' && *p != '\t')
				p++;
		}
		if (*p)
			*p++ = 0;
	}
	return argc;
}

static int run_line(char *line, int from_batch)
{
	int quiet = 0;
	while (*line == ' ')
		line++;
	if (*line == '@') {
		quiet = 1;
		line++;
	}
	if (from_batch && echo_on && !quiet && *line) {
		print_prompt();
		kprintf("%s\n", line);
	}

	struct ctx c = { 0 };
	c.argc = tokenize(line, c.argv);
	if (!c.argc)
		return 0;
	const char *name = c.argv[0];

	/* Drive letters: C: is the only one so far. */
	if (strlen(name) == 2 && name[1] == ':') {
		if (toupper(name[0]) != 'C')
			kprintf("Invalid drive specification\n");
		return 1;
	}

	const struct command *cmd = find_command(name);
	if (!cmd) {
		for (size_t i = 0; i < NELEM(aliases); i++) {
			if (!strcmp(aliases[i].name, name)) {	/* aliases are case-sensitive, like Unix */
				cmd = find_command(aliases[i].target);
				c.unix_style = aliases[i].unix_style;
				break;
			}
		}
	}
	if (cmd) {
		cmd->fn(&c);
		return 1;
	}
	if (run_program(name))
		return 1;
	kprintf("Bad command or file name\n");
	return 0;
}

/* ---- line editor -------------------------------------------------------- */

static char history[HIST_MAX][LINE_MAX];
static int hist_len;

static void erase_line(size_t n)
{
	while (n--)
		con_write("\b \b");
}

static void read_line(char *buf, size_t cap)
{
	size_t len = 0;
	int hpos = hist_len;
	buf[0] = 0;

	con_cursor(1);
	for (;;) {
		int k = getkey();
		if (k == '\n') {
			con_putc('\n');
			break;
		}
		if (k == '\b') {
			if (len) {
				len--;
				con_write("\b \b");
			}
		} else if (k == 3) {			/* ^C: abandon the line */
			con_write("^C\n");
			len = 0;
			break;
		} else if (k == 27) {			/* Esc clears, like DOS */
			erase_line(len);
			len = 0;
		} else if (k == KEY_UP || k == KEY_DOWN) {
			int np = hpos + (k == KEY_UP ? -1 : 1);
			if (np < 0 || np > hist_len)
				continue;
			hpos = np;
			erase_line(len);
			if (hpos == hist_len)
				buf[0] = 0;
			else
				strncpy0(buf, history[hpos], cap);
			len = strlen(buf);
			con_write(buf);
		} else if (k >= 0x20 && k < 0x7F && len + 1 < cap) {
			buf[len++] = (char)k;
			buf[len] = 0;
			con_putc((char)k);
		}
		buf[len] = 0;
	}
	buf[len] = 0;
	con_cursor(0);

	if (len && (!hist_len || strcmp(history[hist_len - 1], buf))) {
		if (hist_len == HIST_MAX) {
			memmove(history[0], history[1], sizeof history[0] * (HIST_MAX - 1));
			hist_len--;
		}
		strncpy0(history[hist_len++], buf, LINE_MAX);
	}
}

void shell_run(const struct bdos_bootinfo *bi)
{
	(void)bi;
	cwd = rd_root();
	set_build_date();

	struct node *ae = rd_lookup(cwd, "AUTOEXEC.BAT", 0);
	if (ae)
		run_batch(ae);
	echo_on = 1;

	char line[LINE_MAX];
	for (;;) {
		print_prompt();
		read_line(line, sizeof line);
		run_line(line, 0);
	}
}
