/*
 * xhci.c - USB 3 host controllers (xHCI), and the USB keyboards, mice and
 * tablets plugged into them.
 *
 * Machines without a PS/2 port (or with the firmware's PS/2 emulation
 * switched off) only have USB for input, so bdos speaks it itself: reset
 * the controller, take it from the BIOS, set up the command and event
 * rings, address every device on a root port, and for each HID interface
 * poll its interrupt endpoint. Keyboards become PS/2 scancodes (so DOS
 * programs see them raw, as ever); mice and tablets become pointer moves.
 * Hubs aren't handled: plug keyboards straight in.
 */
#include "../kernel.h"
#include "drv.h"

void keyboard_scancode(uint8_t sc);

/* operational registers */
#define USBCMD		0x00
#define USBSTS		0x04
#define CRCR		0x18
#define DCBAAP		0x30
#define CONFIG		0x38

/* TRB types */
#define TRB_NORMAL	1
#define TRB_SETUP	2
#define TRB_DATA	3
#define TRB_STATUS	4
#define TRB_LINK	6
#define TRB_ENABLE_SLOT	9
#define TRB_ADDRESS	11
#define TRB_CONFIG_EP	12
#define TRB_EVAL_CTX	13
#define EV_TRANSFER	32
#define EV_CMD_DONE	33
#define EV_PORT		34

#define RING_TRBS	256
#define MAX_DEVS	16

struct trb {
	uint64_t param;
	uint32_t status;
	uint32_t control;
};

struct ring {
	struct trb *t;
	int enq;
	int cycle;
};

enum { HID_KBD = 1, HID_MOUSE, HID_ABS };

struct hid {
	int used, kind, slot, dci;
	struct ring ring;
	uint8_t *buf;
	int len;
	uint8_t last[8];		/* keyboards: the last report */
	/* report layout (non-boot devices) */
	int report_id;
	int x_off, x_size, y_off, y_size, w_off, w_size, b_off, b_count;
	int32_t x_max, y_max;
	int abs;
	uint64_t reports;
	int lastx, lasty, lastb;
	char what[24];
};

static volatile uint8_t *cap, *op, *rt;
static volatile uint32_t *db;
static int csz;			/* context size: 32 or 64 */
static uint64_t *dcbaa;
static struct ring cmd;
static struct trb *evt;
static int evt_deq, evt_cycle;
static struct hid hids[MAX_DEVS];
static int nports, port_proto[256];
static int found;

