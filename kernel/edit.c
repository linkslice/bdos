/*
 * edit.c - EDIT, a full-screen text editor in the MS-DOS EDIT tradition.
 *
 *   arrows/Home/End/PgUp/PgDn  move        F2 or ^S  save
 *   Enter, Backspace, Del      edit        F5        save and run
 *   Esc or ^Q                  quit        F1        help
 *
 * Works on any console that can position the cursor: the screen, a
 * desktop window, or COM1 (ANSI).
 */
#include "kernel.h"
#include "vfs.h"

#define TAB 4

struct editor {
	char path[VFS_PATH_MAX];
	char **lines;
	int *len, *cap;
	int nlines, maxlines;
	int cx, cy;		/* cursor in the text */
	int top, left;		/* scroll position */
	int cols, rows;		/* console size */
	int dirty;
	char msg[80];
};

static int ensure_lines(struct editor *e, int n)
{
	if (n <= e->maxlines)
		return 0;
	int nm = MAX(n, e->maxlines * 2 + 16);
	char **nl = krealloc(e->lines, sizeof(char *) * (size_t)nm);
	if (!nl)
		return -1;
	e->lines = nl;
	int *len = krealloc(e->len, sizeof(int) * (size_t)nm);
	if (!len)
		return -1;
	e->len = len;
	int *cap = krealloc(e->cap, sizeof(int) * (size_t)nm);
	if (!cap)
		return -1;
	e->cap = cap;
	e->maxlines = nm;
	return 0;
}

static int line_reserve(struct editor *e, int y, int need)
{
	if (need + 1 <= e->cap[y])
		return 0;
	int nc = MAX(need + 1, e->cap[y] * 2 + 16);
	char *nb = krealloc(e->lines[y], (size_t)nc);
	if (!nb)
		return -1;
	e->lines[y] = nb;
	e->cap[y] = nc;
	return 0;
}

static int insert_line(struct editor *e, int at, const char *text, int n)
{
	if (ensure_lines(e, e->nlines + 1))
		return -1;
	memmove(&e->lines[at + 1], &e->lines[at], sizeof(char *) * (size_t)(e->nlines - at));
	memmove(&e->len[at + 1], &e->len[at], sizeof(int) * (size_t)(e->nlines - at));
	memmove(&e->cap[at + 1], &e->cap[at], sizeof(int) * (size_t)(e->nlines - at));
	e->nlines++;
	e->lines[at] = NULL;
	e->cap[at] = 0;
	e->len[at] = 0;
	if (line_reserve(e, at, n))
		return -1;
	memcpy(e->lines[at], text, (size_t)n);
	e->lines[at][n] = 0;
	e->len[at] = n;
	return 0;
}

static void delete_line(struct editor *e, int at)
{
	kfree(e->lines[at]);
	memmove(&e->lines[at], &e->lines[at + 1], sizeof(char *) * (size_t)(e->nlines - at - 1));
	memmove(&e->len[at], &e->len[at + 1], sizeof(int) * (size_t)(e->nlines - at - 1));
	memmove(&e->cap[at], &e->cap[at + 1], sizeof(int) * (size_t)(e->nlines - at - 1));
	e->nlines--;
}

static int load(struct editor *e)
{
	int err;
	size_t n;
	char *text = vfs_slurp(e->path, &n, &err);
	if (!text) {
		if (err != E_NOENT)
			return err;
		insert_line(e, 0, "", 0);	/* new file */
		snprintf(e->msg, sizeof e->msg, "New file");
		return 0;
	}
	char buf[1024];
	int bl = 0;
	for (size_t i = 0; i <= n; i++) {
		char ch = i < n ? text[i] : '\n';
		if (ch == '\r' || ch == KEY_EOF)
			continue;
		if (ch == '\n') {
			if (i < n || bl || !e->nlines)
				insert_line(e, e->nlines, buf, bl);
			bl = 0;
		} else if (ch == '\t') {
			do
				if (bl < (int)sizeof buf - 1)
					buf[bl++] = ' ';
			while (bl % 8);
		} else if (bl < (int)sizeof buf - 1) {
			buf[bl++] = ch;
		}
	}
	kfree(text);
	if (!e->nlines)
		insert_line(e, 0, "", 0);
	return 0;
}

