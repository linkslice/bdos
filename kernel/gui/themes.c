/*
 * themes.c - desktop themes.
 *
 * A theme is a file in C:\BDOS\THEMES, NAME.THM, of key=value lines: the
 * window colours, the desktop colour, an optional 8x8 desktop pattern, and
 * whether the desktop follows the time of day. The desktop.theme setting
 * names the one in use. The defaults are written out the first time, so
 * they can be copied, edited, or deleted like any other file.
 */
#include "wm_int.h"
#include "themes.h"
#include "../settings.h"

static const struct theme_file builtin[] = {
	{ "bdos Classic", 0xC3C3C3, 0x1D3A8A, 0x7B7B7B, 0xFFFFFF, 0x000000, 0x1D3A8A, 0xFFFFFF,
	  0x2F6F73, 0, { 0 }, 0, 0 },
	{ "Ocean", 0xB8CCD4, 0x0F5F7F, 0x6F8C98, 0xFFFFFF, 0x000000, 0x0F5F7F, 0xF0F8FA,
	  0x1F4F6F, 0x2A6283, { 0x00, 0x00, 0x18, 0x24, 0x42, 0x81, 0x00, 0x00 }, 1, 0 },
	{ "Plum", 0xCCC2D0, 0x5B2F72, 0x857790, 0xFFFFFF, 0x000000, 0x5B2F72, 0xF6F2F8,
	  0x5B3F72, 0, { 0 }, 0, 0 },
	{ "Graphite", 0xB4B4B4, 0x303030, 0x7A7A7A, 0xFFFFFF, 0x000000, 0x505050, 0xEEEEEE,
	  0x404850, 0x4A525A, { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 }, 1, 0 },
	{ "Rose", 0xD8C4C4, 0x8A2F3F, 0x9A7A80, 0xFFFFFF, 0x000000, 0x8A2F3F, 0xFBF4F4,
	  0x7A3A4A, 0, { 0 }, 0, 0 },
	{ "Desert", 0xD8C8A8, 0x8A5A2F, 0xA89878, 0xFFFFFF, 0x000000, 0x8A5A2F, 0xFAF4E8,
	  0xB07840, 0xA06A34, { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 }, 1, 0 },
	{ "Plan 9", 0xEAFFFF, 0x55AAAA, 0x9EEEEE, 0x000000, 0x000000, 0x448888, 0xFFFFEA,
	  0x777777, 0, { 0 }, 0, 0 },
	{ "Sunrise to Sunset", 0xC3C3C3, 0x1D3A8A, 0x7B7B7B, 0xFFFFFF, 0x000000, 0x1D3A8A, 0xFFFFFF,
	  0x2F6F73, 0, { 0 }, 0, 1 },
	/* Windows 3.1's most notorious scheme, faithfully awful */
	{ "Hot Dog Stand", 0xFFFF00, 0xFF0000, 0x000000, 0xFFFF00, 0x000000, 0x000000, 0xFFFF00,
	  0xFF0000, 0xC00000, { 0x00, 0x66, 0x66, 0x00, 0x00, 0x66, 0x66, 0x00 }, 1, 0 },
};

int theme_is_builtin(const char *name)
{
	for (size_t i = 0; i < NELEM(builtin); i++)
		if (!strcasecmp(builtin[i].name, name))
			return 1;
	return 0;
}

/* ---- the time of day --------------------------------------------------------------- */

static const struct { int min; uint32_t rgb; } sky[] = {
	{ 0, 0x0B1030 }, { 300, 0x141A40 }, { 345, 0x3A2A5A }, { 390, 0xC0603A }, { 420, 0xD89050 },
	{ 480, 0x4A86C5 }, { 720, 0x3A8FD0 }, { 1020, 0x4A86C5 }, { 1080, 0xD08A3A }, { 1125, 0xB0405A },
	{ 1170, 0x4A2A5A }, { 1230, 0x1A1A40 }, { 1440, 0x0B1030 },
};

