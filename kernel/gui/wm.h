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

/*
 * Pointer events carry more than the place, in bits of buttons above the
 * modifiers:
 *   EVB_DELTA  key holds how far the mouse itself moved (dx in the low 16
 *              bits, dy in the high, signed): a relative mouse, or bterm with
 *              the pointer captured. x, y stay where the pointer is drawn.
 *   EVB_REL    x, y are the motion, not a place: what a window that has
 *              captured the pointer (a DOS game) is given.
 */
#define EVB_DELTA	0x1000
#define EVB_REL		0x2000
static inline int ev_dx(const struct event *e) { return (int16_t)(e->key & 0xFFFF); }
static inline int ev_dy(const struct event *e) { return (int16_t)((uint32_t)e->key >> 16); }
static inline int ev_delta(int dx, int dy) { return (int)((uint32_t)(uint16_t)dx | (uint32_t)(uint16_t)dy << 16); }

/* display->pointer(): the pointer is captured -- hide it, and send motion */
#define PTR_CAPTURED	0x100

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
	/* optional: what flush prepared, sent now (the big kernel lock is held
	 * here, but not during flush) */
	void (*commit)(struct display *d);
	/* optional: the display's pixels belong to someone else (a window on
	 * another desktop): hold this while drawing into them */
	void (*lock)(struct display *d);
	void (*unlock)(struct display *d);
	/* optional: the far end is still taking in earlier pictures. Something
	 * drawing a new one many times a second (a game) skips a frame instead */
	int (*busy)(struct display *d);
	/* set by what draws into the display: it reads the mouse's motion, not
	 * its place (a 3D game), so a click in it should capture the pointer */
	volatile int wants_capture;
	const char *name;
	void *priv;
	/* the far end draws for itself: windows' surfaces are mirrored there */
	struct gfx_rec *rec;
};

int desktop_run_on(struct display *d, const char *title);
/* around drawing straight into a display's surface (a DOS game, a viewer) */
void display_lock(struct display *d);
void display_unlock(struct display *d);
int display_busy(struct display *d);
int display_remote(struct display *d);

#endif