static int save(struct editor *e)
{
	size_t total = 0;
	for (int i = 0; i < e->nlines; i++)
		total += (size_t)e->len[i] + 1;
	char *buf = kmalloc(total + 1);
	if (!buf)
		return E_NOMEM;
	size_t o = 0;
	for (int i = 0; i < e->nlines; i++) {
		memcpy(buf + o, e->lines[i], (size_t)e->len[i]);
		o += (size_t)e->len[i];
		buf[o++] = '\n';
	}
	/* no trailing blank line from an empty last line */
	if (e->nlines > 1 && e->len[e->nlines - 1] == 0)
		o--;
	int r = vfs_spew(e->path, buf, o);
	kfree(buf);
	if (!r) {
		e->dirty = 0;
		snprintf(e->msg, sizeof e->msg, "Saved %d lines", e->nlines);
	} else {
		snprintf(e->msg, sizeof e->msg, "Save failed: %s", vfs_strerror(r));
	}
	return r;
}

/* ---- drawing ---- */

#define ATTR_TEXT	0x1F	/* white on blue */
#define ATTR_BAR	0x70	/* black on grey */
#define ATTR_MSG	0x71

static void put_attr(int a)
{
	con_attr(a & 0xF, (a >> 4) & 0xF);
}

static void draw_row(struct editor *e, int sy)
{
	int y = e->top + sy - 1;
	con_gotoxy(0, sy);
	put_attr(ATTR_TEXT);
	char row[260];
	int w = MIN(e->cols, (int)sizeof row - 1);
	for (int x = 0; x < w; x++) {
		int tx = e->left + x;
		row[x] = (y < e->nlines && tx < e->len[y]) ? e->lines[y][tx] : ' ';
	}
	if (y >= e->nlines && w > 0)
		row[0] = y == e->nlines ? ' ' : ' ';
	/* avoid the last cell of the last row to dodge scrolling */
	if (sy == e->rows - 2)
		w--;
	row[w] = 0;
	con_write(row);
}

static void draw_bars(struct editor *e)
{
	char bar[260];
	int w = MIN(e->cols, (int)sizeof bar - 1);
	con_gotoxy(0, 0);
	put_attr(ATTR_BAR);
	snprintf(bar, sizeof bar, "  bdos EDIT   %s%s", e->path, e->dirty ? "  [modified]" : "");
	int l = (int)strlen(bar);
	for (int i = l; i < w; i++)
		bar[i] = ' ';
	bar[w] = 0;
	con_write(bar);

	con_gotoxy(0, e->rows - 1);
	put_attr(e->msg[0] ? ATTR_MSG : ATTR_BAR);
	char right[32];
	snprintf(right, sizeof right, "Line:%-5d Col:%-4d", e->cy + 1, e->cx + 1);
	if (e->msg[0])
		snprintf(bar, sizeof bar, " %s", e->msg);
	else
		snprintf(bar, sizeof bar, " F1=Help  F2=Save  F5=Run  Esc=Quit");
	l = (int)strlen(bar);
	int rl = (int)strlen(right);
	for (int i = l; i < w; i++)
		bar[i] = ' ';
	if (w - rl - 1 > l)
		memcpy(bar + w - rl - 1, right, (size_t)rl);
	bar[w - 1] = 0;		/* never write the very last cell */
	con_write(bar);
}

static void draw_all(struct editor *e)
{
	for (int sy = 1; sy < e->rows - 1; sy++)
		draw_row(e, sy);
	draw_bars(e);
}

/* Keep the cursor on screen; returns 1 if the view scrolled. */
static int scroll_to_cursor(struct editor *e)
{
	int text_rows = e->rows - 2, moved = 0;
	if (e->cy < e->top) {
		e->top = e->cy;
		moved = 1;
	}
	if (e->cy >= e->top + text_rows) {
		e->top = e->cy - text_rows + 1;
		moved = 1;
	}
	if (e->cx < e->left) {
		e->left = MAX(0, e->cx - 8);
		moved = 1;
	}
	if (e->cx >= e->left + e->cols - 1) {
		e->left = e->cx - e->cols + 9;
		moved = 1;
	}
	return moved;
}

