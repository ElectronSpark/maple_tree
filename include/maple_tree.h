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

/**
 * mt_init - Initialise an empty maple tree.
 * @mt: Pointer to the maple tree structure to initialise.
 *
 * Sets the root pointer to NULL and flags to 0.  When MT_CONFIG_LOCK
 * is defined, the per-tree lock is also initialised.
 *
 * Must be called before any other operation on the tree.
 */
static inline void mt_init(struct maple_tree *mt)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = 0;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/**
 * mt_init_flags - Initialise an empty maple tree with flags.
 * @mt:    Pointer to the maple tree structure.
 * @flags: Feature flags stored in mt->ma_flags.  Reserved for future use.
 */
static inline void mt_init_flags(struct maple_tree *mt, unsigned int flags)
{
    mt_rcu_assign_pointer(&mt->ma_root, NULL);
    mt->ma_flags = flags;
#ifdef MT_CONFIG_LOCK
    mt_lock_init(&mt->ma_lock);
#endif
}

/**
 * mt_empty - Check whether a maple tree contains any entries.
 * @mt: Pointer to the maple tree.
 *
 * Return: true if the tree has no entries, false otherwise.
 */
static inline bool mt_empty(const struct maple_tree *mt)
{
    return READ_ONCE(mt->ma_root) == NULL;
}

/* ====================================================================== */
/*  Locking helpers                                                        */
/* ====================================================================== */

#ifdef MT_CONFIG_LOCK

/**
 * mt_lock - Acquire the tree's internal lock.
 * @mt: Pointer to the maple tree.
 *
 * Only available when MT_CONFIG_LOCK is defined.  The simple API does
 * **not** call this automatically — the caller is responsible for
 * holding the lock around write operations, mirroring the Linux kernel
 * pattern where an external lock (e.g. mmap_lock) protects the tree.
 */
static inline void mt_lock(struct maple_tree *mt)
{
    mt_spin_lock(&mt->ma_lock);
}

/** mt_unlock - Release the tree's internal lock. */
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

/**
 * mt_rcu_lock / mt_rcu_unlock - Enter / leave an RCU read-side section.
 *
 * Wraps mt_rcu_read_lock() / mt_rcu_read_unlock() from the config
 * header.  Used internally by the RCU read-side helpers (mt_find,
 * mt_next, mt_prev).  Callers using the cursor API directly during
 * RCU reads should bracket their access with these.
 */
static inline void mt_rcu_lock(void)   { mt_rcu_read_lock(); }
static inline void mt_rcu_unlock(void) { mt_rcu_read_unlock(); }

#else

static inline void mt_rcu_lock(void)   {}
static inline void mt_rcu_unlock(void) {}

#endif

/* ====================================================================== */
/*  Simple API                                                             */
/* ====================================================================== */

/**
 * mtree_load - Look up the entry stored at @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to look up.
 *
 * Searches the tree for the entry whose range contains @index.
 * Safe to call concurrently with RCU readers when MT_CONFIG_RCU is
 * enabled; the caller must hold mt_rcu_lock() or equivalent.
 *
 * Return: The stored void* entry, or NULL if the index falls in a gap.
 */
void *mtree_load(struct maple_tree *mt, uint64_t index);

/**
 * mtree_store_range - Store @entry for every index in [@first, @last].
 * @mt:    Pointer to the maple tree.
 * @first: First index of the range (inclusive).
 * @last:  Last index of the range (inclusive).  Must be >= @first.
 * @entry: The value to associate with the range.  May be NULL (creates
 *         an explicit gap / punches a hole in an existing range).
 *
 * Inserts or overwrites the entry for the given index range.  If the
 * range overlaps existing entries, those entries are split or replaced
 * as needed.  Adjacent slots with the same pointer value are coalesced
 * automatically.
 *
 * Nodes are split when full; the tree grows in height as required.
 * After erasure-style stores (entry == NULL), underfull nodes are
 * rebalanced (merged or redistributed with siblings) and the tree
 * height may shrink.
 *
 * Write-side locking is the caller's responsibility.
 *
 * Return: 0 on success, -EINVAL if @first > @last, -ENOMEM on
 *         allocation failure.
 */
