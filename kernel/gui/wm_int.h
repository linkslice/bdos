/*
 * wm_int.h - the desktop's insides, shared by wm.c (windows, input,
 * composing), forms.c, ui.c (menus, scroll bars), progman.c and filemgr.c.
 */
#ifndef BDOS_WM_INT_H
#define BDOS_WM_INT_H

#include "../kernel.h"
#include "../vfs.h"
#include "gfx.h"
#include "wm.h"
#include "form.h"

#define BORDER		4
#define TITLE_H		20
#define MENU_H		20
#define CLIENT_X	BORDER
#define CLIENT_Y	(BORDER + TITLE_H)
#define FRAME_W(cw)	((cw) + 2 * BORDER)
#define FRAME_H(ch)	((ch) + 2 * BORDER + TITLE_H)
#define GRIP		8	/* how near the edge a press starts a resize */
#define SB_W		16	/* scroll bar width */
#define ICON_SLOT_W	112	/* minimized windows along the bottom */
#define ICON_SLOT_H	64

extern int dclick_ms;		/* the mouse.dclick setting */

enum { W_TERM, W_PROGMAN, W_FILES, W_CANVAS, W_DIALOG, W_FORM };

/* modifier bits carried in event.buttons above the mouse buttons */
#define MOD_SHIFT	0x100
#define MOD_CTRL	0x200
#define MOD_ALT		0x400

struct theme {
	uint32_t desktop, face, active, inactive, title_text, menu_text, select;
	uint32_t workspace;	/* inside Program Manager and File Manager */
	uint8_t pattern[8];	/* desktop pattern, 1 bits drawn in pattern_ink */
	uint32_t pattern_ink;
	int has_pattern;
};
extern struct theme theme;

struct wm;
struct win;

/* ---- terminal windows ---- */

struct termwin {
	struct console con;
	struct win *win;
	struct wm *wm;
	int cols, rows, cx, cy, attr, cursor_on;
	uint16_t *grid;
	int keys[256];
	int kh, kt;
	struct task *task;
	int closed;		/* the window is gone: input reads as end-of-file */
	int hold;		/* keep the window after the command ends */
	int child;		/* a nested desktop or remote view owns the pixels */
	struct evq child_ev;
	/* scrollback: lines gone off the top, oldest first in a ring */
	uint16_t **sb;
	uint16_t *sblen;
	int sbhead, sbcount;
	int view;		/* how many lines back we're looking (0: live) */
	/* a selection, in lines counted from the oldest kept (history, then the screen) */
	int sel, selecting, sa_line, sa_col, se_line, se_col;
	char *paste;		/* being typed in, from the clipboard */
	size_t paste_pos;
	struct termwin *next_zombie;
};

struct dialog {
	char lines[6][80];
	int nlines;
	int input;		/* has a text field */
	char field[160];
	int done;		/* 1 OK, 2 Cancel */
	int purpose;		/* 0 message, 1 Run..., 2 Exit?, 3 a program waits */
};

/*
 * What a kind of window does. Coordinates are in the window's client area.
 * mouse() gets EV_DOWN/EV_UP/EV_MOVE; once a button goes down inside a
 * window, moves and the release go to it until every button is up.
 */
struct winops {
	void (*key)(struct win *w, int key);
	void (*mouse)(struct win *w, int type, int x, int y, int buttons, int dbl);
	void (*context)(struct win *w, int x, int y);	/* right click */
	void (*resized)(struct win *w);
	void (*repaint)(struct win *w);			/* colours changed */
	void (*destroy)(struct win *w);			/* free priv */
	/* drag and drop: paths are '\n'-separated absolute paths */
	int (*drop)(struct win *w, int x, int y, const char *paths, int copy);
	const char *icon;				/* when minimized */
	/* or draw the 32x32 icon yourself (live icons, like Pulse's) */
	void (*draw_icon)(struct win *w, struct surface *s, int x, int y);
	/* the mouse wheel: clicks + up, - down; x, y in the client area */
	void (*wheel)(struct win *w, int x, int y, int clicks);
};

