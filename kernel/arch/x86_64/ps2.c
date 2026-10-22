/*
 * ps2.c - i8042 keyboard (scan code set 1) and mouse, interrupt driven.
 *
 * Keys and mouse packets go into the generic input queues (input.c).
 * USB keyboards reach this through the firmware's legacy emulation;
 * a native xHCI driver is future work.
 */
#include "../../kernel.h"

#define DATA	0x60
#define STATUS	0x64
#define CMD	0x64

static int shift, caps, ctrl, alt, extended;
static int mouse_ok;
static uint8_t mpkt[3];
static int mcount;

static const char map_lo[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
	'*', 0, ' ',
};

static const char map_hi[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
	'*', 0, ' ',
};

/* keys go through input_key_press so the kernel can do its own key repeat */
static int cur_id;
#define PRESS(k) input_key_press(cur_id, (k))

static void kbd_byte(uint8_t sc)
{
	if (sc == 0xFA || sc == 0xFE || sc == 0xEE || sc == 0x00 || sc == 0xFF)
		return;		/* acks and controller noise */
	if (sc == 0xE0) {
		extended = 1;
		return;
	}
	int release = sc & 0x80;
	sc &= 0x7F;
	cur_id = (extended ? 0x100 : 0) | sc;
	if (release)
		input_key_release(cur_id);

	if (extended) {
		extended = 0;
		int k = 0;
		switch (sc) {
		case 0x48: k = KEY_UP; break;
		case 0x50: k = KEY_DOWN; break;
		case 0x4B: k = KEY_LEFT; break;
		case 0x4D: k = KEY_RIGHT; break;
		case 0x47: k = KEY_HOME; break;
		case 0x4F: k = KEY_END; break;
		case 0x49: k = KEY_PGUP; break;
		case 0x51: k = KEY_PGDN; break;
		case 0x53: k = KEY_DEL; break;
		case 0x52: k = KEY_INS; break;
		case 0x1C: k = '\n'; break;
		case 0x35: k = '/'; break;
		case 0x1D: ctrl = !release; return;
		case 0x38: alt = !release; return;
		}
		if (k && !release)
			PRESS(k);
		return;
	}

	switch (sc) {
	case 0x2A:
	case 0x36:
		shift = !release;
		return;
	case 0x1D:
		ctrl = !release;
		return;
	case 0x38:
		alt = !release;
		return;
	case 0x3A:
		if (!release)
			caps = !caps;
		return;
	}
	if (release)
		return;
	if (sc >= 0x3B && sc <= 0x44) {		/* F1-F10 */
		PRESS(KEY_F1 + (sc - 0x3B));
		return;
	}
	if (sc == 0x57 || sc == 0x58) {		/* F11, F12 */
		PRESS(KEY_F1 + 10 + (sc - 0x57));
		return;
	}

	char c = shift ? map_hi[sc] : map_lo[sc];
	if (!c)
		return;
	if (c == '\t' && shift) {
		PRESS(KEY_SHIFT_TAB);
		return;
	}
	int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
	if (caps && letter)
		c ^= 0x20;
	if (ctrl && letter) {
		PRESS(toupper(c) - '@');	/* ^C = 3 and friends */
		return;
	}
	if (alt && letter) {
		PRESS(KEY_ALT | tolower(c));
		return;
	}
	PRESS((uint8_t)c);
}

static void mouse_byte(uint8_t b)
{
	if (mcount == 0 && !(b & 0x08))
		return;		/* out of sync: bit 3 is always set in byte 0 */
	mpkt[mcount++] = b;
	if (mcount < 3)
		return;
	mcount = 0;
	int dx = mpkt[1] - ((mpkt[0] & 0x10) ? 256 : 0);
	int dy = mpkt[2] - ((mpkt[0] & 0x20) ? 256 : 0);
	if (mpkt[0] & 0xC0)	/* overflow: drop */
		return;
	input_mouse(dx, -dy, mpkt[0] & 7);
}

/*
 * The VMware "vmmouse" backdoor, which QEMU and VirtualBox also provide:
 * an absolute pointer, so the guest pointer sits exactly under the host's
 * cursor (no drift, no grabbing). It still raises IRQ 12 with a dummy PS/2
 * packet; the real data comes through I/O port 0x5658.
 */
#define VMW_MAGIC	0x564D5868u
#define VMW_PORT	0x5658
#define VMW_GETVERSION	10
#define VMM_DATA	39
#define VMM_STATUS	40
#define VMM_COMMAND	41
#define VMM_READ_ID	0x45414552u
#define VMM_ABSOLUTE	0x53424152u
#define VMM_DISABLE	0x000000F5u
#define VMM_VERSION	0x3442554Au

static int vmmouse_on;

