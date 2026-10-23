/*
 * dosbox.c - a DOS program's machine, and the loop that runs it.
 *
 * Each DOS program gets a box: a 486 with 16 MB, a VGA, the PC around it,
 * a BIOS and DOS. The box runs on the program's own task without the big
 * kernel lock, so several can run on several cores at once.
 *
 * The CPU runs in slices; between slices the loop delivers interrupts
 * (the timer at whatever rate the program programmed, the keyboard), feeds
 * in keys and mouse moves, and every 1/60 s puts the screen on show:
 *
 *  - text the program prints line by line goes to the console like any
 *    other program's output (so it scrolls, redirects and pipes);
 *  - once it treats the screen as a screen (sets a mode, moves the
 *    cursor, writes B800), the text page is mirrored onto the console;
 *  - graphics modes open a display: the program's desktop window, or the
 *    whole screen, with raw make/break keys and the mouse.
 *
 * Speed: the CPU can be held to a period-correct number of instructions
 * per millisecond (dos.speed: xt, at, 386, 486, pentium, max), set for
 * everything with CONTROL or for one program as a property of its file.
 */
#include "dos.h"
#include "../gate.h"
#include "../settings.h"

int display_want_size(struct display *d, int w, int h);
extern struct console *console_unwrap(struct console *c);
struct display *display_open(struct console *c, int *err);
void display_close(struct display *d);

static const struct x86_env env = {
	.in = hw_in,
	.out = hw_out,
	.vga_rd = vga_rd,
	.vga_wr = vga_wr,
	.hle = bios_hle,
};

static const struct { const char *name; long ipms; } speeds[] = {
	{ "xt", 330 },		/* a 4.77 MHz 8088 */
	{ "at", 1500 },		/* a 12 MHz 286 */
	{ "386", 6000 },	/* a 386DX-33 */
	{ "486", 25000 },	/* a 486DX2-66 */
	{ "pentium", 90000 },
	{ "max", 0 },
};

long dosbox_speed_for(const char *name)
{
	for (size_t i = 0; i < NELEM(speeds); i++)
		if (!strcasecmp(name, speeds[i].name))
			return speeds[i].ipms;
	long v = strtol(name, NULL, 10);	/* or a number: instructions per ms */
	return v > 0 ? v : 25000;
}

/* ---- the screen ---------------------------------------------------------------------- */

void dosbox_text_dirty(struct dosbox *b)
{
	b->vga.dirty = 1;
}

/* teletype output while the program is just printing: to the console as text */
void dosbox_stream_char(struct dosbox *b, uint8_t ch)
{
	if (!b->stream || b->text_live || b->disp)
		return;
	if (ch == '\r')
		return;			/* the console's newline does both */
	if (ch == 7 || ch == 8 || ch == '\n' || ch == '\t' || (ch >= 0x20 && ch != 0x7F)) {
		if (ch >= 0x80 || ch < 0x20) {
			if (ch == '\n' || ch == '\b' || ch == '\t' || ch == 7)
				con_putc((char)ch);
			else
				con_rawchar(ch);
		} else {
			con_putc((char)ch);
		}
	} else {
		con_rawchar(ch);
	}
}

/* the program is drawing the whole screen: mirror the text page from now on */
void dosbox_screen_mode(struct dosbox *b)
{
	if (b->text_live || !b->stream)
		return;
	b->text_live = 1;
	memset(b->shown, 0xFF, sizeof b->shown);	/* everything differs: paint it all */
	bkl_enter();
	con_attr(7, 0);
	con_clear();
	bkl_leave();
}

