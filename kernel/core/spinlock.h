#pragma once

/* Spinlock (Milestone 58): sincronizare intre nuclee. Foloseste operatii
 * atomice ale GCC (lock xchg) — nu are nevoie de libatomic pentru un int
 * aliniat. Pattern test-and-test-and-set: asteptam pe o citire relaxata si
 * abia cand pare liber incercam schimbul, ca sa nu plimbam linia de cache. */

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

static inline void spin_lock(spinlock_t *l)
{
    for (;;) {
        if (!__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
            return;
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
            __asm__ volatile("pause");
    }
}

static inline int spin_trylock(spinlock_t *l)
{
    return __atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE) == 0;
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