int   mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last,
                        void *entry);

/**
 * mtree_store - Store @entry at a single @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to store at.
 * @entry: The value to associate with @index.  May be NULL.
 *
 * Convenience wrapper around mtree_store_range() with first == last.
 *
 * Return: 0 on success, negative errno on failure.
 */
static inline int mtree_store(struct maple_tree *mt, uint64_t index,
                              void *entry)
{
    return mtree_store_range(mt, index, index, entry);
}

/**
 * mtree_erase - Remove the entry that covers @index.
 * @mt:    Pointer to the maple tree.
 * @index: An index within the entry's range.
 *
 * Removes the *entire* entry whose range contains @index — not just the
 * single index.  For example, if a range [10, 29] was stored and
 * mtree_erase(mt, 20) is called, the whole [10, 29] entry is removed.
 * To punch a single-index hole within a range, use
 * mtree_store_range(mt, idx, idx, NULL) instead.
 *
 * After removal, the affected leaf is coalesced and rebalanced if it
 * falls below the minimum occupancy threshold.  The tree height may
 * shrink if the root becomes a single-child internal node.
 *
 * Return: The previously stored entry (the void* pointer), or NULL if
 *         no entry covered @index.
 */
void *mtree_erase(struct maple_tree *mt, uint64_t index);

/**
 * mtree_destroy - Free all nodes in the tree.
 * @mt: Pointer to the maple tree.
 *
 * Recursively frees every node.  After return the tree is empty
 * (mt_empty() returns true).  Safe to call on an already-empty tree.
 * Does **not** free the user-supplied entry pointers — only the
 * internal node structures.
 *
 * The caller must ensure no concurrent readers or writers are active.
 */
void  mtree_destroy(struct maple_tree *mt);

/* ====================================================================== */
/*  Cursor API                                                             */
/* ====================================================================== */

/**
 * mas_walk - Walk the tree to the entry at mas->index.
 * @mas: Maple state cursor.  On entry, mas->index is the target index.
 *
 * Descends from the root to the leaf slot that covers mas->index.
 * On return, mas->node, mas->offset, mas->min, mas->max, and
 * mas->depth are updated to reflect the located position.
 *
 * Return: The entry at that position, or NULL if it is a gap.
 */
void *mas_walk(struct ma_state *mas);

/**
 * mas_store - Store an entry at [mas->index, mas->last].
 * @mas:   Maple state cursor with index/last set to the target range.
 * @entry: The value to store.  May be NULL.
 *
 * Equivalent to mtree_store_range(mas->tree, mas->index, mas->last, entry).
 *
 * Return: 0 on success, negative errno on failure.
 */
int   mas_store(struct ma_state *mas, void *entry);

/**
 * mas_erase - Erase the entry at mas->index.
 * @mas: Maple state cursor.
 *
 * Equivalent to mtree_erase(mas->tree, mas->index).
 *
 * Return: The previously stored entry, or NULL.
 */
void *mas_erase(struct ma_state *mas);

/**
 * mas_find - Find the next non-NULL entry at or after mas->index, up to @max.
 * @mas: Maple state cursor.  On entry, mas->index is the search start.
 *       On return, mas->index is advanced past the found entry (to
 *       mas->max + 1) so that repeated calls iterate forward.
 * @max: Upper index bound (inclusive).
 *
 * If the cursor has not yet been walked (mas->node == NULL), an
 * implicit mas_walk() is performed first.
 *
 * Return: The next non-NULL entry, or NULL if none exists in
 *         [mas->index, @max].
 */
void *mas_find(struct ma_state *mas, uint64_t max);