static void text_sync(struct dosbox *b)
{
	int cols, rows, ccols, crows;
	vga_text_size(b, &cols, &rows);
	bkl_enter();
	con_size(&ccols, &crows);
	cols = MIN(cols, MIN(ccols, 132));
	rows = MIN(rows, MIN(crows, 60));
	uint32_t base = (b->vga.mode == 7 ? 0xB0000 : 0xB8000) + mw(b, 0x44E);
	int stride = mw(b, 0x44A);
	if (stride < cols)
		stride = cols;
	int lastx = -2, lasty = -1, lastattr = -1;
	for (int y = 0; y < rows; y++)
		for (int x = 0; x < cols; x++) {
			uint32_t a = base + (uint32_t)(y * stride + x) * 2;
			uint8_t ch = mb(b, a), at = mb(b, a + 1);
			int i = (y * 132 + x) * 2;
			if (b->shown[i] == ch && b->shown[i + 1] == at)
				continue;
			b->shown[i] = ch;
			b->shown[i + 1] = at;
			if (y != lasty || x != lastx + 1)
				con_gotoxy(x, y);
			if (at != lastattr) {
				int bg = (at >> 4) & ((b->vga.attr[0x10] & 8) ? 7 : 15);
				con_attr(at & 15, bg);
				lastattr = at;
			}
			con_rawchar(ch ? ch : ' ');
			lastx = x;
			lasty = y;
		}
	int cx, cy;
	uint16_t pos = mw(b, 0x450 + (uint32_t)(mb(b, 0x462) & 7) * 2);
	cx = pos & 0xFF;
	cy = pos >> 8;
	if (cx < cols && cy < rows)
		con_gotoxy(cx, cy);
	con_cursor(!(b->vga.crtc[0x0A] & 0x20));
	bkl_leave();
}

/* dos.trace=1: the road to a crash, and the code where it happened */
void dosbox_crash_dump(struct dosbox *b, uint16_t cs, uint32_t ip)
{
	if (!b->cpu.trace)
		return;
	kprintf("\nLast instructions:");
	for (unsigned k = 0; k < 64; k++) {
		unsigned i = (b->cpu.hist_n + k) % 64;
		if (k % 6 == 0)
			kprintf("\n");
		kprintf(" %04X:%04X", b->cpu.hist[i].cs, b->cpu.hist[i].ip);
	}
	uint32_t at = (uint32_t)cs * 16 + (ip & 0xFFFF);
	for (int k = -96; k < 32; k++) {
		if ((k + 96) % 16 == 0)
			kprintf("\n%04X:%04X ", cs, (ip + (uint32_t)k) & 0xFFFF);
		kprintf(" %02X", x86_rd8(&b->cpu, at + (uint32_t)k));
	}
	struct x86 *c = &b->cpu;
	kprintf("\nEAX=%08X EBX=%08X ECX=%08X EDX=%08X ESI=%08X EDI=%08X EBP=%08X ESP=%08X\n"
	    "DS=%04X ES=%04X SS=%04X FS=%04X GS=%04X\n",
	    c->r[0], c->r[3], c->r[1], c->r[2], c->r[6], c->r[7], c->r[5], c->r[4],
	    c->s[S_DS].sel, c->s[S_ES].sel, c->s[S_SS].sel, c->s[S_FS].sel, c->s[S_GS].sel);
}

/* graphics: show the VGA in a display, opening one if need be */
void dosbox_set_graphics(struct dosbox *b, int on)
{
	if (on && !b->disp && !b->disp_failed) {
		int err = 0;
		bkl_enter();
		struct display *d = display_open(console_unwrap(b->con), &err);
		if (d && d->attach)
			d->attach(d, NULL);
		bkl_leave();
		if (!d) {
			b->disp_failed = 1;
			if (!b->said_no_graphics) {
				b->said_no_graphics = 1;
				kprintf("\r\n[This program uses graphics: run it on the screen or in a desktop window.]\r\n");
			}
			return;
		}
		b->disp = d;
		b->mouse_possible = 1;
		b->vga.dirty = 1;
		b->last_w = b->last_h = 0;
	} else if (!on && b->disp) {
		bkl_enter();
		display_close(b->disp);
		bkl_leave();
		b->disp = NULL;
		memset(b->shown, 0xFF, sizeof b->shown);
		if (b->stream) {
			b->text_live = 1;
			bkl_enter();
			con_attr(7, 0);
			con_clear();
			bkl_leave();
		}
	}
}