static void place_cursor(struct editor *e)
{
	con_gotoxy(e->cx - e->left, e->cy - e->top + 1);
}

static void help(struct editor *e)
{
	static const char *const lines[] = {
		"",
		"  bdos EDIT - keys",
		"",
		"  Arrows, Home, End, PgUp, PgDn    move the cursor",
		"  Enter, Backspace, Del, Tab       edit",
		"  F2 or Ctrl+S                     save",
		"  F5                               save, then run the file",
		"                                   (.BAS, .BAT, .C)",
		"  Ctrl+Y                           delete the current line",
		"  Esc or Ctrl+Q                    quit",
		"",
		"  Press any key to return to your file.",
	};
	put_attr(ATTR_BAR);
	int w = MIN(e->cols, 60);
	int x0 = MAX(0, (e->cols - w) / 2), y0 = MAX(1, (e->rows - (int)NELEM(lines)) / 2);
	for (size_t i = 0; i < NELEM(lines); i++) {
		char row[128];
		snprintf(row, sizeof row, "%-*s", w, lines[i]);
		row[w] = 0;
		con_gotoxy(x0, y0 + (int)i);
		con_write(row);
	}
	con_cursor(0);
	con_getkey();
	con_cursor(1);
	draw_all(e);
}

/* Ask a yes/no/cancel question on the status line. */
static int ask(struct editor *e, const char *q)
{
	snprintf(e->msg, sizeof e->msg, "%s (Y/N/Esc)", q);
	draw_bars(e);
	place_cursor(e);
	for (;;) {
		int k = con_getkey();
		if (k == 'y' || k == 'Y')
			return 1;
		if (k == 'n' || k == 'N')
			return 0;
		if (k == 27 || k == 3 || k == KEY_EOF) {
			e->msg[0] = 0;
			return -1;
		}
	}
}

static void run_file(struct editor *e)
{
	if (e->dirty && save(e))
		return;
	put_attr(0x07);
	con_clear();
	char cmd[VFS_PATH_MAX + 8];
	snprintf(cmd, sizeof cmd, "\"%s\"", e->path);
	shell_exec(cmd);
	kprintf("\n-- Press any key to return to EDIT --");
	con_getkey();
	snprintf(e->msg, sizeof e->msg, "Back from %s", vfs_basename(e->path));
	put_attr(ATTR_TEXT);
	con_clear();
	draw_all(e);
}