/**
 * mas_next - Advance to the next non-NULL entry after the current position.
 * @mas: Maple state cursor, already positioned (e.g. via mas_walk).
 * @max: Upper index bound (inclusive).
 *
 * Moves forward one slot at a time, ascending to parent/sibling nodes
 * as needed.  Updates mas->node, mas->offset, mas->min, and mas->max.
 *
 * Return: The next non-NULL entry, or NULL if none exists up to @max.
 */
void *mas_next(struct ma_state *mas, uint64_t max);

/**
 * mas_prev - Move to the previous non-NULL entry before the current position.
 * @mas: Maple state cursor, already positioned.
 * @min: Lower index bound (inclusive).
 *
 * Moves backward one slot at a time, ascending to parent/sibling nodes
 * as needed.
 *
 * Return: The previous non-NULL entry, or NULL if none exists down to @min.
 */
void *mas_prev(struct ma_state *mas, uint64_t min);

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

/**
 * mas_empty_area - Find the first (lowest-address) gap of at least @size.
 * @mas:  Maple state cursor.  On success, mas->index and mas->last are
 *        set to the found gap range [start, start + size - 1].
 * @min:  Minimum index to consider (inclusive).
 * @max:  Maximum index to consider (inclusive).
 * @size: Required gap size (number of contiguous NULL indices).
 *        Must be > 0.
 *
 * Searches forward through the tree for the first contiguous run of
 * @size NULL entries within [@min, @max].  Uses gap tracking in
 * internal nodes for O(log n) search when available.
 *
 * Return: 0 on success, -EBUSY if no suitable gap exists.
 */
int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max,
                   uint64_t size);

/**
 * mas_empty_area_rev - Find the last (highest-address) gap of at least @size.
 * @mas:  Maple state cursor.  On success, mas->index and mas->last are
 *        set to the found gap range.
 * @min:  Minimum index to consider (inclusive).
 * @max:  Maximum index to consider (inclusive).
 * @size: Required gap size.  Must be > 0.
 *
 * Like mas_empty_area() but searches backward, returning the
 * highest-address gap that satisfies the size requirement.
 *
 * Return: 0 on success, -EBUSY if no suitable gap exists.
 */
int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max,
                       uint64_t size);

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

/**
 * mt_find - Find the next non-NULL entry at or after *@index, up to @max.
 * @mt:    Pointer to the maple tree.
 * @index: Pointer to the search start index.  Updated on success to
 *         one past the end of the found entry's range (so that
 *         repeated calls iterate all entries).  Set to MAPLE_MAX if
 *         the entry extends to the end of the index space.
 * @max:   Upper index bound (inclusive).
 *
 * Acquires and releases the RCU read lock internally (when
 * MT_CONFIG_RCU is enabled).
 *
 * Return: The found entry, or NULL if no non-NULL entry exists in
 *         [*@index, @max].
 */
void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max);

/**
 * mt_next - Return the next non-NULL entry strictly after @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to search after (exclusive).
 * @max:   Upper index bound (inclusive).
 *
 * Return: The next entry, or NULL if @index >= @max or no entry exists
 *         in (@index, @max].
 */
void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max);

/**
 * mt_prev - Return the previous non-NULL entry strictly before @index.
 * @mt:    Pointer to the maple tree.
 * @index: The index to search before (exclusive).
 * @min:   Lower index bound (inclusive).
 *
 * Return: The previous entry, or NULL if @index <= @min or no entry
 *         exists in [@min, @index).
 */
void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min);

/* ====================================================================== */
/*  Debug                                                                  */
/* ====================================================================== */

/**
 * mt_dump_tree - Print the tree structure to stdout for debugging.
 * @mt: Pointer to the maple tree.
 *
 * Prints every node, its type (LEAF / INTERNAL), range, parent info,
 * slot contents, and raw pivots.  Output goes to stdout via printf().
 */
void mt_dump_tree(struct maple_tree *mt);

/* ====================================================================== */
/*  Locked convenience wrappers                                            */
/* ====================================================================== */