static void frame(struct dosbox *b)
{
	int gfx = !vga_is_text(b) || (b->mouse.on && b->mouse.shown > 0 && b->disp);
	if (!vga_is_text(b) && !b->disp && !b->disp_failed)
		dosbox_set_graphics(b, 1);
	if (vga_is_text(b) && b->disp && !(b->mouse.on && b->mouse_wanted))
		dosbox_set_graphics(b, 0);
	if (b->disp) {
		struct display *d = b->disp;
		int w, h;
		vga_screen_size(b, &w, &h);
		if (w != b->last_w || h != b->last_h) {
			/* a new mode: ask for a window the right size (2x for low resolutions) */
			int sw = w, sh = h;
			if (w <= 360)
				sw *= 2, sh *= 2;
			else if (h <= 240)
				sh *= 2;
			bkl_enter();
			display_want_size(d, sw, sh);
			bkl_leave();
			b->last_w = w;
			b->last_h = h;
			b->vga.dirty = 1;
		}
		int text_blink = vga_is_text(b);
		if (!b->vga.dirty && !text_blink && !(b->mouse.on && b->mouse.shown > 0))
			return;
		bkl_enter();
		display_lock(d);	/* the pixels may be a window's on a desktop */
		if (d->alive && d->surf && d->surf->px) {
			struct rect area = { 0, 0, d->surf->w, d->surf->h };
			vga_render(b, d->surf, area);
			mouse_draw(b, d->surf, area);
			surf_touched(d->surf, area);	/* pixels, for a viewer that draws for itself */
			d->flush(d, area);
		}
		display_unlock(d);
		bkl_leave();
		(void)gfx;
		return;
	}
	if (b->cpu.vram_touched && !b->text_live && vga_is_text(b)) {
		b->cpu.vram_touched = 0;
		dosbox_screen_mode(b);
	}
	if (b->text_live && vga_is_text(b))
		text_sync(b);
	b->vga.dirty = 0;
}

/* ---- input -------------------------------------------------------------------------------- */