static uint32_t rd(volatile uint8_t *b, int o) { return *(volatile uint32_t *)(b + o); }
static void wr(volatile uint8_t *b, int o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static void wr64(volatile uint8_t *b, int o, uint64_t v)
{
	*(volatile uint32_t *)(b + o) = (uint32_t)v;
	*(volatile uint32_t *)(b + o + 4) = (uint32_t)(v >> 32);
}

static int ring_init(struct ring *r)
{
	r->t = pmm_alloc(1);
	if (!r->t)
		return -1;
	r->enq = 0;
	r->cycle = 1;
	/* the last TRB links back to the first, toggling the cycle */
	r->t[RING_TRBS - 1].param = (uint64_t)r->t;
	r->t[RING_TRBS - 1].control = TRB_LINK << 10 | 2;
	return 0;
}

static struct trb *ring_put(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
	struct trb *t = &r->t[r->enq];
	t->param = param;
	t->status = status;
	mmio_barrier();
	t->control = (control & ~1u) | (uint32_t)r->cycle;
	if (++r->enq == RING_TRBS - 1) {
		struct trb *link = &r->t[RING_TRBS - 1];
		link->control = (link->control & ~1u) | (uint32_t)r->cycle;
		r->enq = 0;
		r->cycle ^= 1;
	}
	return t;
}

/* the next event, or NULL */
static struct trb *event_next(void)
{
	struct trb *e = &evt[evt_deq];
	if ((e->control & 1) != (uint32_t)evt_cycle)
		return NULL;
	if (++evt_deq == RING_TRBS) {
		evt_deq = 0;
		evt_cycle ^= 1;
	}
	wr64(rt, 0x20 + 0x18, (uint64_t)&evt[evt_deq] | 8);	/* ERDP, clearing busy */
	return e;
}

static void hid_report(struct hid *h, const uint8_t *r, int n);

/* wait for an event of one type (dealing with reports that come meanwhile) */
static int wait_event(int type, int slot, struct trb *out, int ms)
{
	uint64_t until = uptime_ms() + (uint64_t)ms;
	while (uptime_ms() < until) {
		struct trb *e = event_next();
		if (!e) {
			delay_us(50);
			continue;
		}
		int t = (int)(e->control >> 10) & 0x3F;
		if (t == type && (!slot || (int)(e->control >> 24) == slot)) {
			*out = *e;
			return 0;
		}
	}
	return -1;
}

static int command(uint64_t param, uint32_t status, uint32_t control, struct trb *ev)
{
	ring_put(&cmd, param, status, control);
	db[0] = 0;
	if (wait_event(EV_CMD_DONE, 0, ev, 1000))
		return -1;
	return (int)(ev->status >> 24) == 1 ? 0 : -1;
}

/* ---- devices -------------------------------------------------------------------------- */

struct dev {
	int slot, speed, port;
	uint8_t *in_ctx, *out_ctx;
	struct ring ep0;
	uint8_t *buf;		/* a page for descriptors */
	int mps0;
};

static uint32_t *ctx(uint8_t *base, int i) { return (uint32_t *)(base + i * csz); }

static int control(struct dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len,
    uint8_t *data)
{
	uint64_t setup = type | (uint64_t)req << 8 | (uint64_t)value << 16 | (uint64_t)index << 32 | (uint64_t)len << 48;
	int in = (type & 0x80) != 0;
	int trt = len ? (in ? 3 : 2) : 0;
	ring_put(&d->ep0, setup, 8, TRB_SETUP << 10 | 1 << 6 | (uint32_t)trt << 16);
	if (len)
		ring_put(&d->ep0, (uint64_t)data, len, TRB_DATA << 10 | (in ? 1u << 16 : 0));
	ring_put(&d->ep0, 0, 0, TRB_STATUS << 10 | 1 << 5 | ((len && in) ? 0 : 1u << 16));
	db[d->slot] = 1;
	struct trb ev;
	if (wait_event(EV_TRANSFER, d->slot, &ev, 1000))
		return -1;
	int cc = (int)(ev.status >> 24);
	return cc == 1 || cc == 13 ? 0 : -1;
}

static int addr_device(struct dev *d)
{
	memset(d->in_ctx, 0, 4096);
	ctx(d->in_ctx, 0)[1] = 3;		/* add the slot and EP0 */
	uint32_t *slot = ctx(d->in_ctx, 1), *ep0 = ctx(d->in_ctx, 2);
	slot[0] = 1u << 27 | (uint32_t)d->speed << 20;
	slot[1] = (uint32_t)d->port << 16;
	ep0[1] = 3 << 1 | 4 << 3 | (uint32_t)d->mps0 << 16;	/* 3 retries, control, max packet */
	ep0[2] = (uint32_t)(uint64_t)d->ep0.t | 1;
	ep0[3] = (uint32_t)((uint64_t)d->ep0.t >> 32);
	ep0[4] = 8;
	dcbaa[d->slot] = (uint64_t)d->out_ctx;
	struct trb ev;
	return command((uint64_t)d->in_ctx, 0, TRB_ADDRESS << 10 | (uint32_t)d->slot << 24, &ev);
}

/* ---- HID report descriptors: where the buttons and X and Y are ----------------------- */

static void parse_report_desc(struct hid *h, const uint8_t *p, int n)
{
	int page = 0, size = 0, count = 0, id = 0, bit = 0;
	int32_t lmax = 0;
	int usages[16], nu = 0, umin = -1, umax = -1;
	h->x_off = h->y_off = h->w_off = h->b_off = -1;
	for (int i = 0; i < n;) {
		uint8_t b = p[i++];
		int sz = b & 3;
		if (sz == 3)
			sz = 4;
		int type = (b >> 2) & 3, tag = b >> 4;
		uint32_t v = 0;
		for (int k = 0; k < sz && i + k < n; k++)
			v |= (uint32_t)p[i + k] << (8 * k);
		int32_t sv = sz == 1 ? (int8_t)v : sz == 2 ? (int16_t)v : (int32_t)v;
		i += sz;
		if (type == 1) {		/* global */
			switch (tag) {
			case 0: page = (int)v; break;
			case 2: lmax = sv; if (sz == 1 && lmax < 0) lmax = (int32_t)(v & 0xFF); else if (sz == 2 && lmax < 0) lmax = (int32_t)(v & 0xFFFF); break;
			case 7: size = (int)v; break;
			case 8: id = (int)v; bit = 0; break;
			case 9: count = (int)v; break;
			}
		} else if (type == 2) {		/* local */
			if (tag == 0 && nu < 16)
				usages[nu++] = (int)(v & 0xFFFF) | (sz == 4 ? 0 : page << 16);
			else if (tag == 1)
				umin = (int)v;
			else if (tag == 2)
				umax = (int)v;
		} else if (type == 0) {		/* main */
			if (tag == 8) {		/* input */
				int rel = (v & 4) != 0;
				for (int f = 0; f < count; f++) {
					int u = f < nu ? usages[f] : nu ? usages[nu - 1] : (umin >= 0 ? (page << 16 | (umin + f)) : 0);
					if (umin >= 0 && umax >= umin && f <= umax - umin)
						u = page << 16 | (umin + f);
					int off = bit + f * size;
					if (page == 9 && h->b_off < 0) {
						h->b_off = off;
						h->b_count = count;
						h->report_id = id;
					} else if (u == (1 << 16 | 0x30)) {
						h->x_off = off, h->x_size = size, h->x_max = lmax, h->abs = !rel, h->report_id = id;
					} else if (u == (1 << 16 | 0x31)) {
						h->y_off = off, h->y_size = size, h->y_max = lmax;
					} else if (u == (1 << 16 | 0x38)) {
						h->w_off = off, h->w_size = size;
					}
				}
				bit += count * size;
			} else if (tag == 9 || tag == 0xB) {
				/* output and feature reports aren't in the input report */
			}
			nu = 0;
			umin = umax = -1;
		}
	}
}

static int32_t field(const uint8_t *r, int n, int off, int size, int is_signed)
{
	if (off < 0 || off + size > n * 8)
		return 0;
	uint32_t v = 0;
	for (int i = 0; i < size; i++)
		if (r[(off + i) / 8] & (1 << ((off + i) % 8)))
			v |= 1u << i;
	if (is_signed && size < 32 && (v & (1u << (size - 1))))
		v |= ~0u << size;
	return (int32_t)v;
}

/* ---- keyboards: HID usages to set 1 scancodes ------------------------------------------ */

static const uint16_t usage_sc[0x68] = {
	[0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18,
	0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
	[0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
	[0x28] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,
	0x35, 0x3A,
	[0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
	[0x46] = 0xE037, 0x46, 0,
	[0x49] = 0xE052, 0xE047, 0xE049, 0xE053, 0xE04F, 0xE051, 0xE04D, 0xE04B, 0xE050, 0xE048,
	[0x53] = 0x45, 0xE035, 0x37, 0x4A, 0x4E, 0xE01C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49,
	0x52, 0x53, 0x56,
};
/* the modifier byte, bit by bit: LCtrl LShift LAlt LGUI RCtrl RShift RAlt RGUI */
static const uint16_t mod_sc[8] = { 0x1D, 0x2A, 0x38, 0xE05B, 0xE01D, 0x36, 0xE038, 0xE05C };

static void send_sc(uint16_t sc, int release)
{
	if (!sc)
		return;
	if (sc >> 8)
		keyboard_scancode(0xE0);
	keyboard_scancode((uint8_t)(sc | (release ? 0x80 : 0)));
}

static void kbd_report(struct hid *h, const uint8_t *r)
{
	uint8_t mods = r[0], old = h->last[0];
	for (int i = 0; i < 8; i++)
		if ((mods ^ old) & (1 << i))
			send_sc(mod_sc[i], !(mods & (1 << i)));
	for (int i = 2; i < 8; i++) {		/* released */
		uint8_t u = h->last[i];
		if (u < 4 || u >= 0x68)
			continue;
		int still = 0;
		for (int k = 2; k < 8; k++)
			still |= r[k] == u;
		if (!still)
			send_sc(usage_sc[u], 1);
	}
	for (int i = 2; i < 8; i++) {		/* pressed */
		uint8_t u = r[i];
		if (u < 4 || u >= 0x68)
			continue;
		int was = 0;
		for (int k = 2; k < 8; k++)
			was |= h->last[k] == u;
		if (!was)
			send_sc(usage_sc[u], 0);
	}
	memcpy(h->last, r, 8);
}

static void hid_report(struct hid *h, const uint8_t *r, int n)
{
	h->reports++;
	if (h->kind == HID_KBD) {
		if (n >= 8)
			kbd_report(h, r);
		return;
	}
	if (h->kind == HID_MOUSE && h->x_off < 0) {	/* the boot protocol */
		if (n >= 3)
			input_mouse((int8_t)r[1], (int8_t)r[2], r[0] & 7);
		if (n >= 4)
			input_wheel((int8_t)r[3]);
		return;
	}
	if (h->report_id) {
		if (n < 1 || r[0] != h->report_id)
			return;
		r++;
		n--;
	}
	int buttons = field(r, n, h->b_off, MIN(h->b_count, 3), 0);
	int32_t x = field(r, n, h->x_off, h->x_size, !h->abs), y = field(r, n, h->y_off, h->y_size, !h->abs);
	if (h->abs) {
		int32_t mx = h->x_max > 0 ? h->x_max : 32767, my = h->y_max > 0 ? h->y_max : 32767;
		h->lastx = (int)((int64_t)x * 65535 / mx);
		h->lasty = (int)((int64_t)y * 65535 / my);
		h->lastb = buttons;
		input_mouse_abs(h->lastx, h->lasty, buttons);
	} else {
		input_mouse(x, y, buttons);
	}
	if (h->w_off >= 0)		/* HID's wheel: + is away from you */
		input_wheel((int)field(r, n, h->w_off, h->w_size, 1));
}

/* ---- setting a device up ---------------------------------------------------------------- */

static void queue_report(struct hid *h)
{
	ring_put(&h->ring, (uint64_t)h->buf, (uint32_t)h->len, TRB_NORMAL << 10 | 1 << 5 | 1 << 2);
	db[h->slot] = (uint32_t)h->dci;
}

static int config_endpoint(struct dev *d, struct hid *h, int epaddr, int mps, int interval)
{
	int dci = (epaddr & 0xF) * 2 + 1;
	if (ring_init(&h->ring))
		return -1;
	memset(d->in_ctx, 0, 4096);
	ctx(d->in_ctx, 0)[1] = 1 | 1u << dci;
	uint32_t *slot = ctx(d->in_ctx, 1);
	memcpy(slot, ctx(d->out_ctx, 0), (size_t)csz);
	slot[0] = (slot[0] & ~(0x1Fu << 27)) | (uint32_t)dci << 27;
	uint32_t *ep = ctx(d->in_ctx, 1 + dci);
	int iv;
	if (d->speed >= 3) {
		iv = MAX(0, MIN(interval - 1, 15));
	} else {
		iv = 3;
		while (iv < 10 && (1 << iv) < interval * 8)
			iv++;
	}
	ep[0] = (uint32_t)iv << 16;
	ep[1] = 3 << 1 | 7 << 3 | (uint32_t)mps << 16;	/* interrupt IN */
	ep[2] = (uint32_t)(uint64_t)h->ring.t | 1;
	ep[3] = (uint32_t)((uint64_t)h->ring.t >> 32);
	ep[4] = (uint32_t)mps | (uint32_t)mps << 16;
	struct trb ev;
	if (command((uint64_t)d->in_ctx, 0, TRB_CONFIG_EP << 10 | (uint32_t)d->slot << 24, &ev))
		return -1;
	h->slot = d->slot;
	h->dci = dci;
	return 0;
}

static void device_init(int port, int speed)
{
	struct trb ev;
	if (command(0, 0, TRB_ENABLE_SLOT << 10, &ev))
		return;
	struct dev dd, *d = &dd;
	memset(d, 0, sizeof *d);
	d->slot = (int)(ev.control >> 24);
	d->speed = speed;
	d->port = port;
	d->in_ctx = pmm_alloc(1);
	d->out_ctx = pmm_alloc(1);
	d->buf = pmm_alloc(1);
	if (!d->in_ctx || !d->out_ctx || !d->buf || ring_init(&d->ep0))
		return;
	d->mps0 = speed == 4 ? 512 : speed == 3 ? 64 : 8;
	if (addr_device(d))
		return;
	/* the device descriptor: the first 8 bytes give EP0's real packet size */
	if (control(d, 0x80, 6, 0x0100, 0, 8, d->buf))
		return;
	int mps = speed == 4 ? 1 << d->buf[7] : d->buf[7];
	if (mps && mps != d->mps0) {
		d->mps0 = mps;
		memset(d->in_ctx, 0, 4096);
		ctx(d->in_ctx, 0)[1] = 2;
		ctx(d->in_ctx, 2)[1] = 3 << 1 | 4 << 3 | (uint32_t)mps << 16;
		command((uint64_t)d->in_ctx, 0, TRB_EVAL_CTX << 10 | (uint32_t)d->slot << 24, &ev);
	}
	if (control(d, 0x80, 6, 0x0100, 0, 18, d->buf))
		return;
	uint16_t vid = (uint16_t)(d->buf[8] | d->buf[9] << 8), pid = (uint16_t)(d->buf[10] | d->buf[11] << 8);
	/* the configuration, with its interfaces and endpoints */
	if (control(d, 0x80, 6, 0x0200, 0, 9, d->buf))
		return;
	int total = MIN(d->buf[2] | d->buf[3] << 8, 1024);
	if (control(d, 0x80, 6, 0x0200, 0, (uint16_t)total, d->buf))
		return;
	uint8_t confval = d->buf[5];
	uint8_t desc[1024];
	memcpy(desc, d->buf, (size_t)total);
	if (control(d, 0x00, 9, confval, 0, 0, NULL))
		return;
	int iface = -1, iclass = 0, isub = 0, iproto = 0, hidlen = 0;
	for (int i = 0; i + 2 <= total && desc[i];) {
		int len = desc[i], type = desc[i + 1];
		if (type == 4) {		/* interface */
			iface = desc[i + 2];
			iclass = desc[i + 5];
			isub = desc[i + 6];
			iproto = desc[i + 7];
			hidlen = 0;
		} else if (type == 0x21 && iclass == 3) {	/* HID: its report descriptor's length */
			hidlen = desc[i + 7] | desc[i + 8] << 8;
		} else if (type == 5 && iclass == 3 && (desc[i + 2] & 0x80) && (desc[i + 3] & 3) == 3) {
			int slot = -1;
			for (int k = 0; k < MAX_DEVS; k++)
				if (!hids[k].used) {
					slot = k;
					break;
				}
			if (slot < 0)
				break;
			struct hid *h = &hids[slot];
			memset(h, 0, sizeof *h);
			int emps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
			h->x_off = h->y_off = h->w_off = h->b_off = -1;
			if (isub == 1 && iproto == 1) {
				h->kind = HID_KBD;
				control(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, NULL);	/* boot protocol */
				control(d, 0x21, 0x0A, 0, (uint16_t)iface, 0, NULL);	/* no idle repeats */
				strcpy(h->what, "keyboard");
			} else {
				/* mice and tablets: read what their reports look like */
				h->kind = HID_MOUSE;
				int rl = MIN(hidlen ? hidlen : 256, 1024);
				uint8_t *rd_buf = d->buf;
				if (!control(d, 0x81, 6, 0x2200, (uint16_t)iface, (uint16_t)rl, rd_buf))
					parse_report_desc(h, rd_buf, rl);
				if (h->x_off < 0 && isub == 1 && iproto == 2) {
					control(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, NULL);
				} else if (h->x_off < 0) {
					i += len;
					continue;	/* not something that points */
				}
				if (h->abs)
					h->kind = HID_ABS;
				strcpy(h->what, h->abs ? "tablet" : "mouse");
			}
			h->len = MIN(emps, 64);
			h->buf = pmm_alloc(1);
			if (!h->buf || config_endpoint(d, h, desc[i + 2], emps, desc[i + 6]))
				break;
			h->used = 1;
			found++;
			kprintf("USB: %s on port %d (%04x:%04x)\n", h->what, port, vid, pid);
			queue_report(h);
		}
		if (len < 2)
			break;
		i += len;
	}
}

/* ---- the controller ---------------------------------------------------------------------- */

static void poll_task(void *arg)
{
	(void)arg;
	for (;;) {
		struct trb *e;
		int any = 0;
		while ((e = event_next())) {
			any = 1;
			int type = (int)(e->control >> 10) & 0x3F;
			if (type != EV_TRANSFER)
				continue;
			int slot = (int)(e->control >> 24), ep = (int)(e->control >> 16) & 0x1F;
			int cc = (int)(e->status >> 24), left = (int)(e->status & 0xFFFFFF);
			for (int i = 0; i < MAX_DEVS; i++) {
				struct hid *h = &hids[i];
				if (!h->used || h->slot != slot || h->dci != ep)
					continue;
				if (cc == 1 || cc == 13)
					hid_report(h, h->buf, h->len - left);
				queue_report(h);
			}
		}
		if (!any)
			sleep_ms(2);
	}
}

static void take_from_bios(void)
{
	uint32_t hcc = rd(cap, 0x10);
	int off = (int)(hcc >> 16) << 2;
	while (off) {
		volatile uint8_t *x = cap + off;
		uint32_t v = rd(x, 0);
		if ((v & 0xFF) == 1) {		/* legacy support */
			wr(x, 0, v | 1 << 24);	/* OS owned */
			for (int i = 0; i < 1000 && (rd(x, 0) & 1 << 16); i++)
				delay_us(1000);
			wr(x, 4, rd(x, 4) & ~0x1Fu);	/* no SMIs */
		} else if ((v & 0xFF) == 2) {	/* supported protocol: which ports are USB 2, which 3 */
			int major = (int)(v >> 24);
			uint32_t pp = rd(x, 8);
			int first = (int)(pp & 0xFF), count = (int)((pp >> 8) & 0xFF);
			for (int p = first; p < first + count && p < 256; p++)
				port_proto[p] = major;
		}
		int next = (int)((v >> 8) & 0xFF) << 2;
		if (!next)
			break;
		off += next;
	}
}

static void controller_init(struct pci_dev *pd)
{
	pci_enable(pd);
	cap = pci_map_bar(pd, 0, 0x10000);
	if (!cap)
		return;
	op = cap + (rd(cap, 0) & 0xFF);
	rt = cap + (rd(cap, 0x18) & ~0x1Fu);
	db = (volatile uint32_t *)(cap + (rd(cap, 0x14) & ~3u));
	uint32_t hcs1 = rd(cap, 4), hcs2 = rd(cap, 8), hcc1 = rd(cap, 0x10);
	csz = (hcc1 & 4) ? 64 : 32;
	nports = (int)(hcs1 >> 24);
	int slots = MIN((int)(hcs1 & 0xFF), 32);
	take_from_bios();
	/* stop, reset */
	wr(op, USBCMD, rd(op, USBCMD) & ~1u);
	for (int i = 0; i < 100 && !(rd(op, USBSTS) & 1); i++)
		delay_us(1000);
	wr(op, USBCMD, 2);
	for (int i = 0; i < 1000 && ((rd(op, USBCMD) & 2) || (rd(op, USBSTS) & 1 << 11)); i++)
		delay_us(1000);
	wr(op, CONFIG, (uint32_t)slots);
	dcbaa = pmm_alloc(1);
	if (!dcbaa || ring_init(&cmd))
		return;
	int sp = (int)(((hcs2 >> 27) & 0x1F) | ((hcs2 >> 21) & 0x1F) << 5);
	if (sp) {
		uint64_t *arr = pmm_alloc(1);
		if (!arr)
			return;
		for (int i = 0; i < sp && i < 512; i++)
			arr[i] = (uint64_t)pmm_alloc(1);
		dcbaa[0] = (uint64_t)arr;
	}
	wr64(op, DCBAAP, (uint64_t)dcbaa);
	wr64(op, CRCR, (uint64_t)cmd.t | 1);
	/* one event ring segment */
	evt = pmm_alloc(1);
	uint64_t *erst = pmm_alloc(1);
	if (!evt || !erst)
		return;
	erst[0] = (uint64_t)evt;
	erst[1] = RING_TRBS;
	evt_deq = 0;
	evt_cycle = 1;
	wr(rt, 0x20 + 0x08, 1);				/* ERSTSZ */
	wr64(rt, 0x20 + 0x18, (uint64_t)evt);		/* ERDP */
	wr64(rt, 0x20 + 0x10, (uint64_t)erst);		/* ERSTBA */
	wr(op, USBCMD, 1);				/* run */
	for (int i = 0; i < 100 && (rd(op, USBSTS) & 1); i++)
		delay_us(1000);
	/* the ports */
	for (int p = 1; p <= nports; p++) {
		volatile uint8_t *ps = op + 0x400 + (p - 1) * 0x10;
		uint32_t v = rd(ps, 0);
		const uint32_t keep = ~(2u | 0x00FE0000u | (0xFu << 5));	/* never write PED, the change bits, or PLS */
		if (!(v & 1 << 9)) {
			wr(ps, 0, (v & keep) | 1 << 9);	/* power */
			delay_us(20000);
			v = rd(ps, 0);
		}
		if (!(v & 1))
			continue;		/* nothing plugged in */
		if (port_proto[p] != 3) {
			/* USB 2: reset the port to enable it */
			wr(ps, 0, (v & keep) | 1 << 4);
			for (int i = 0; i < 100 && !(rd(ps, 0) & 1 << 21); i++)
				delay_us(1000);
			v = rd(ps, 0);
			wr(ps, 0, (v & keep) | 1 << 21);	/* clear the reset change */
		}
		for (int i = 0; i < 100 && !(rd(ps, 0) & 2); i++)
			delay_us(1000);
		v = rd(ps, 0);
		if (!(v & 2))
			continue;
		delay_us(10000);	/* recovery time after reset */
		device_init(p, (int)(v >> 10) & 0xF);
	}
	/* port change events from all that: drop them */
	struct trb *e;
	while ((e = event_next()))
		;
	if (found)
		task_spawn("usb", poll_task, NULL, NULL);
}

void xhci_init(void)
{
	struct pci_dev *d = pci_find_class(0x0C, 0x03, 0x30, 0);
	if (d)
		controller_init(d);
}

void usb_list(void)
{
	if (!cap) {
		kprintf("No USB 3 (xHCI) controller.\n");
		return;
	}
	kprintf("xHCI controller, %d ports\n", nports);
	int n = 0;
	for (int i = 0; i < MAX_DEVS; i++)
		if (hids[i].used) {
			kprintf("  %-10s slot %d, %lu reports", hids[i].what, hids[i].slot, hids[i].reports);
			if (hids[i].kind == HID_ABS)
				kprintf(", at %d,%d of 65535", hids[i].lastx, hids[i].lasty);
			kprintf("\n");
			n++;
		}
	if (!n)
		kprintf("  no keyboards, mice or tablets\n");
}
