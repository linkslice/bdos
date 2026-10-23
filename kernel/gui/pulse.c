/*
 * pulse.c - Pulse: how busy every core is, after Haiku's Pulse.
 *
 * One LED column per core, updated four times a second, with a power
 * button under each: switch a core off and the scheduler stops giving it
 * work (it sits halted until you switch it back on). Minimized, the icon
 * keeps showing live bars.
 */
#include "../kernel.h"
#include "gfx.h"
#include "form.h"

#define SEGS 16
#define TOP 30
#define BOTTOM 78

struct pulse {
	int area;
	char name[64];
};

static uint32_t seg_colour(int i, int lit)
{
	/* green, then yellow, then red at the top */
	uint32_t on = i >= SEGS - 2 ? 0xFF3030 : i >= SEGS * 2 / 3 ? 0xF0E030 : 0x30E040;
	uint32_t off = i >= SEGS - 2 ? 0x401010 : i >= SEGS * 2 / 3 ? 0x403C10 : 0x0C3A12;
	return lit ? on : off;
}

/* column width: 30 pixels, narrower when there are lots of cores */
static int col_w(struct rect r)
{
	return MAX(6, MIN(30, (r.w - 16) / MAX(1, ncpus)));
}

/* where the columns go: centred if the window is wider than they need */
static int first_x(struct rect r)
{
	int need = ncpus * col_w(r);
	return r.x + MAX(8, (r.w - need) / 2);
}

static void pulse_draw(struct form *f, int id, struct surface *s, struct rect r)
{
	(void)id;
	struct pulse *p = form_priv(f);
	fill(s, r.x, r.y, r.w, r.h, col(0x101010));
	int total = 0, on = 0;
	for (int i = 0; i < ncpus; i++) {
		total += cpus[i].load;
		on += cpus[i].online && !cpus[i].parked;
	}
	char head[96];
	snprintf(head, sizeof head, "%d core%s, %d on, %d%% busy", ncpus, ncpus == 1 ? "" : "s", on,
	    ncpus ? total / ncpus : 0);
	text(s, r.x + 8, r.y + 6, head, col(0xC0FFC0));
	int x0 = first_x(r), COL_W = col_w(r);
	int bar_h = r.h - TOP - BOTTOM;
	if (bar_h < SEGS * 3)
		bar_h = SEGS * 3;
	int seg_h = bar_h / SEGS;
	for (int c = 0; c < ncpus; c++) {
		int x = x0 + c * COL_W, parked = cpus[c].parked || !cpus[c].online;
		int lit = parked ? 0 : (cpus[c].load * SEGS + 50) / 100;
		if (!parked && cpus[c].load > 0 && lit == 0)
			lit = 1;	/* a flicker of life */
		for (int i = 0; i < SEGS; i++) {
			int y = r.y + TOP + (SEGS - 1 - i) * seg_h;
			fill(s, x + 4, y, COL_W - 10, seg_h - 1, col(seg_colour(i, i < lit)));
		}
		char num[8];
		snprintf(num, sizeof num, "%d", c);
		int ly = r.y + TOP + SEGS * seg_h + 4;
		text(s, x + (COL_W - 2 - (int)strlen(num) * FONT_W) / 2, ly, num, col(0xC0C0C0));
		/* the power button: lit green when the core is on */
		int bx = x + 6, by = ly + 20, bw = COL_W - 14, bh = 14;
		fill(s, bx, by, bw, bh, col(0xC0C0C0));
		bevel(s, bx, by, bw, bh, !parked);
		if (bw > 8)
			fill(s, bx + 4, by + 4, bw - 8, bh - 8, col(parked ? 0x303030 : 0x30F040));
	}
	char name[64];
	strncpy0(name, p->name, (size_t)MAX(1, MIN(63, (r.w - 16) / FONT_W + 1)));
	text(s, r.x + 8, r.y + r.h - 20, name, col(0x808080));
}

static void pulse_icon(struct form *f, struct surface *s, int x, int y)
{
	(void)f;
	fill(s, x, y, 32, 32, col(0x101010));
	outline(s, x, y, 32, 32, col(0x808080));
	/* up to 8 bars; with more cores, each bar is a group's average */
	int bars = MIN(ncpus, 8), per = (ncpus + bars - 1) / bars;
	int bw = 28 / bars;
	for (int b = 0; b < bars; b++) {
		int sum = 0, n = 0;
		for (int c = b * per; c < MIN(ncpus, (b + 1) * per); c++, n++)
			sum += cpus[c].parked ? 0 : cpus[c].load;
		int load = n ? sum / n : 0, h = load * 28 / 100;
		int bx = x + 2 + b * bw;
		fill(s, bx, y + 2, MAX(1, bw - 1), 28, col(0x0C3A12));
		if (h)
			fill(s, bx, y + 30 - h, MAX(1, bw - 1), h, col(load > 85 ? 0xFF3030 : 0x30E040));
	}
}

static void pulse_event(struct form *f, int id, int ev)
{
	struct pulse *p = form_priv(f);
	if (ev == FE_CLOSE) {
		kfree(p);
		return;
	}
	if (ev == FE_RESIZE) {
		int w, h;
		form_size(f, &w, &h);
		form_move(f, p->area, 0, 0, w, h);
	} else if (ev == FE_TICK) {
		form_redraw(f);
	} else if (ev == FE_CLICK && id == p->area) {
		int px, py, w, h;
		form_point(f, &px, &py);
		form_size(f, &w, &h);
		struct rect r = { 0, 0, w, h };
		int c = (px - first_x(r)) / col_w(r);
		int bar_h = MAX(h - TOP - BOTTOM, SEGS * 3), seg_h = bar_h / SEGS;
		int by = TOP + SEGS * seg_h + 4 + 20;
		if (px >= first_x(r) && c >= 0 && c < ncpus && py >= by - 4 && py < by + 18) {
			if (cpu_park(c, !cpus[c].parked))
				wm_message(form_wm(f), "Pulse", "At least one core has to stay on.");
			form_redraw(f);
		}
	}
}

void pulse_open(struct wm *wm)
{
	struct form *e = form_find(wm, "Pulse");
	if (e) {
		form_raise(e);
		return;
	}
	struct pulse *p = kzalloc(sizeof *p);
	if (!p)
		return;
	arch_cpu_name(p->name, sizeof p->name);
	int w = MAX(240, MIN(ncpus * 30 + 16, 900)), h = 260;
	struct form *f = form_new("Pulse", w, h, pulse_event, p);
	if (!f) {
		kfree(p);
		return;
	}
	p->area = form_add(f, FW_CUSTOM, 0, 0, w, h, NULL);
	form_custom(f, p->area, pulse_draw);
	form_resizable(f, 160, 160);
	form_icon_draw(f, pulse_icon);
	form_tick(f, 250);
	form_show(wm, f, -1, 60);
}
