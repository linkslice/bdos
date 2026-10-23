/*
 * form.h - dialog-box windows built from widgets: the toolkit behind the
 * Control Panel and its applets.
 *
 *   struct form *f = form_new("Mouse", 320, 200, handler, priv);
 *   int speed = form_add(f, FW_SLIDER, 20, 40, 200, 24, NULL);
 *   form_range(f, speed, 1, 10);
 *   form_show(wm, f, -1, -1);
 *
 * The handler hears about everything: fn(f, widget_id, FE_...). Widget ids
 * are the numbers form_add returns; -1 means the form itself.
 */
#ifndef BDOS_FORM_H
#define BDOS_FORM_H

#include "gfx.h"

struct form;
struct wm;

enum {
	FW_LABEL,	/* text */
	FW_GROUP,	/* an etched frame with a caption */
	FW_BUTTON,	/* push button; form_default() makes it the Enter button */
	FW_EDIT,	/* one line of text */
	FW_SPIN,	/* a number with up/down arrows */
	FW_CHOICE,	/* one of several strings, with < > arrows */
	FW_CHECK,	/* a check box */
	FW_SLIDER,	/* a horizontal slider */
	FW_ICON,	/* a big icon with a caption (Control Panel) */
	FW_SWATCH,	/* a colour sample; val=1 draws it selected */
	FW_LIST,	/* a scrolling list of lines */
	FW_CUSTOM,	/* the program draws it */
};

enum {
	FE_CHANGE,	/* the value changed */
	FE_CLICK,	/* button pressed, swatch/icon/custom clicked */
	FE_DBLCLICK,	/* icon, list line or custom area double-clicked */
	FE_TICK,	/* the timer asked for with form_tick() */
	FE_RESIZE,	/* the window was resized (form_size() has the new size) */
	FE_CLOSE,	/* the window is closing (id -1) */
	FE_RCLICK,	/* a custom area was right-clicked (form_point says where) */
};

typedef void (*form_fn)(struct form *f, int id, int ev);
typedef void (*form_draw_fn)(struct form *f, int id, struct surface *s, struct rect r);

struct form *form_new(const char *title, int w, int h, form_fn fn, void *priv);
int form_add(struct form *f, int kind, int x, int y, int w, int h, const char *text);
int form_show(struct wm *wm, struct form *f, int x, int y);	/* 0 on success */
void form_close(struct form *f);	/* safe from inside the handler */

/* finding an open form by title (so applets aren't opened twice) */
struct form *form_find(struct wm *wm, const char *title);
void form_raise(struct form *f);

void form_range(struct form *f, int id, long min, long max);
void form_options(struct form *f, int id, const char *const *opts, int n);
void form_icon(struct form *f, int id, const char *icon);
void form_custom(struct form *f, int id, form_draw_fn draw);
void form_default(struct form *f, int id);
void form_enable(struct form *f, int id, int on);
void form_hide(struct form *f, int id, int hidden);
void form_move(struct form *f, int id, int x, int y, int w, int h);
void form_focus(struct form *f, int id);
int form_focused(struct form *f);

long form_val(struct form *f, int id);
void form_set_val(struct form *f, int id, long v);
const char *form_text(struct form *f, int id);
void form_set_text(struct form *f, int id, const char *text);
void form_set_rgb(struct form *f, int id, uint32_t rgb);
uint32_t form_rgb(struct form *f, int id);
void form_list_clear(struct form *f, int id);
void form_list_add(struct form *f, int id, const char *line);
int form_list_top(struct form *f, int id);		/* the first line showing */
void form_list_set_top(struct form *f, int id, int top);
int form_list_rows(struct form *f, int id);		/* how many lines show */
int form_list_count(struct form *f, int id);

void form_tick(struct form *f, int ms);	/* FE_TICK every ms (0 = off) */
void form_resizable(struct form *f, int min_w, int min_h);
void form_size(struct form *f, int *w, int *h);
void form_point(struct form *f, int *x, int *y);	/* last click, widget-relative */
int form_buttons(struct form *f);			/* and its buttons and Shift/Ctrl (MOD_ bits) */
void form_pointer(struct form *f, int *x, int *y);	/* the pointer now, window-relative (maybe outside) */
int form_is_active(struct form *f);			/* the window has the keyboard */
int form_is_minimized(struct form *f);
/* keys first: fn returns 1 when it used the key (Esc still closes unless it does) */
void form_key_hook(struct form *f, int (*fn)(struct form *f, int key));
void form_redraw(struct form *f);
void *form_priv(struct form *f);
struct wm *form_wm(struct form *f);
void form_title(struct form *f, const char *title);
void form_icon_name(struct form *f, const char *icon);	/* when minimized */
void form_icon_draw(struct form *f, void (*fn)(struct form *f, struct surface *s, int x, int y));

/* the desktop side, for applets */
void wm_launch(struct wm *wm, const char *label, const char *cmd);
int wm_depth(struct wm *wm);
uint32_t wm_desktop_colour(struct wm *wm);
void wm_set_desktop_colour(struct wm *wm, uint32_t rgb);
void wm_message(struct wm *wm, const char *title, const char *msg);
uint32_t ui_colour(int which);
/* small modal-ish questions, answered through a callback */
void ui_confirm(struct wm *wm, const char *title, const char *msg, const char *yes,
    void (*done)(struct wm *wm, void *arg, int yes), void *arg);
void ui_prompt(struct wm *wm, const char *title, const char *label, const char *initial,
    void (*done)(struct wm *wm, void *arg, const char *text), void *arg);	/* 0 face, 1 active title, 2 selection */

#endif
