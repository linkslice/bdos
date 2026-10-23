/*
 * smp.c - finding and starting the other cores (x86-64).
 *
 * The ACPI MADT lists every core's local APIC. Each one is woken with the
 * INIT/SIPI/SIPI dance into the trampoline (tramp.S), which brings it up
 * to long mode and into ap_entry(). Each core then gets its own GDT, TSS,
 * GS (its struct cpu) and a local APIC timer ticking at 1 kHz, and joins
 * the scheduler.
 *
 * Device interrupts all still go to core 0 through the 8259s, as they
 * always have; the other cores only ever hear their own timers.
 */
#include "../../kernel.h"

#define TRAMP_BASE 0x8000

/* local APIC registers */
#define LAPIC_ID	0x020
#define LAPIC_EOI	0x0B0
#define LAPIC_SVR	0x0F0
#define LAPIC_ICR_LO	0x300
#define LAPIC_ICR_HI	0x310
#define LAPIC_TIMER	0x320
#define LAPIC_LINT0	0x350
#define LAPIC_LINT1	0x360
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR	0x390
#define LAPIC_TIMER_DIV	0x3E0

static volatile uint32_t *lapic;
static int x2apic;
static uint32_t timer_count;	/* LAPIC timer counts per ms, divide by 16 */

static uint32_t lapic_read(uint32_t reg)
{
	if (x2apic)
		return (uint32_t)rdmsr(0x800 + (reg >> 4));
	return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v)
{
	if (x2apic)
		wrmsr(0x800 + (reg >> 4), v);
	else
		lapic[reg / 4] = v;
}

void lapic_eoi(void)
{
	lapic_write(LAPIC_EOI, 0);
}

static uint32_t lapic_id(void)
{
	uint32_t v = lapic_read(LAPIC_ID);
	return x2apic ? v : v >> 24;
}

static void send_ipi(uint32_t dest, uint32_t lo)
{
	if (x2apic) {
		wrmsr(0x830, ((uint64_t)dest << 32) | lo);
		return;
	}
	lapic_write(LAPIC_ICR_HI, dest << 24);
	lapic_write(LAPIC_ICR_LO, lo);
	for (int i = 0; i < 1000000 && (lapic_read(LAPIC_ICR_LO) & (1 << 12)); i++)
		cpu_pause();
}

static void lapic_timer_start(void)
{
	lapic_write(LAPIC_TIMER_DIV, 0x3);			/* divide by 16 */
	lapic_write(LAPIC_TIMER, (1 << 17) | VEC_TIMER);	/* periodic */
	lapic_write(LAPIC_TIMER_INIT, timer_count);
}

/* ---- ACPI: which cores are there? --------------------------------------- */

struct sdt {
	char sig[4];
	uint32_t len;
	uint8_t rev, sum;
	char oem[6], table[8];
	uint32_t oemrev, creator, creatorrev;
} __attribute__((packed));

static const struct sdt *find_table(const char *sig)
{
	uint64_t rsdp = bootinfo->acpi_rsdp;
	if (!rsdp || memcmp((void *)rsdp, "RSD PTR ", 8))
		return NULL;
	const uint8_t *r = (const uint8_t *)rsdp;
	int xsdt = r[15] >= 2 && *(const uint64_t *)(r + 24);
	const struct sdt *root = xsdt ? (const struct sdt *)*(const uint64_t *)(r + 24) :
	    (const struct sdt *)(uint64_t)*(const uint32_t *)(r + 16);
	int esz = xsdt ? 8 : 4;
	int n = (int)(root->len - sizeof *root) / esz;
	const uint8_t *ents = (const uint8_t *)(root + 1);
	for (int i = 0; i < n; i++) {
		uint64_t a = xsdt ? *(const uint64_t *)(ents + i * 8) : *(const uint32_t *)(ents + i * 4);
		const struct sdt *t = (const struct sdt *)a;
		if (t && !memcmp(t->sig, sig, 4))
			return t;
	}
	return NULL;
}

static int madt_cores(uint32_t *ids, int max)
{
	const struct sdt *m = find_table("APIC");
	if (!m)
		return 0;
	int n = 0;
	const uint8_t *p = (const uint8_t *)m + 44, *end = (const uint8_t *)m + m->len;
	while (p + 2 <= end && p[1] >= 2) {
		if (p[0] == 0 && (p[4] & 1) && n < max)		/* processor local APIC, enabled */
			ids[n++] = p[3];
		else if (p[0] == 9 && (*(const uint32_t *)(p + 8) & 1) && n < max)	/* x2APIC */
			ids[n++] = *(const uint32_t *)(p + 4);
		p += p[1];
	}
	return n;
}