/* set-1 scancodes for an ASCII character, and whether it needs shift */
static int scan_for(int ch, int *shift)
{
	static const char lo[] = "\0\0331234567890-=\b\tqwertyuiop[]\n\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
	static const char hi[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\n\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
	*shift = 0;
	for (int i = 1; i < (int)sizeof lo - 1; i++)
		if (lo[i] == ch && ch)
			return i;
	for (int i = 1; i < (int)sizeof hi - 1; i++)
		if (hi[i] == ch && ch) {
			*shift = 1;
			return i;
		}
	return 0;
}

static void press(struct dosbox *b, int sc, int ext)
{
	if (ext)
		kbd_push(b, 0xE0);
	kbd_push(b, (uint8_t)sc);
	if (ext)
		kbd_push(b, 0xE0);
	kbd_push(b, (uint8_t)(sc | 0x80));
}

/* a key the console gave us, turned back into what the keyboard would have sent */
static void cooked_key(struct dosbox *b, int k)
{
	int sc = 0, ext = 0, shift = 0, ctrl = 0, alt = 0;
	switch (k) {
	case KEY_UP: sc = 0x48; ext = 1; break;
	case KEY_DOWN: sc = 0x50; ext = 1; break;
	case KEY_LEFT: sc = 0x4B; ext = 1; break;
	case KEY_RIGHT: sc = 0x4D; ext = 1; break;
	case KEY_HOME: sc = 0x47; ext = 1; break;
	case KEY_END: sc = 0x4F; ext = 1; break;
	case KEY_PGUP: sc = 0x49; ext = 1; break;
	case KEY_PGDN: sc = 0x51; ext = 1; break;
	case KEY_DEL: sc = 0x53; ext = 1; break;
	case KEY_INS: sc = 0x52; ext = 1; break;
	case KEY_SHIFT_TAB: sc = 0x0F; shift = 1; break;
	case '\n': sc = 0x1C; break;
	case 27: sc = 0x01; break;
	case '\b': sc = 0x0E; break;
	case '\t': sc = 0x0F; break;
	default:
		if (k >= KEY_F1 && k < KEY_F1 + 10)
			sc = 0x3B + (k - KEY_F1);
		else if (k == KEY_F1 + 10 || k == KEY_F1 + 11)
			sc = 0x57 + (k - KEY_F1 - 10);
		else if (k & KEY_ALT) {
			sc = scan_for(tolower(k & 0xFF), &shift);
			alt = 1;
			shift = 0;
		} else if (k >= 1 && k <= 26) {
			sc = scan_for('a' + k - 1, &shift);
			ctrl = 1;
		} else if (k > 0 && k < 0x80) {
			sc = scan_for(k, &shift);
		}
	}
	if (!sc)
		return;
	if (ctrl)
		kbd_push(b, 0x1D);
	if (alt)
		kbd_push(b, 0x38);
	if (shift)
		kbd_push(b, 0x2A);
	press(b, sc, ext);
	if (shift)
		kbd_push(b, 0xAA);
	if (alt)
		kbd_push(b, 0xB8);
	if (ctrl)
		kbd_push(b, 0x9D);
}

/* Ctrl+Alt+F11 / F12: freeze to the state file, or go back to it */
static void state_key(struct dosbox *b, int load)
{
	b->state_req = load ? 2 : 1;
}

/* raw make/break bytes: watch for our own keys, Ctrl+Alt+Del and the states */
static void raw_key(struct dosbox *b, uint8_t sc)
{
	if (sc == 0x1D) b->raw_ctrl = 1;
	else if (sc == 0x9D) b->raw_ctrl = 0;
	else if (sc == 0x38) b->raw_alt = 1;
	else if (sc == 0xB8) b->raw_alt = 0;
	else if (b->raw_ctrl && b->raw_alt && sc == 0x44 && b->debug_keys) {
		b->dump_req = 1;	/* Ctrl+Alt+F10 with dos.trace: the VGA's registers */
		return;
	} else if (b->raw_ctrl && b->raw_alt && sc == 0xC4 && b->debug_keys) {
		return;
	} else if (b->raw_ctrl && b->raw_alt && (sc == 0x53 || sc == 0x57 || sc == 0x58)) {
		if (sc == 0x53)
			b->want_quit = 1;
		else
			state_key(b, sc == 0x58);
		return;
	} else if (b->raw_ctrl && b->raw_alt && (sc == 0xD3 || sc == 0xD7 || sc == 0xD8)) {
		return;		/* their releases */
	}
	kbd_push(b, sc);
}

/* cooked keys can't say Ctrl+Alt, so ask the keyboard what's held */
static int cooked_special(struct dosbox *b, int k)
{
	if (k == KEY_F1 + 9 && b->debug_keys) {
		bkl_enter();
		int mods = input_mods();
		bkl_leave();
		if ((mods & 6) == 6) {
			b->dump_req = 1;
			return 1;
		}
	}
	if (k != KEY_F1 + 10 && k != KEY_F1 + 11)
		return 0;
	bkl_enter();
	int mods = input_mods();
	bkl_leave();
	if ((mods & 6) != 6)
		return 0;
	state_key(b, k == KEY_F1 + 11);
	return 1;
}

void dosbox_poll(struct dosbox *b)
{
	if (b->disp) {
		struct display *d = b->disp;
		struct event e;
		int n = 0;
		bkl_enter();
		int alive = d->alive;
		while (alive && n < 64 && d->next_event(d, &e)) {
			n++;
			switch (e.type) {
			case EV_RAWKEY:
				b->raw_seen = 1;
				raw_key(b, (uint8_t)e.key);
				break;
			case EV_KEY:
				if (e.key == KEY_EOF) {
					b->want_quit = 1;
					break;
				}
				if (!b->raw_seen && !cooked_special(b, e.key))
					cooked_key(b, e.key);
				break;
			case EV_CLOSE:
				b->want_quit = 1;
				break;
			case EV_RESIZE:		/* the window was resized: draw it all at the new size */
				b->vga.dirty = 1;
				break;
			case EV_MOVE: case EV_DOWN: case EV_UP: {
				/* window pixels to the program's mouse coordinates */
				int w, h;
				vga_screen_size(b, &w, &h);
				struct rect pic = vga_fit((struct rect){ 0, 0, d->surf->w, d->surf->h }, w, h);
				int x = (e.x - pic.x) * w / MAX(pic.w, 1), y = (e.y - pic.y) * h / MAX(pic.h, 1);
				x = MAX(0, MIN(w - 1, x));
				y = MAX(0, MIN(h - 1, y));
				int buttons = (e.buttons & 1) | ((e.buttons & 2)) | ((e.buttons & 4));
				mouse_event(b, x, y, buttons & 7);
				break;
			}
			}
		}
		if (!d->alive)
			alive = 0;
		bkl_leave();
		if (!alive)
			b->want_quit = 1;
		return;
	}
	for (int n = 0; n < 32; n++) {
		int k = con_pollkey();
		if (k < 0)
			break;
		if (k == KEY_EOF) {		/* the window closed */
			b->want_quit = 1;
			break;
		}
		if (!cooked_special(b, k))
			cooked_key(b, k);
	}
}

int dosbox_input_wait(struct dosbox *b)
{
	b->input_wait = 1;
	return 0;
}

/* ---- the loop ---------------------------------------------------------------------------- */

static void run(struct dosbox *b)
{
	struct x86 *c = &b->cpu;
	uint64_t next_frame = 0, next_poll = 0;
	b->ran = 0;
	b->lag_us = 0;
	b->g_last_wall = 0;
	b->t0_us = dos_now(b);
	while (b->running) {
		/*
		 * The guest's clock moves with the instructions it ran, at its pace;
		 * while it waits (HLT, a key, ahead of its pace) with the wall clock.
		 * So a host too slow for the pace slows the program down, rather
		 * than burying it in timer interrupts it can't keep up with (which
		 * starve the keyboard's: IRQ 0 always wins).
		 */
		uint64_t wall = uptime_us();
		if (b->speed && b->g_last_wall) {
			uint64_t dt = wall - b->g_last_wall, adv;
			if (b->g_idle) {
				adv = dt;
			} else {
				uint64_t t = (b->ran - b->g_last_ran) * 1000 + b->g_frac;
				adv = t / (uint64_t)b->speed;
				b->g_frac = t % (uint64_t)b->speed;
			}
			if (adv < dt)
				b->lag_us += dt - adv;
		}
		b->g_last_wall = wall;
		b->g_last_ran = b->ran;
		b->g_idle = 0;
		uint64_t now = dos_now(b);
		pit_tick(b, now);
		sb_poll(b, wall);	/* the sound card's DMA keeps real time, as a real one does */
		if (now >= next_poll || b->input_wait) {
			dosbox_poll(b);
			next_poll = now + 2000;
		}
		if (b->want_quit) {
			b->exit_code = 3;
			break;
		}
		if (b->dump_req) {
			b->dump_req = 0;
			struct vga *v = &b->vga;
			kprintf("\nVGA mode %02X misc %02X\nCRTC", v->mode, v->misc);
			for (int i = 0; i < 25; i++)
				kprintf(" %02X", v->crtc[i]);
			kprintf("\nSEQ ");
			for (int i = 0; i < 5; i++)
				kprintf(" %02X", v->seq[i]);
			kprintf("\nGC  ");
			for (int i = 0; i < 9; i++)
				kprintf(" %02X", v->gc[i]);
			kprintf("\nATTR");
			for (int i = 0; i < 21; i++)
				kprintf(" %02X", v->attr[i]);
			kprintf("\nPIC imr %02X irr %02X isr %02X  kbd q %d..%d status %02X cmd %02X port60 %02X  IF %d  INT9 %04X:%04X  INT16 %04X:%04X",
			    b->pic_imr[0], b->pic_irr[0], b->pic_isr[0], b->kbt, b->kbh, b->kbc_status, b->kbc_cmdbyte,
			    b->port60, (b->cpu.flags & FL_IF) != 0, mw(b, 9 * 4 + 2), mw(b, 9 * 4), mw(b, 0x16 * 4 + 2), mw(b, 0x16 * 4));
			kprintf("\nIRQs taken:");
			for (int i = 0; i < 256; i++)
				if (b->ints_taken[i])
					kprintf(" %02X:%u", i, b->ints_taken[i]);
			kprintf("  PIT0 reload %u mode %d", b->pit[0].reload, b->pit[0].mode);
			kprintf("\nclock: lag %llu us, ran %llu, speed %ld, state %d at %04X:%04X, in mouse handler %d",
			    (unsigned long long)b->lag_us, (unsigned long long)b->ran, b->speed, b->cpu.state,
			    b->cpu.s[S_CS].sel, b->cpu.eip, b->mouse.in_handler);
			struct mouse *ms = &b->mouse;
			kprintf("\nmouse on %d shown %d at %d,%d buttons %d range %d..%d x %d..%d handler %04X:%04X mask %X presses %d",
			    ms->on, ms->shown, ms->x, ms->y, ms->buttons, ms->minx, ms->maxx, ms->miny, ms->maxy,
			    ms->handler_seg, ms->handler_off, ms->handler_mask, ms->press_count[0]);
			kprintf("\nvram at %p\nnon-zero bytes per 2 KB of each plane:", (void *)V2P(v->vram));
			for (int p = 0; p < 4; p++) {
				kprintf("\n p%d", p);
				for (int k = 0; k < 32; k++) {
					int n = 0;
					for (int i = 0; i < 2048; i++)
						n += v->vram[(size_t)p * VGA_PLANE + (size_t)k * 2048 + (size_t)i] != 0;
					kprintf(" %d", n);
				}
			}
			kprintf("\nDAC");
			for (int i = 0; i < 64; i++)
				kprintf(" %02X%02X%02X", v->dac[i][0], v->dac[i][1], v->dac[i][2]);
			kprintf("\n");
			dosbox_crash_dump(b, b->cpu.s[S_CS].sel, b->cpu.eip);
		}
		if (b->state_req) {
			int load = b->state_req == 2;
			b->state_req = 0;
			if (load) {
				char file[VFS_PATH_MAX];
				dosbox_state_name(b->prog, file, sizeof file);
				dosbox_load_state(b, file);
			} else {
				dosbox_save_state(b);
			}
			continue;
		}
		/* release synthesized keys, one byte per read, as an 8042 does */
		kbd_pump(b);
		/* interrupts */
		int took = 0;
		if (x86_irq_ok(c)) {
			int v = pic_pending(b);
			if (v >= 0) {
				b->ints_taken[v & 0xFF]++;
				x86_interrupt(c, v);
				took = 1;
				b->input_wait = 0;
			} else if (mouse_callback(b)) {
				took = 1;
			}
		}
		if (c->state == XS_TRIPLE && took) {
			snprintf(b->errbuf, sizeof b->errbuf, "The program crashed the CPU at %04X:%04X",
			    c->s[S_CS].sel, c->eip & 0xFFFF);
			b->errmsg = b->errbuf;
			break;
		}
		/* how much to run */
		long budget = 20000;
		if (b->speed) {
			uint64_t allowed = (now - b->t0_us) * (uint64_t)b->speed / 1000;
			if (b->ran >= allowed) {
				/* ahead of the clock: wait for the next millisecond */
				goto idle;
			}
			if (allowed - b->ran > (uint64_t)b->speed * 50) {
				/* we were held up (an EXEC, a pause): don't race to catch up */
				b->t0_us = now - b->ran * 1000 / (uint64_t)b->speed;
				allowed = b->ran + (uint64_t)b->speed;
			}
			budget = (long)MIN(allowed - b->ran, (uint64_t)b->speed);
			if (budget < 64)
				budget = 64;
		}
		/* with a fast timer (music, digitised sound), come back in time for it */
		uint64_t period = pit_irq0_period_us(b);
		if (period < 1000) {
			long per = b->speed ? (long)((uint64_t)b->speed * period / 1000) : (long)(period * 30);
			if (per < budget)
				budget = MAX(per, 32);
		}
		b->input_wait = 0;
		c->irq_waiting = pic_waiting(b);
		long ran = x86_run(c, budget);
		b->ran += (uint64_t)ran;
		switch (c->state) {
		case XS_TRIPLE:
			snprintf(b->errbuf, sizeof b->errbuf, "The program crashed the CPU at %04X:%04X",
			    c->s[S_CS].sel, c->ip0 & 0xFFFF);
			b->errmsg = b->errbuf;
			b->running = 0;
			break;
		case XS_HALT:
			/* HLT: nothing to do till an interrupt */
			if (!(c->flags & FL_IF)) {
				snprintf(b->errbuf, sizeof b->errbuf, "The program halted the CPU");
				b->errmsg = b->errbuf;
				b->running = 0;
				break;
			}
			goto idle;
		case XS_WAIT:
			if (b->input_wait)
				goto idle;
			break;
		}
		if (b->idle_polls > 400) {
			/* polling for a key that isn't coming: let the core rest */
			b->idle_polls = 0;
			goto idle;
		}
		goto shown;
idle:
		b->g_idle = 1;		/* waiting: the guest's time goes by meanwhile */
		if (b->text_live || b->disp || 1) {
			now = uptime_us();
			if (now >= next_frame) {
				frame(b);
				next_frame = now + 16667;
			}
		}
		task_idle();
		continue;
shown:
		now = uptime_us();
		if (now >= next_frame) {
			frame(b);
			next_frame = now + 16667;
			task_yield();
		}
	}
	frame(b);
}

static void hw_quiet(struct dosbox *b)
{
	if (b->speaker_hz) {
		bkl_enter();
		arch_speaker(0);
		bkl_leave();
	}
}

/* COMSPEC programs, file properties: the speed a program asked for */
static void scale_setting(const char *key)
{
	extern int vga_scale_mode;
	if (*key && strcmp(key, "dos.scale"))
		return;
	const char *s = setting("dos.scale");
	vga_scale_mode = !strcasecmp(s, "stretch") ? 2 : !strcasecmp(s, "smooth") ? 1 : 0;
}

static long speed_for_file(const char *path)
{
	static int watching;
	if (!watching) {
		watching = 1;
		bkl_enter();
		setting_watch(scale_setting);
		scale_setting("");
		bkl_leave();
	}
	char v[32];
	if (task_current()->dos_speed[0])	/* a Program Manager item said how fast */
		return dosbox_speed_for(task_current()->dos_speed);
	if (!vfs_xattr_get(path, "dos.speed", v, sizeof v) && v[0])
		return dosbox_speed_for(v);
	bkl_enter();
	const char *s = setting("dos.speed");
	char copy[32];
	strncpy0(copy, s, sizeof copy);
	bkl_leave();
	return dosbox_speed_for(copy);
}

static int last_exit;

static int dosbox_run(const char *path, int argc, char **argv, const char *state)
{
	struct dosbox *b = kzalloc(sizeof *b);
	if (!b) {
		kprintf("Not enough memory\n");
		return 1;
	}
	uint32_t sizes[] = { DOS_MEM_DEFAULT, 8u << 20, 4u << 20, 2u << 20 };
	for (size_t i = 0; i < NELEM(sizes) && !b->cpu.mem; i++) {
		b->cpu.mem = kzalloc(sizes[i]);
		b->cpu.mem_size = sizes[i];
	}
	if (!b->cpu.mem || vga_init(b)) {
		kfree(b->cpu.mem);
		kfree(b);
		kprintf("Not enough memory for a DOS machine\n");
		return 1;
	}
	struct x86 *c = &b->cpu;
	uint32_t memsz = c->mem_size;
	uint8_t *mem = c->mem;
	x86_reset(c);
	c->mem = mem;
	c->mem_size = memsz;
	c->env = &env;
	c->box = b;
	b->task = task_current();
	b->con = b->task->con;
	b->stream = 1;
	b->speed = speed_for_file(path);
	int tr = setting_int("dos.trace");
	b->cpu.trace = tr == 1 || tr == 3;
	b->debug_keys = tr != 0;
	b->log_dos = tr == 3;
	strncpy0(b->prog, path, sizeof b->prog);
	hw_reset(b);
	vga_set_mode(b, 3, 0);
	for (uint32_t i = 0; i < 4000; i += 2) {
		mbw(b, 0xB8000 + i, ' ');
		mbw(b, 0xB8000 + i + 1, 7);
	}
	bios_init(b);
	dos_init(b);
	b->retcode = last_exit;
	char tail[130] = "";
	for (int i = 1; i < argc; i++) {
		strcat0(tail, " ", sizeof tail);
		strcat0(tail, argv[i], sizeof tail);
	}
	int r = state ? dosbox_load_state(b, state) : dos_load(b, path, tail, 0, 0);
	if (r && state) {
		kprintf("Can't resume from %s: %s\n", state, vfs_strerror(r));
		vga_free(b);
		kfree(c->mem);
		kfree(b);
		return 1;
	}
	if (r) {
		kprintf(r == 8 ? "Program too big to fit in memory\n" :
		    r == 11 ? "Bad or damaged program file\n" : "Can't load %s\n", path);
		vga_free(b);
		kfree(c->mem);
		kfree(b);
		return 1;
	}
	if (!state)
		b->first_psp = b->psp;
	b->running = 1;
	/* the cursor carries on from wherever bdos left it */
	int cx = 0, cy = 0;
	(void)cx;
	(void)cy;
	if (!state)
		x87_enter(c);
	run(b);
	x87_leave(c);
	x87_free(c);
	hw_quiet(b);
	if (b->disp)
		dosbox_set_graphics(b, 0);
	if (b->text_live) {
		bkl_enter();
		int cols, rows;
		con_size(&cols, &rows);
		con_attr(7, 0);
		uint16_t pos = mw(b, 0x450);
		con_gotoxy(0, MIN((pos >> 8) + 1, rows - 1));
		con_cursor(1);
		bkl_leave();
	}
	dos_close_all(b);
	if (b->errmsg) {
		kprintf("\n%s\n", b->errmsg);
	}
	int code = b->exit_code;
	last_exit = code;
	/* a program that changed drive or directory leaves bdos there, as DOS would */
	for (int i = 0; i < MAX_SEARCH; i++)
		kfree(b->search[i].ents);
	dpmi_free(b);
	sb_free(b);
	vga_free(b);
	kfree(c->mem);
	kfree(b);
	return code;
}

/* The DOS machine runs without the big kernel lock: on any core, in
 * parallel with everything else. Kernel calls go through gate.h. */
int com_run(const char *path, int argc, char **argv)
{
	int held = bkl_release_all();
	int r = dosbox_run(path, argc, argv, NULL);
	bkl_reacquire(held);
	return r;
}

/* RESUME NAME: carry on from C:\BDOS\STATES\NAME.BST */
int com_resume(const char *name)
{
	char file[VFS_PATH_MAX];
	if (strchr(name, '\\') || strchr(name, ':'))
		strncpy0(file, name, sizeof file);
	else
		dosbox_state_name(name, file, sizeof file);
	int held = bkl_release_all();
	char prog[VFS_PATH_MAX];
	snprintf(prog, sizeof prog, "%s", name);
	int r = dosbox_run(prog, 0, NULL, file);
	bkl_reacquire(held);
	return r;
}
