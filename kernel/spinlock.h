/*
 * spinlock.h - ticket spinlocks, portable (compiler atomics only).
 *
 * Ticket locks are fair: whoever started waiting first gets the lock next,
 * which matters for the big kernel lock, where a core spinning in a gate
 * must not be starved by a core that keeps re-taking it in the scheduler.
 */
#ifndef BDOS_SPINLOCK_H
#define BDOS_SPINLOCK_H

#include <stdint.h>
#include "arch.h"

struct spinlock {
	volatile uint16_t next;
	volatile uint16_t owner;
};

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock(struct spinlock *l)
{
	uint16_t t = __atomic_fetch_add(&l->next, 1, __ATOMIC_RELAXED);
	while (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) != t)
		cpu_pause();
}

/* Only succeeds when nobody holds it and nobody is queued. */
static inline int spin_trylock(struct spinlock *l)
{
	uint16_t o = __atomic_load_n(&l->owner, __ATOMIC_ACQUIRE);
	uint16_t expect = o;
	return __atomic_compare_exchange_n(&l->next, &expect, (uint16_t)(o + 1), 0,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_unlock(struct spinlock *l)
{
	__atomic_store_n(&l->owner, (uint16_t)(l->owner + 1), __ATOMIC_RELEASE);
}

static inline int spin_is_locked(struct spinlock *l)
{
	return __atomic_load_n(&l->owner, __ATOMIC_RELAXED) != __atomic_load_n(&l->next, __ATOMIC_RELAXED);
}

/* With interrupts off on this core, for data an interrupt handler also touches. */
static inline uint64_t spin_lock_irqsave(struct spinlock *l)
{
	uint64_t f = irq_save();
	spin_lock(l);
	return f;
}

static inline void spin_unlock_irqrestore(struct spinlock *l, uint64_t f)
{
	spin_unlock(l);
	irq_restore(f);
}

#endif