struct vmw_regs {
	uint32_t a, b, c, d;
};

static void vmw_call(uint32_t cmd, uint32_t arg, struct vmw_regs *r)
{
	uint32_t a = VMW_MAGIC, b = arg, c = cmd, d = VMW_PORT;
	__asm__ volatile("inl %%dx, %%eax" : "+a"(a), "+b"(b), "+c"(c), "+d"(d) : : "memory");
	r->a = a;
	r->b = b;
	r->c = c;
	r->d = d;
}

static int vmmouse_init(void)
{
	struct vmw_regs r;
	vmw_call(VMW_GETVERSION, ~VMW_MAGIC, &r);
	if (r.b != VMW_MAGIC || r.a == 0xFFFFFFFFu)
		return 0;		/* not a VMware-compatible hypervisor */
	vmw_call(VMM_COMMAND, VMM_READ_ID, &r);
	vmw_call(VMM_STATUS, 0, &r);
	if ((r.a & 0xFFFF) < 1 || (r.a & 0xFFFF0000u) == 0xFFFF0000u)
		return 0;
	vmw_call(VMM_DATA, 1, &r);
	if (r.a != VMM_VERSION) {
		vmw_call(VMM_COMMAND, VMM_DISABLE, &r);
		return 0;
	}
	vmw_call(VMM_COMMAND, VMM_ABSOLUTE, &r);
	return 1;
}

static void vmmouse_drain(void)
{
	for (int guard = 0; guard < 64; guard++) {
		struct vmw_regs r;
		vmw_call(VMM_STATUS, 0, &r);
		if ((r.a & 0xFFFF0000u) == 0xFFFF0000u) {	/* error: fall back to PS/2 */
			vmmouse_on = 0;
			return;
		}
		if ((r.a & 0xFFFF) < 4)
			return;
		vmw_call(VMM_DATA, 4, &r);
		int b = 0;
		if (r.a & 0x20)
			b |= 1;		/* left */
		if (r.a & 0x10)
			b |= 2;		/* right */
		if (r.a & 0x08)
			b |= 4;		/* middle */
		input_mouse_abs((int)(r.b & 0xFFFF), (int)(r.c & 0xFFFF), b);
	}
}

static void ps2_irq(void)
{
	int mouse_seen = 0;
	while (inb(STATUS) & 1) {
		uint8_t st = inb(STATUS);
		uint8_t b = inb(DATA);
		if (st & 0x20) {
			if (vmmouse_on)
				mouse_seen = 1;	/* the PS/2 bytes are only a doorbell */
			else
				mouse_byte(b);
		} else {
			kbd_byte(b);
		}
	}
	if (mouse_seen)
		vmmouse_drain();
}

static int wait_write(void)
{
	for (int i = 0; i < 100000; i++)
		if (!(inb(STATUS) & 2))
			return 1;
	return 0;
}

static int wait_read(void)
{
	for (int i = 0; i < 100000; i++)
		if (inb(STATUS) & 1)
			return 1;
	return 0;
}

static void ctl_cmd(uint8_t c)
{
	wait_write();
	outb(CMD, c);
}

static int mouse_cmd(uint8_t c)
{
	ctl_cmd(0xD4);		/* next byte goes to the mouse */
	wait_write();
	outb(DATA, c);
	if (!wait_read())
		return -1;
	return inb(DATA);	/* 0xFA = ACK */
}

void ps2_init(void)
{
	irq_disable();
	/* Drain anything the firmware left behind. */
	for (int i = 0; i < 64 && (inb(STATUS) & 1); i++)
		(void)inb(DATA);

	/* Controller config: enable IRQ1 and IRQ12, keep translation on. */
	ctl_cmd(0x20);
	uint8_t cfg = wait_read() ? inb(DATA) : 0x45;
	cfg |= 0x01 | 0x02 | 0x40;	/* kbd IRQ, mouse IRQ, set-1 translation */
	cfg &= ~0x30;			/* both clocks enabled */
	ctl_cmd(0x60);
	wait_write();
	outb(DATA, cfg);

	ctl_cmd(0xA8);			/* enable the aux (mouse) port */
	if (mouse_cmd(0xF6) == 0xFA && mouse_cmd(0xF4) == 0xFA)	/* defaults, stream on */
		mouse_ok = 1;
	if (mouse_ok)
		vmmouse_on = vmmouse_init();
	for (int i = 0; i < 64 && (inb(STATUS) & 1); i++)
		(void)inb(DATA);

	irq_register(IRQ_KBD, ps2_irq);
	irq_register(IRQ_MOUSE, ps2_irq);
	irq_enable();
}

int input_mods(void)
{
	return (shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0);
}

int ps2_mouse_present(void)
{
	return mouse_ok;
}
