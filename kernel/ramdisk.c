/*
 * ramdisk.c - a read-only, built-in C: drive until the real filesystem
 * lands. Names are long and case-preserving; lookups ignore case, the
 * way bdos's own filesystem will.
 */
#include "kernel.h"

#define MAX_NODES 64

static struct node nodes[MAX_NODES];
static int nnodes;
static struct node *root;

static struct node *add(struct node *dir, const char *name, int is_dir,
    const char *data, uint8_t attr)
{
	if (nnodes >= MAX_NODES)
		panic("ramdisk full");
	struct node *n = &nodes[nnodes++];
	strncpy0(n->name, name, sizeof n->name);
	n->is_dir = is_dir;
	n->attr = attr;
	n->data = data;
	n->size = data ? strlen(data) : 0;
	n->parent = dir ? dir : n;

	if (dir) {	/* keep siblings sorted: directories first, then by name */
		struct node **pp = &dir->child;
		while (*pp && (((*pp)->is_dir > is_dir) ||
		    ((*pp)->is_dir == is_dir && strcasecmp((*pp)->name, name) < 0)))
			pp = &(*pp)->next;
		n->next = *pp;
		*pp = n;
	}
	return n;
}

static const char autoexec[] =
	"@ECHO OFF\n"
	"REM AUTOEXEC.BAT runs every time bdos starts.\n"
	"ECHO Welcome to bdos. Type HELP for a list of commands.\n"
	"ECHO.\n"
	"REM Uncomment the next line to start the desktop automatically:\n"
	"REM WIN\n";

static const char readme[] =
	"bdos - Bryan Disk Operating System\n"
	"==================================\n"
	"\n"
	"A 64-bit, single-user operating system that should feel like home to\n"
	"anyone who grew up at a C:\\> prompt.\n"
	"\n"
	"This drive is a small read-only RAM disk built into the kernel. The\n"
	"real filesystem (long names, journaling, extended attributes) is next.\n"
	"\n"
	"Both DOS and Unix habits work:\n"
	"\n"
	"    DIR /W     is the same as   ls\n"
	"    DIR        is the same as   ls -l\n"
	"    TYPE FILE  is the same as   cat file\n"
	"    CD \\GAMES  is the same as   cd /games\n"
	"\n"
	"Under DIR, '/' starts a switch and '\\' separates paths.\n"
	"Under ls, '-' starts a switch and '/' separates paths.\n";

static const char hello_bas[] =
	"10 REM The first bdos program. BASIC support is coming!\n"
	"20 PRINT \"Hello from bdos\"\n"
	"30 SYS \"Desk_Color\", &204060\n"
	"40 END\n";

static const char hello_c[] =
	"#include <desk.h>\n"
	"\n"
	"/* Run me with HELLO.C once the built-in C compiler exists. */\n"
	"int main(void)\n"
	"{\n"
	"\tdesk_color(0x204060);\n"
	"\tWindow w = win_open(\"Hello\", 40, 40, 220, 90);\n"
	"\twin_text(w, 10, 10, \"Hi from C:\\\\HELLO.C\");\n"
	"\treturn win_wait_close(w);\n"
	"}\n";

static const char lander[] =
	"10 REM Lunar Lander - placeholder until BASIC runs\n"
	"20 PRINT \"Fuel: 120   Altitude: 1000\"\n";

static const char roadmap[] =
	"bdos roadmap\n"
	"------------\n"
	"[x] UEFI loader (no third-party bootloader)\n"
	"[x] Framebuffer console, PS/2 + serial input\n"
	"[x] Shell with DOS commands and Unix aliases\n"
	"[ ] Journaled filesystem with long names and 8.3 aliases\n"
	"[ ] BBC-style BASIC and run-from-source C\n"
	"[ ] Device-based GUI: CON, MOUSE$, DRAW$, DESK$\n"
	"[ ] WIN \\\\HOST remote sessions with T: drive\n"
	"[ ] 8086 emulator for classic .COM programs\n"
	"[ ] Lemote Yeeloong (MIPS64) port\n";

void rd_init(void)
{
	root = add(NULL, "", 1, NULL, 0);
	add(root, "AUTOEXEC.BAT", 0, autoexec, ATTR_ARCHIVE);
	add(root, "Read Me First.txt", 0, readme, ATTR_RO);
	add(root, "HELLO.BAS", 0, hello_bas, ATTR_ARCHIVE);
	add(root, "HELLO.C", 0, hello_c, ATTR_ARCHIVE);

	struct node *docs = add(root, "Docs", 1, NULL, 0);
	add(docs, "Roadmap.txt", 0, roadmap, ATTR_ARCHIVE);

	struct node *games = add(root, "GAMES", 1, NULL, 0);
	add(games, "Lunar Lander.bas", 0, lander, ATTR_ARCHIVE);

	struct node *sys = add(root, "SYSTEM", 1, NULL, ATTR_SYSTEM);
	add(sys, "KERNEL.SYS", 0, "(the real one lives on the boot partition)\n",
	    ATTR_SYSTEM | ATTR_HIDDEN | ATTR_RO);
}

struct node *rd_root(void) { return root; }

static int is_sep(char c, int unix_style)
{
	return unix_style ? c == '/' : (c == '\\' || c == '/');
}

/*
 * Resolve a path relative to cwd. Accepts "C:" prefixes, "." and "..".
 * DOS style splits on '\' (and '/' inside a path, as DOS itself allowed);
 * Unix style splits on '/' only.
 */
struct node *rd_lookup(struct node *cwd, const char *path, int unix_style)
{
	struct node *n = cwd;

	if ((path[0] == 'c' || path[0] == 'C') && path[1] == ':') {
		path += 2;
		if (!*path)
			return cwd;
	}
	if (is_sep(*path, unix_style)) {
		n = root;
		while (is_sep(*path, unix_style))
			path++;
	}

	while (*path) {
		char part[64];
		size_t len = 0;
		while (*path && !is_sep(*path, unix_style)) {
			if (len + 1 < sizeof part)
				part[len++] = *path;
			path++;
		}
		part[len] = 0;
		while (is_sep(*path, unix_style))
			path++;

		if (!strcmp(part, ".") || !len)
			continue;
		if (!strcmp(part, "..")) {
			n = n->parent;
			continue;
		}
		if (!n->is_dir)
			return NULL;
		struct node *c;
		for (c = n->child; c; c = c->next)
			if (!strcasecmp(c->name, part))
				break;
		if (!c)
			return NULL;
		n = c;
	}
	return n;
}

void rd_path(struct node *n, char *buf, size_t len, int unix_style)
{
	const char *parts[32];
	int depth = 0;
	char sep = unix_style ? '/' : '\\';

	for (; n != root && depth < 32; n = n->parent)
		parts[depth++] = n->name;

	size_t o = 0;
	if (!unix_style && o + 2 < len) {
		buf[o++] = 'C';
		buf[o++] = ':';
	}
	if (o + 1 < len)
		buf[o++] = sep;
	while (depth--) {
		for (const char *p = parts[depth]; *p && o + 1 < len; p++)
			buf[o++] = *p;
		if (depth && o + 1 < len)
			buf[o++] = sep;
	}
	buf[o] = 0;
}