/**
 * The mtree_lock_* family acquires the tree's internal lock (when
 * MT_CONFIG_LOCK is defined) around each operation, providing a
 * serialised interface safe for concurrent use from multiple threads.
 *
 * Without MT_CONFIG_LOCK the lock calls compile to no-ops, so these
 * wrappers remain usable (and equivalent to the plain API).
 *
 * For callers that need to batch several operations atomically, use
 * mt_lock() / mt_unlock() manually around the plain API instead.
 */

/**
 * mtree_lock_store_range - Locked version of mtree_store_range().
 */
static inline int mtree_lock_store_range(struct maple_tree *mt,
                                         uint64_t first, uint64_t last,
                                         void *entry)
{
    mt_lock(mt);
    int ret = mtree_store_range(mt, first, last, entry);
    mt_unlock(mt);
    return ret;
}

/**
 * mtree_lock_store - Locked version of mtree_store().
 */
static inline int mtree_lock_store(struct maple_tree *mt, uint64_t index,
                                   void *entry)
{
    return mtree_lock_store_range(mt, index, index, entry);
}

/**
 * mtree_lock_load - Locked version of mtree_load().
 */
static inline void *mtree_lock_load(struct maple_tree *mt, uint64_t index)
{
    mt_lock(mt);
    void *entry = mtree_load(mt, index);
    mt_unlock(mt);
    return entry;
}

/**
 * mtree_lock_erase - Locked version of mtree_erase().
 */
static inline void *mtree_lock_erase(struct maple_tree *mt, uint64_t index)
{
    mt_lock(mt);
    void *entry = mtree_erase(mt, index);
    mt_unlock(mt);
    return entry;
}

/**
 * mtree_lock_destroy - Locked version of mtree_destroy().
 *
 * Acquires the lock, destroys all nodes, then unlocks.
 */
static inline void mtree_lock_destroy(struct maple_tree *mt)
{
    mt_lock(mt);
    mtree_destroy(mt);
    mt_unlock(mt);
}

/* ====================================================================== */
/*  Iteration macros                                                       */
/* ====================================================================== */

/**
 * mt_for_each - Iterate over all non-NULL entries in [0, @__max].
 * @__mt:    Pointer to the maple tree.
 * @__entry: Loop variable receiving each non-NULL entry (void *).
 * @__index: uint64_t variable used internally; after each iteration
 *           holds the next search index (one past the current entry).
 * @__max:   Upper index bound (inclusive).
 *
 * Uses mt_find() internally, so the RCU read lock is acquired and
 * released on each iteration when MT_CONFIG_RCU is enabled.
 *
 * Example:
 *   uint64_t idx = 0;
 *   void *entry;
 *   mt_for_each(&mt, entry, idx, MAPLE_MAX) {
 *       // process entry
 *   }
 */
#define mt_for_each(__mt, __entry, __index, __max)                          \
    for ((__index) = 0,                                                     \
         (__entry) = mt_find((__mt), &(__index), (__max));                   \
         (__entry) != NULL;                                                 \
         (__entry) = mt_find((__mt), &(__index), (__max)))

/**
 * mas_for_each - Iterate entries forward from the current cursor position.
 * @__mas:   Pointer to a struct ma_state (cursor).
 * @__entry: Loop variable receiving each non-NULL entry (void *).
 * @__max:   Upper index bound (inclusive).
 *
 * Calls mas_find() repeatedly.  The cursor must be initialised (e.g.
 * via MA_STATE()) before the first invocation.  The cursor is advanced
 * past each entry automatically.
 *
 * Example:
 *   MA_STATE(mas, &mt, 0, 0);
 *   void *entry;
 *   mas_for_each(&mas, entry, MAPLE_MAX) {
 *       // process entry
 *   }
 */
#define mas_for_each(__mas, __entry, __max)                                 \
    while (((__entry) = mas_find((__mas), (__max))) != NULL)

#endif /* MAPLE_TREE_H */
