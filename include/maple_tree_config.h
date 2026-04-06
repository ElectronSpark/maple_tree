/**
 * @file maple_tree_config.h
 * @brief Maple tree build-time configuration and pluggable interfaces.
 *
 * This header provides:
 *  1. Build-time feature toggles (locking, RCU).
 *  2. Pluggable memory allocation interface.
 *  3. Pluggable lock and RCU interfaces (when enabled).
 *  4. Architecture-aware memory barriers.
 *  5. Compiler helpers (READ_ONCE / WRITE_ONCE).
 *
 * Feature macros:
 *   MT_CONFIG_LOCK  — Enable internal lock in struct maple_tree.
 *   MT_CONFIG_RCU   — Enable RCU read-side lock-free lookups.
 */

#ifndef MAPLE_TREE_CONFIG_H
#define MAPLE_TREE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/* ====================================================================== */
/*  Feature toggles                                                        */
/* ====================================================================== */

/*
 * Define MT_CONFIG_LOCK to include a lock in struct maple_tree.
 * The simple API does NOT auto-lock; the caller manages locking externally,
 * matching the Linux kernel pattern (vm->rw_lock protects the tree).
 *
 * Define MT_CONFIG_RCU to wrap reads in RCU sections and defer node
 * freeing via mt_call_rcu().
 */

/* Uncomment or define via -D to enable:
 * #define MT_CONFIG_LOCK
 * #define MT_CONFIG_RCU
 */

/* ====================================================================== */
/*  Memory allocation interface                                            */
/* ====================================================================== */

#ifndef MT_CUSTOM_ALLOC

#include <stdlib.h>
#include <string.h>

static inline void *mt_alloc_fn(size_t size)
{
    /* Round up to alignment so aligned_alloc succeeds (C11 requirement). */
    size_t align = 256;
    size_t alloc_size = (size + align - 1) & ~(align - 1);
    void *p = aligned_alloc(align, alloc_size);
    if (p)
        memset(p, 0, alloc_size);
    return p;
}

static inline void mt_free_fn(void *ptr)
{
    free(ptr);
}

#else /* MT_CUSTOM_ALLOC */

extern void *mt_alloc_fn(size_t size);
extern void  mt_free_fn(void *ptr);

#endif /* MT_CUSTOM_ALLOC */

/* ====================================================================== */
/*  Lock interface                                                         */
/* ====================================================================== */

#ifdef MT_CONFIG_LOCK

#ifndef MT_CUSTOM_LOCK

#include <pthread.h>

typedef pthread_mutex_t mt_lock_t;

#define MT_LOCK_INITIALIZER  PTHREAD_MUTEX_INITIALIZER

static inline void mt_lock_init(mt_lock_t *lock)
{
    pthread_mutex_init(lock, NULL);
}

static inline void mt_spin_lock(mt_lock_t *lock)
{
    pthread_mutex_lock(lock);
}

static inline void mt_spin_unlock(mt_lock_t *lock)
{
    pthread_mutex_unlock(lock);
}

#endif /* MT_CUSTOM_LOCK */

#endif /* MT_CONFIG_LOCK */

/* ====================================================================== */
/*  RCU interface                                                          */
/* ====================================================================== */

#ifdef MT_CONFIG_RCU

#ifndef MT_CUSTOM_RCU

static inline void mt_rcu_read_lock(void) {}
static inline void mt_rcu_read_unlock(void) {}

typedef void (*mt_rcu_callback_t)(void *);

static inline void mt_call_rcu(mt_rcu_callback_t cb, void *data)
{
    cb(data);
}

#else /* MT_CUSTOM_RCU */

extern void mt_rcu_read_lock(void);
extern void mt_rcu_read_unlock(void);

typedef void (*mt_rcu_callback_t)(void *);
extern void mt_call_rcu(mt_rcu_callback_t cb, void *data);

#endif /* MT_CUSTOM_RCU */

#endif /* MT_CONFIG_RCU */

/* ====================================================================== */
/*  Memory barrier primitives                                              */
/* ====================================================================== */

#ifndef MT_CUSTOM_BARRIERS

#if defined(__x86_64__) || defined(__i386__)

#define mt_smp_rmb()  __asm__ volatile("" ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("" ::: "memory")
#define mt_smp_mb()   __asm__ volatile("mfence" ::: "memory")

#elif defined(__aarch64__)

#define mt_smp_rmb()  __asm__ volatile("dmb ishld" ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("dmb ishst" ::: "memory")
#define mt_smp_mb()   __asm__ volatile("dmb ish"   ::: "memory")

#elif defined(__riscv)

#define mt_smp_rmb()  __asm__ volatile("fence r,r"   ::: "memory")
#define mt_smp_wmb()  __asm__ volatile("fence w,w"   ::: "memory")
#define mt_smp_mb()   __asm__ volatile("fence rw,rw" ::: "memory")

#else

#define mt_smp_rmb()  __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define mt_smp_wmb()  __atomic_thread_fence(__ATOMIC_RELEASE)
#define mt_smp_mb()   __atomic_thread_fence(__ATOMIC_SEQ_CST)

#endif /* arch detection */

#endif /* MT_CUSTOM_BARRIERS */

/* ====================================================================== */
/*  Slot / pointer access with memory ordering                             */
/* ====================================================================== */

/*
 * READ_ONCE / WRITE_ONCE — prevent compiler from optimising away or
 * reordering accesses.  Used for RCU-safe reads of pivots and slots.
 */
#define READ_ONCE(x)   (*(const volatile __typeof__(x) *)&(x))
#define WRITE_ONCE(x, val) do { *(volatile __typeof__(x) *)&(x) = (val); } while (0)

/*
 * rcu_dereference / rcu_assign_pointer — ordered slot access.
 *
 * With RCU: volatile load + read barrier / write barrier + volatile store.
 * Without RCU: plain access.
 */
#ifdef MT_CONFIG_RCU

static inline void *mt_rcu_dereference(void *const *slot)
{
    void *p = *(void *const volatile *)slot;
    mt_smp_rmb();
    return p;
}

static inline void mt_rcu_assign_pointer(void **slot, void *val)
{
    mt_smp_wmb();
    *(void *volatile *)slot = val;
}

#else /* !MT_CONFIG_RCU */

static inline void *mt_rcu_dereference(void *const *slot)
{
    return *slot;
}

static inline void mt_rcu_assign_pointer(void **slot, void *val)
{
    *slot = val;
}

#endif /* MT_CONFIG_RCU */

#endif /* MAPLE_TREE_CONFIG_H */