static uint32_t mix(uint32_t a, uint32_t b, int num, int den)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		int ca = (int)((a >> sh) & 255), cb = (int)((b >> sh) & 255);
		out |= (uint32_t)(ca + (cb - ca) * num / den) << sh;
	}
	return out;
}

uint32_t time_of_day_colour(int minutes)
{
	minutes = ((minutes % 1440) + 1440) % 1440;
	for (size_t i = 0; i + 1 < NELEM(sky); i++)
		if (minutes >= sky[i].min && minutes < sky[i + 1].min)
			return mix(sky[i].rgb, sky[i + 1].rgb, minutes - sky[i].min, sky[i + 1].min - sky[i].min);
	return sky[0].rgb;
}

/* ---- files -------------------------------------------------------------------------------- */

static void file_name(const char *name, char *out, size_t n)
{
	char fn[64];
	snprintf(fn, sizeof fn, "%s.THM", name);
	vfs_join(THEMES_DIR, fn, out, n);
}

int theme_save(const struct theme_file *t)
{
	char path[VFS_PATH_MAX], pat[20], buf[640];
	file_name(t->name, path, sizeof path);
	for (int i = 0; i < 8; i++)
		snprintf(pat + i * 2, 3, "%02x", t->pattern[i]);
	int n = snprintf(buf, sizeof buf,
	    "; bdos theme - edit me, or make one with Themes in the Control Panel\n"
	    "name=%s\nface=%06x\nactive=%06x\ninactive=%06x\ntitle_text=%06x\nmenu_text=%06x\n"
	    "select=%06x\nworkspace=%06x\ndesktop=%06x\npattern=%s\npattern_ink=%06x\ntimeofday=%d\n",
	    t->name, t->face, t->active, t->inactive, t->title_text, t->menu_text, t->select, t->workspace,
	    t->desktop, t->has_pattern ? pat : "none", t->pattern_ink, t->timeofday);
	return vfs_spew(path, buf, (size_t)n);
}

static uint32_t hex(const char *v)
{
	while (*v == '#' || *v == ' ')
		v++;
	return (uint32_t)strtol(v, NULL, 16) & 0xFFFFFF;
}

int theme_load(const char *name, struct theme_file *t)
{
	char path[VFS_PATH_MAX];
	file_name(name, path, sizeof path);
	size_t len;
	int err;
	char *src = vfs_slurp(path, &len, &err);
	if (!src) {
		for (size_t i = 0; i < NELEM(builtin); i++)
			if (!strcasecmp(builtin[i].name, name)) {
				*t = builtin[i];
				return 0;
			}
		return err;
	}
	*t = builtin[0];
	strncpy0(t->name, name, sizeof t->name);
	for (char *p = src; *p; ) {
		char *e = strchr(p, '\n');
		if (e)
			*e = 0;
		char *eq = strchr(p, '=');
		if (*p != ';' && eq) {
			*eq = 0;
			char *k = p, *v = eq + 1;
			size_t vl = strlen(v);
			while (vl && (v[vl - 1] == '\r' || v[vl - 1] == ' '))
				v[--vl] = 0;
			if (!strcasecmp(k, "face")) t->face = hex(v);
			else if (!strcasecmp(k, "active")) t->active = hex(v);
			else if (!strcasecmp(k, "inactive")) t->inactive = hex(v);
			else if (!strcasecmp(k, "title_text")) t->title_text = hex(v);
			else if (!strcasecmp(k, "menu_text")) t->menu_text = hex(v);
			else if (!strcasecmp(k, "select")) t->select = hex(v);
			else if (!strcasecmp(k, "workspace")) t->workspace = hex(v);
			else if (!strcasecmp(k, "desktop")) t->desktop = hex(v);
			else if (!strcasecmp(k, "pattern_ink")) t->pattern_ink = hex(v);
			else if (!strcasecmp(k, "timeofday")) t->timeofday = strtol(v, NULL, 10) != 0;
			else if (!strcasecmp(k, "pattern")) {
				t->has_pattern = strlen(v) >= 16;
				for (int i = 0; i < 8 && t->has_pattern; i++) {
					char b[3] = { v[i * 2], v[i * 2 + 1], 0 };
					t->pattern[i] = (uint8_t)strtol(b, NULL, 16);
				}
			}
		}
		if (!e)
			break;
		p = e + 1;
	}
	kfree(src);
	return 0;
}

