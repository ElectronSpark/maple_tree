/**
 * @file maple_tree.h
 * @brief Maple tree public API — Linux-style RCU-safe B-tree for ranges.
 *
 * Usage patterns:
 *
 *   1. Simple API (caller manages external locking for writes):
 *        mtree_store_range(mt, first, last, entry)
 *        mtree_store(mt, index, entry)
 *        mtree_load(mt, index)
 *        mtree_erase(mt, index)
 *        mtree_destroy(mt)
 *
 *   2. Cursor API:
 *        MA_STATE(mas, mt, index, last);
 *        mas_walk(&mas)
 *        mas_store(&mas, entry)
 *        mas_erase(&mas)
 *        mas_find(&mas, max)
 *        mas_next(&mas, max)
 *        mas_prev(&mas, min)
 *
 *   3. RCU read-side helpers:
 *        mt_find(mt, &index, max)
 *        mt_next(mt, index, max)
 *        mt_prev(mt, index, min)
 *
 *   4. Gap search:
 *        mas_empty_area(&mas, min, max, size)
 *        mas_empty_area_rev(&mas, min, max, size)
 *
 *   5. Iteration:
 *        mt_for_each(mt, entry, index, max)
 *        mas_for_each(mas, entry, max)
 */

#ifndef MAPLE_TREE_H
#define MAPLE_TREE_H

#include "maple_tree_type.h"

/* ====================================================================== */
/*  Initialisation                                                         */
/* ====================================================================== */

/** Initialise an empty maple tree. */
static inline void mt_init(struct maple_tree *mt)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = 0;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/** Initialise an empty maple tree with flags. */
static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = flags;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/** Check whether a maple tree is empty. */
static inline bool mt_empty(const struct maple_tree *mt)
{
    return READ_ONCE(mt->ma_root) == NULL;
}

/* ====================================================================== */
/*  Locking helpers                                                        */
/* ====================================================================== */

#ifdef MT_CONFIG_LOCK

static inline void mt_lock(struct maple_tree *mt)
{
    mt_spin_lock(&mt->ma_lock);
}

static inline void mt_unlock(struct maple_tree *mt)
{
    mt_spin_unlock(&mt->ma_lock);
}

#else /* !MT_CONFIG_LOCK */

static inline void mt_lock(struct maple_tree *mt)   { (void)mt; }
static inline void mt_unlock(struct maple_tree *mt) { (void)mt; }

#endif /* MT_CONFIG_LOCK */

/* ====================================================================== */
/*  RCU helpers                                                            */
/* ====================================================================== */

#ifdef MT_CONFIG_RCU

static inline void mt_rcu_lock(void)   { mt_rcu_read_lock(); }
static inline void mt_rcu_unlock(void) { mt_rcu_read_unlock(); }

#else

static inline void mt_rcu_lock(void)   {}
static inline void mt_rcu_unlock(void) {}

#endif

/* ====================================================================== */
/*  Simple API                                                             */
/* ====================================================================== */

void *mtree_load(struct maple_tree *mt, uint64_t index);

int   mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last,
                        void *entry);

static inline int mtree_store(struct maple_tree *mt, uint64_t index,
                              void *entry)
{
    return mtree_store_range(mt, index, index, entry);
}

void *mtree_erase(struct maple_tree *mt, uint64_t index);

void  mtree_destroy(struct maple_tree *mt);

/* ====================================================================== */
/*  Cursor API                                                             */
/* ====================================================================== */

void *mas_walk(struct ma_state *mas);
int   mas_store(struct ma_state *mas, void *entry);
void *mas_erase(struct ma_state *mas);
void *mas_find(struct ma_state *mas, uint64_t max);
void *mas_next(struct ma_state *mas, uint64_t max);
void *mas_prev(struct ma_state *mas, uint64_t min);

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max,
                   uint64_t size);
int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max,
                       uint64_t size);

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max);
void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max);
void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min);

/* ====================================================================== */
/*  Debug                                                                  */
/* ====================================================================== */

void mt_dump_tree(struct maple_tree *mt);

/* ====================================================================== */
/*  Iteration macros                                                       */
/* ====================================================================== */

/**
 * mt_for_each - Iterate over all non-NULL entries in [0, max].
 */
#define mt_for_each(__mt, __entry, __index, __max)                          \
    for ((__index) = 0,                                                     \
         (__entry) = mt_find((__mt), &(__index), (__max));                   \
         (__entry) != NULL;                                                 \
         (__entry) = mt_find((__mt), &(__index), (__max)))

/**
 * mas_for_each - Iterate entries from current MAS position.
 */
#define mas_for_each(__mas, __entry, __max)                                 \
    while (((__entry) = mas_find((__mas), (__max))) != NULL)

#endif /* MAPLE_TREE_H */