/* ---- bringing a core up --------------------------------------------------- */

extern char smp_tramp_start[], smp_tramp_end[];
extern char tramp_cr3[], tramp_stack[], tramp_entry[], tramp_arg[];
void arch_ap_setup(struct cpu *c);
uint64_t arch_cr3(void);

#define TV(sym) (*(volatile uint64_t *)(TRAMP_BASE + (sym - smp_tramp_start)))

static void ap_entry(struct cpu *c)
{
	arch_ap_setup(c);
	lapic_write(LAPIC_SVR, 0x100 | 0xFF);
	lapic_write(LAPIC_LINT0, 0x10000);	/* the 8259s only talk to core 0 */
	lapic_write(LAPIC_LINT1, 0x400);	/* NMI */
	lapic_timer_start();
	sched_start_cpu();
}

static void delay_ms(uint64_t ms)
{
	uint64_t until = uptime_ms() + ms;
	while (uptime_ms() < until)
		cpu_pause();
}

static int tramp_page_free(void)
{
	for (uint32_t i = 0; i < bootinfo->nranges; i++) {
		const struct bdos_memrange *r = &bootinfo->ranges[i];
		if (r->type == BDOS_MEM_FREE && r->base <= TRAMP_BASE &&
		    r->base + r->pages * PAGE_SIZE >= TRAMP_BASE + PAGE_SIZE)
			return 1;
	}
	return 0;
}

static void calibrate_timer(void)
{
	lapic_write(LAPIC_TIMER_DIV, 0x3);
	lapic_write(LAPIC_TIMER, 0x10000 | VEC_TIMER);		/* masked, one-shot */
	uint64_t t0 = uptime_ms();
	while (uptime_ms() == t0)
		cpu_pause();
	lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
	t0 = uptime_ms();
	while (uptime_ms() < t0 + 20)
		cpu_pause();
	uint32_t used = 0xFFFFFFFF - lapic_read(LAPIC_TIMER_CUR);
	lapic_write(LAPIC_TIMER_INIT, 0);
	timer_count = used / 20;
	if (timer_count < 100)
		timer_count = 100000;
}

int arch_smp_init(void)
{
	uint64_t base = rdmsr(0x1B);
	if (!(base & (1 << 11)))
		return 1;			/* no local APIC: one core */
	x2apic = (base & (1 << 10)) != 0;
	lapic = (volatile uint32_t *)(base & ~0xFFFULL);
	cpus[0].hwid = lapic_id();

	uint32_t ids[MAX_CPUS];
	int n = madt_cores(ids, MAX_CPUS);
	if (n <= 1 || !tramp_page_free() || arch_cr3() >= (1ULL << 32))
		return 1;
	calibrate_timer();

	memcpy((void *)TRAMP_BASE, smp_tramp_start, (size_t)(smp_tramp_end - smp_tramp_start));
	int next = 1;
	for (int i = 0; i < n && next < MAX_CPUS; i++) {
		if (ids[i] == cpus[0].hwid)
			continue;
		struct cpu *c = &cpus[next];
		c->id = next;
		c->hwid = ids[i];
		void *stack = pmm_alloc(4);
		if (!stack)
			break;
		TV(tramp_cr3) = arch_cr3();
		TV(tramp_stack) = (uint64_t)stack + 4 * PAGE_SIZE;
		TV(tramp_entry) = (uint64_t)ap_entry;
		TV(tramp_arg) = (uint64_t)c;
		ncpus = next + 1;	/* so its struct is counted once it's up */
		send_ipi(c->hwid, 0x4500);			/* INIT */
		delay_ms(10);
		send_ipi(c->hwid, 0x4600 | (TRAMP_BASE >> 12));	/* SIPI */
		delay_us(200);
		if (!c->online)
			send_ipi(c->hwid, 0x4600 | (TRAMP_BASE >> 12));
		for (int w = 0; w < 200 && !c->online; w++)
			delay_ms(1);
		if (!c->online) {
			ncpus = next;
			pmm_free(stack, 4);
			continue;
		}
		next++;
	}
	ncpus = next;
	return ncpus;
}

void arch_kick_cpu(int id)
{
	if (lapic && id >= 0 && id < ncpus && cpus[id].online)
		send_ipi(cpus[id].hwid, VEC_KICK);
}

void arch_halt_others(void)
{
	if (!lapic || ncpus < 2)
		return;
	send_ipi(0, 0xC0000 | 0x4000 | VEC_HALT);	/* all excluding self */
}
