/* wm.h - the bdos desktop: displays, windows, and the window manager */
#ifndef BDOS_WM_H
#define BDOS_WM_H

#include "gfx.h"

enum { EV_NONE, EV_KEY, EV_MOVE, EV_DOWN, EV_UP, EV_CLOSE, EV_RESIZE, EV_RAWKEY,
	EV_WHEEL,	/* key: clicks, + up (away from you), - down; x, y: where */
};

struct event {
	int type;
	int x, y, buttons;
	int key;
};

#define EVQ_SIZE 128
struct evq {
	struct event e[EVQ_SIZE];
	int head, tail;
};
void evq_push(struct evq *q, const struct event *e);
int evq_pop(struct evq *q, struct event *e);

/*
 * A display is what a window manager draws on and takes input from: the
 * real screen, a terminal window of another desktop (nesting), or a
 * network connection (remote sessions).
 */
struct wm;

struct display {
	struct surface *surf;
	void (*flush)(struct display *d, struct rect r);
	int (*next_event)(struct display *d, struct event *e);
	void (*close)(struct display *d);
	void (*attach)(struct display *d, struct wm *wm);	/* optional */
	int alive;			/* 0 once the far end is gone */
	int draws_pointer;
	/* when the far end draws the pointer: its shape (0 an arrow, else resize
	 * edges: 1 left, 2 right, 4 top, 8 bottom). Optional */
	void (*pointer)(struct display *d, int shape);
	const char *name;
	void *priv;
};

int desktop_run_on(struct display *d, const char *title);

#endif