struct win {
	int id;
	int kind;
	char title[64];
	int x, y, w, h;		/* frame, in desktop coordinates */
	struct surface content;
	struct wm *wm;
	const struct winops *ops;
	void *priv;
	struct termwin *term;
	struct dialog *dlg;
	struct form *form;
	int resizable, minimizable;
	int min_cw, min_ch;	/* smallest client size when resizing (0: default) */
	int maximized, minimized;
	struct rect restore;	/* frame before maximizing */
	struct task *owner;	/* canvas windows: the task drawing in it */
	struct evq events;
	int last_key, last_x, last_y;
	int closed;
	struct menubar *menubar;	/* Alt+letter opens its menus */
	struct win *next;	/* z order, bottom first */
};

/* a popup menu: menu bars, right clicks, the control menu */
#define MENU_MAX 32
enum { MI_DISABLED = 1, MI_CHECKED = 2, MI_SEP = 4 };
struct menu_item {
	char label[40];
	int flags;
	int id;
};

static inline void menu_put(struct menu_item *it, int *n, const char *label, int flags, int id)
{
	if (*n >= MENU_MAX)
		return;
	strncpy0(it[*n].label, label, sizeof it[*n].label);
	it[*n].flags = flags;
	it[*n].id = id;
	(*n)++;
}
struct popup {
	struct menu_item items[MENU_MAX];
	int n;
	struct rect r;		/* desktop coordinates */
	int sel;
	void (*pick)(struct wm *wm, void *arg, int id);
	void *arg;
	struct win *owner;	/* closed with it */
	int bar_x, bar_w;	/* the menu-bar title it hangs from, to highlight */
};

struct dnd {
	int active, armed;
	struct win *src;
	char *paths;
	char icon[16];
	int sx, sy;		/* where the press started */
	int count;
};

struct wm {
	struct display *disp;
	struct win *wins;
	struct win *focus;
	int mx, my, buttons, mods;
	int rawmods, raw_e0;	/* Shift/Ctrl/Alt as the raw make/break codes have them (bterm) */
	struct win *drag;
	int drag_dx, drag_dy;
	struct rect dirty;
	int running;
	int next_id;
	int depth;
	uint64_t click_ms;
	int click_x, click_y, click_button;
	struct win *progman;
	char title[48];
	uint32_t desktop;	/* this desktop's own colour */
	int follow_time;	/* top level: colour follows the time of day */
	uint32_t timed_colour;
	uint64_t next_time_check;
	struct win *resizing;	/* border being dragged */
	int rs_edges;		/* 1 left 2 right 4 top 8 bottom */
	struct rect rs_orig, ghost;
	int rs_mx, rs_my;
	int ptr;		/* pointer shape: 0 arrow, else resize edges */
	struct win *grab;	/* the window that got the button press */
	struct popup *popup;
	struct win *kbmove;	/* Move/Size from the keyboard: the window, and 1 move or 2 size */
	int kbmode;
	struct dnd dnd;
	struct wm *next_wm;
};

extern struct wm *all_wms;

/* ---- wm.c ---- */
uint32_t P(uint32_t rgb);
void wm_dirty(struct wm *wm, struct rect r);
void wm_dirty_all(struct wm *wm);
struct rect frame_rect(struct win *w);
void win_dirty(struct win *w, struct rect r);
void win_dirty_all(struct win *w);
struct win *win_new(struct wm *wm, int kind, const char *title, int x, int y, int cw, int ch);
void raise_win(struct wm *wm, struct win *w);
struct win *topmost(struct wm *wm);
void win_destroy(struct wm *wm, struct win *w);
void win_resize(struct win *w, int x, int y, int fw, int fh);
int win_can_resize(struct win *w);
void win_minimize(struct win *w, int on);
void toggle_maximize(struct wm *wm, struct win *w);
void close_request(struct wm *wm, struct win *w);
struct win *open_term(struct wm *wm, const char *title, const char *cmd, int cols, int rows, int x, int y);
void launch_cmd(struct wm *wm, const char *label, const char *cmd);
struct lnk {
	char cmd[200], dir[200], speed[12];
	int minimized;
};
int lnk_read(const char *path, struct lnk *l);
int lnk_write(const char *path, const struct lnk *l);
void launch_item(struct wm *wm, const char *label, const char *cmd, const char *dir, int minimized,
    const char *speed);