int edit_file(const char *abs)
{
	struct editor *e = kzalloc(sizeof *e);
	if (!e)
		return 1;
	if (vfs_canon(abs, e->path, sizeof e->path))
		strncpy0(e->path, abs, sizeof e->path);
	struct vstat st;
	if (!vfs_stat(abs, &st) && st.type == VFS_DIR) {
		kprintf("%s is a directory\n", abs);
		kfree(e);
		return 1;
	}
	int r = load(e);
	if (r) {
		kprintf("Cannot open %s: %s\n", abs, vfs_strerror(r));
		kfree(e);
		return 1;
	}
	con_size(&e->cols, &e->rows);
	if (e->rows > 200)
		e->rows = 25;	/* output is captured; nothing sensible to do */
	put_attr(ATTR_TEXT);
	con_clear();
	con_cursor(1);
	draw_all(e);

	for (;;) {
		place_cursor(e);
		int k = con_getkey();
		int redraw_line = 0, redraw_all = 0;
		char *msg_before = e->msg[0] ? e->msg : NULL;
		e->msg[0] = 0;
		int *len = &e->len[e->cy];

		switch (k) {
		case KEY_UP:
			if (e->cy > 0)
				e->cy--;
			break;
		case KEY_DOWN:
			if (e->cy + 1 < e->nlines)
				e->cy++;
			break;
		case KEY_LEFT:
			if (e->cx > 0)
				e->cx--;
			else if (e->cy > 0) {
				e->cy--;
				e->cx = e->len[e->cy];
			}
			break;
		case KEY_RIGHT:
			if (e->cx < *len)
				e->cx++;
			else if (e->cy + 1 < e->nlines) {
				e->cy++;
				e->cx = 0;
			}
			break;
		case KEY_HOME:
			e->cx = 0;
			break;
		case KEY_END:
			e->cx = *len;
			break;
		case KEY_PGUP:
			e->cy = MAX(0, e->cy - (e->rows - 3));
			break;
		case KEY_PGDN:
			e->cy = MIN(e->nlines - 1, e->cy + (e->rows - 3));
			break;
		case '\n': {
			char *l = e->lines[e->cy];
			if (insert_line(e, e->cy + 1, l + e->cx, *len - e->cx) == 0) {
				e->len[e->cy] = e->cx;
				e->lines[e->cy][e->cx] = 0;
				e->cy++;
				e->cx = 0;
				e->dirty = 1;
				redraw_all = 1;
			}
			break;
		}
		case '\b':
			if (e->cx > 0) {
				char *l = e->lines[e->cy];
				memmove(l + e->cx - 1, l + e->cx, (size_t)(*len - e->cx + 1));
				(*len)--;
				e->cx--;
				e->dirty = 1;
				redraw_line = 1;
			} else if (e->cy > 0) {
				int py = e->cy - 1, pl = e->len[py];
				if (!line_reserve(e, py, pl + *len)) {
					memcpy(e->lines[py] + pl, e->lines[e->cy], (size_t)*len + 1);
					e->len[py] = pl + *len;
					delete_line(e, e->cy);
					e->cy = py;
					e->cx = pl;
					e->dirty = 1;
					redraw_all = 1;
				}
			}
			break;
		case KEY_DEL:
			if (e->cx < *len) {
				char *l = e->lines[e->cy];
				memmove(l + e->cx, l + e->cx + 1, (size_t)(*len - e->cx));
				(*len)--;
				e->dirty = 1;
				redraw_line = 1;
			} else if (e->cy + 1 < e->nlines) {
				int nl = e->len[e->cy + 1];
				if (!line_reserve(e, e->cy, *len + nl)) {
					memcpy(e->lines[e->cy] + *len, e->lines[e->cy + 1], (size_t)nl + 1);
					e->len[e->cy] += nl;
					delete_line(e, e->cy + 1);
					e->dirty = 1;
					redraw_all = 1;
				}
			}
			break;
		case 25:	/* ^Y: delete line */
			if (e->nlines > 1) {
				delete_line(e, e->cy);
				if (e->cy >= e->nlines)
					e->cy = e->nlines - 1;
			} else {
				e->len[0] = 0;
				e->lines[0][0] = 0;
			}
			e->dirty = 1;
			redraw_all = 1;
			break;
		case KEY_F1:
			help(e);
			break;
		case KEY_F1 + 1:	/* F2 */
		case 19:		/* ^S */
			save(e);
			break;
		case KEY_F1 + 4:	/* F5 */
			run_file(e);
			break;
		case 27:
		case 17:		/* ^Q */
		case KEY_EOF: {
			if (e->dirty) {
				int a = ask(e, "Save changes before quitting?");
				if (a < 0) {
					draw_bars(e);
					continue;
				}
				if (a == 1 && save(e)) {
					draw_bars(e);
					continue;
				}
			}
			goto out;
		}
		case '\t':
		default:
			if (k == '\t' || (k >= 0x20 && k < 0x7F)) {
				int n = k == '\t' ? TAB - (e->cx % TAB) : 1;
				if (line_reserve(e, e->cy, *len + n))
					break;
				char *l = e->lines[e->cy];
				memmove(l + e->cx + n, l + e->cx, (size_t)(*len - e->cx + 1));
				for (int i = 0; i < n; i++)
					l[e->cx + i] = (char)(k == '\t' ? ' ' : k);
				*len += n;
				e->cx += n;
				e->dirty = 1;
				redraw_line = 1;
			}
		}
		if (e->cx > e->len[e->cy])
			e->cx = e->len[e->cy];
		if (scroll_to_cursor(e) || redraw_all)
			draw_all(e);
		else if (redraw_line)
			draw_row(e, e->cy - e->top + 1);
		draw_bars(e);
		(void)msg_before;
	}
out:
	for (int i = 0; i < e->nlines; i++)
		kfree(e->lines[i]);
	kfree(e->lines);
	kfree(e->len);
	kfree(e->cap);
	kfree(e);
	put_attr(0x07);
	con_clear();
	return 0;
}