struct tl {
	char (*names)[48];
	int n, max;
};

static int list_cb(void *arg, struct vstat *st)
{
	struct tl *l = arg;
	const char *dot = strrchr(st->name, '.');
	if (st->type != VFS_FILE || !dot || strcasecmp(dot, ".THM") || l->n >= l->max)
		return 0;
	size_t n = MIN((size_t)(dot - st->name), (size_t)47);
	memcpy(l->names[l->n], st->name, n);
	l->names[l->n][n] = 0;
	l->n++;
	return 0;
}

int themes_list(char names[][48], int max)
{
	struct tl l = { names, 0, max };
	if (vfs_list(THEMES_DIR, list_cb, &l))
		for (size_t i = 0; i < NELEM(builtin) && l.n < max; i++)
			strncpy0(names[l.n++], builtin[i].name, 48);
	for (int i = 1; i < l.n; i++) {
		char k[48];
		strncpy0(k, names[i], sizeof k);
		int j = i - 1;
		while (j >= 0 && strcasecmp(names[j], k) > 0) {
			strncpy0(names[j + 1], names[j], 48);
			j--;
		}
		strncpy0(names[j + 1], k, 48);
	}
	return l.n;
}

void theme_current(struct theme_file *t)
{
	memset(t, 0, sizeof *t);
	strncpy0(t->name, setting("desktop.theme"), sizeof t->name);
	t->face = theme.face;
	t->active = theme.active;
	t->inactive = theme.inactive;
	t->title_text = theme.title_text;
	t->menu_text = theme.menu_text;
	t->select = theme.select;
	t->workspace = theme.workspace;
	t->desktop = setting_rgb("desktop.color");
	t->pattern_ink = theme.pattern_ink;
	memcpy(t->pattern, theme.pattern, 8);
	t->has_pattern = theme.has_pattern;
	t->timeofday = setting_int("desktop.timeofday") != 0;
}

/* ---- applying --------------------------------------------------------------------------- */

static void use(const struct theme_file *t)
{
	theme.face = t->face;
	theme.active = t->active;
	theme.inactive = t->inactive;
	theme.title_text = t->title_text;
	theme.menu_text = t->menu_text;
	theme.select = t->select;
	theme.workspace = t->workspace;
	theme.pattern_ink = t->pattern_ink;
	memcpy(theme.pattern, t->pattern, 8);
	theme.has_pattern = t->has_pattern;
}

static void watch(const char *key)
{
	if (*key && strcmp(key, "desktop.theme"))
		return;
	struct theme_file t;
	if (theme_load(setting("desktop.theme"), &t))
		t = builtin[0];
	use(&t);
	if (*key) {
		/* chosen just now: the theme's desktop comes with it */
		char hexs[8];
		snprintf(hexs, sizeof hexs, "%06x", t.desktop);
		setting_set("desktop.color", hexs);
		setting_set_int("desktop.timeofday", t.timeofday);
	}
	repaint_all();
}

void themes_init(void)
{
	struct vstat st;
	if (vfs_stat(THEMES_DIR, &st)) {
		vfs_mkdir("C:\\BDOS");
		if (!vfs_mkdir(THEMES_DIR))
			for (size_t i = 0; i < NELEM(builtin); i++)
				theme_save(&builtin[i]);
	}
	setting_watch(watch);
}