void launch_path(struct wm *wm, const char *path);	/* open a file the obvious way */
struct win *open_dialog(struct wm *wm, const char *title, const char *msg, int input, int purpose);
void render_dialog(struct win *w);
void about(struct wm *wm);
void tile(struct wm *wm, int cascade);
void next_window(struct wm *wm);
void exit_desktop(struct wm *wm);
void desktop_menu(struct wm *wm, int x, int y);
void control_menu(struct wm *wm, struct win *w, int x, int y);
const char *icon_for_file(const char *name, int is_dir);
void term_type(struct win *w, const char *text);
void repaint_all(void);
void win_icon_dirty(struct win *w);
void wm_dnd_start(struct wm *wm, struct win *src, const char *paths, const char *icon);
int console_is_termwin(struct console *c);
/* the text clipboard (CLIP$ too) */
void clip_text_set(const char *text, size_t n);
const char *clip_text_get(void);

/* ---- ui.c: popup menus, scroll bars, small dialogs ---- */
void popup_open(struct wm *wm, int x, int y, const struct menu_item *items, int n,
    void (*pick)(struct wm *, void *, int), void *arg, struct win *owner);
void text_mnemonic(struct surface *s, int x, int y, const char *label, uint32_t ink);
void popup_close(struct wm *wm);
void popup_draw(struct wm *wm, struct surface *s);
int popup_mouse(struct wm *wm, int type, int x, int y);	/* 1 if it took the event */
int popup_key(struct wm *wm, int k);
void popup_select_first(struct wm *wm);	/* opened from the keyboard: highlight the first item */

/* menu bars drawn inside a window: titles left to right */
struct menubar {
	const char *const *titles;
	int n;
	int open;		/* which one has its popup down, -1 none */
};
void menubar_draw(struct surface *s, struct menubar *mb, int width);
int menubar_hit(struct menubar *mb, int x, int *tx, int *tw);	/* title index or -1 */

struct scrollbar {
	int vertical;
	struct rect r;		/* in the surface it's drawn on */
	int total, visible, pos;
	int dragging, grab;	/* thumb drag */
};
void sb_draw(struct surface *s, struct scrollbar *sb);
int sb_mouse(struct scrollbar *sb, int type, int x, int y, int step);	/* 1 if pos changed */
int sb_max(const struct scrollbar *sb);
void sb_clamp(struct scrollbar *sb);

void ui_confirm(struct wm *wm, const char *title, const char *msg, const char *yes,
    void (*done)(struct wm *wm, void *arg, int yes), void *arg);
void ui_prompt(struct wm *wm, const char *title, const char *label, const char *initial,
    void (*done)(struct wm *wm, void *arg, const char *text), void *arg);

/* ---- forms.c ---- */
extern const struct winops form_ops;
void form_tick_all(struct wm *wm);
void arrow_glyph(struct surface *s, int cx, int cy, int dir, uint32_t c);
void render_form(struct win *w);

/* ---- progman.c ---- */
struct win *open_progman(struct wm *wm);
void progman_reload(struct wm *wm);

/* ---- filemgr.c ---- */
struct win *open_files(struct wm *wm, const char *dir);
void files_refresh_all(void);		/* something changed on disk */

/* ---- fileops.c: copying, moving and deleting trees ---- */
#define FOP_MOVE	1
#define FOP_REPLACE	2	/* overwrite what's there */
#define FOP_DUP		4	/* a copy onto itself makes "NAME (2)" */
int fop_copy(const char *src, const char *dst_dir, int flags);	/* 0 or E_ */
int fop_exists_in(const char *src, const char *dst_dir);	/* would it collide? */
int fop_delete(const char *path);
int fop_unique(const char *dir, const char *name, char *out, size_t n);

/* ---- themes.c ---- */
void themes_init(void);
uint32_t time_of_day_colour(int minutes);

#endif
