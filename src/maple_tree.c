/**
 * @file maple_tree.c
 * @brief Maple tree implementation — RCU-safe B-tree for index ranges.
 *
 * Standalone C11 implementation based on the Linux kernel maple_tree.
 *
 * Design:
 *  - 16 slots per node, 15 pivots.
 *  - Leaf nodes store void* entries; internal nodes store child pointers.
 *  - Gap tracking in internal nodes for efficient free-range search.
 *  - Optional RCU-safe reads via mt_rcu_dereference / mt_rcu_assign_pointer.
 *  - Write-side externally locked.
 *  - Freed nodes optionally deferred via mt_call_rcu.
 */

#include <string.h>
#include <stdio.h>
#include <inttypes.h>

#include "maple_tree.h"

/* Error codes — only these are used internally. */
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef EBUSY
#define EBUSY 16
#endif

/* ====================================================================== */
/*  Node allocator                                                         */
/* ====================================================================== */

static struct maple_node *mt_alloc_node(void)
{
    struct maple_node *node = mt_alloc_fn(sizeof(struct maple_node));
    if (node == NULL)
        return NULL;
    /* mt_alloc_fn returns zeroed memory, but ensure key fields. */
    node->type = maple_leaf_64;
    node->slot_len = 0;
    node->parent = 0;
    return node;
}

static void mt_free_node_now(struct maple_node *node)
{
    if (node == NULL)
        return;
    mt_free_fn(node);
}

#ifdef MT_CONFIG_RCU
static void __mt_free_rcu_cb(void *data)
{
    mt_free_fn(data);
}

static void mt_free_node_rcu(struct maple_node *node)
{
    if (node == NULL)
        return;
    mt_call_rcu(__mt_free_rcu_cb, node);
}
#else
#define mt_free_node_rcu mt_free_node_now
#endif

/* ====================================================================== */
/*  Tagged-pointer helpers for ma_root                                     */
/* ====================================================================== */

static inline bool mt_is_node(const void *ptr)
{
    return ((uintptr_t)ptr & MAPLE_ROOT_NODE) != 0;
}

static inline struct maple_node *mt_to_node(const void *ptr)
{
    return (struct maple_node *)((uintptr_t)ptr & ~MAPLE_ROOT_NODE);
}

static inline void *mt_mk_root(struct maple_node *node)
{
    return (void *)((uintptr_t)node | MAPLE_ROOT_NODE);
}

/* ====================================================================== */
/*  Parent-pointer encoding                                                */
/* ====================================================================== */

#define MAPLE_PARENT_ROOT   0x01UL

static inline void mn_set_parent(struct maple_node *node,
                                 struct maple_node *parent, uint8_t slot)
{
    node->parent = (uint64_t)(uintptr_t)parent;
    node->parent_slot = slot;
}

static inline void mn_set_root(struct maple_node *node)
{
    node->parent = MAPLE_PARENT_ROOT;
    node->parent_slot = 0;
}

static inline bool mn_is_root(const struct maple_node *node)
{
    return (node->parent & MAPLE_PARENT_ROOT) != 0;
}

static inline struct maple_node *mn_get_parent(const struct maple_node *node)
{
    return (struct maple_node *)(uintptr_t)(node->parent & ~(uint64_t)0x1);
}

static inline uint8_t mn_get_parent_slot(const struct maple_node *node)
{
    return node->parent_slot;
}

static inline bool mn_is_leaf(const struct maple_node *node)
{
    return node->type == maple_leaf_64;
}

/* ====================================================================== */
/*  Pivot helpers                                                          */
/* ====================================================================== */

static inline uint64_t mn_pivot(const struct maple_node *node, uint8_t i,
                                 uint64_t node_max)
{
    if (i >= node->slot_len - 1)
        return node_max;
    return node->pivot[i];
}

static inline uint64_t mn_slot_min(const struct maple_node *node, uint8_t i,
                                    uint64_t node_min)
{
    if (i == 0)
        return node_min;
    return node->pivot[i - 1] + 1;
}

/* ====================================================================== */
/*  Pivot binary search                                                    */
/* ====================================================================== */

static inline uint8_t mn_find_slot(const struct maple_node *node,
                                    uint64_t index, uint8_t len)
{
    uint8_t lo = 0;
    uint8_t hi = len - 1;
    while (lo < hi) {
        uint8_t mid = (lo + hi) >> 1;
        uint64_t piv = READ_ONCE(node->pivot[mid]);
        if (index <= piv)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static inline uint8_t mn_find_slot_from(const struct maple_node *node,
                                         uint64_t index, uint8_t from,
                                         uint8_t len)
{
    uint8_t lo = from;
    uint8_t hi = len - 1;
    while (lo < hi) {
        uint8_t mid = (lo + hi) >> 1;
        uint64_t piv = READ_ONCE(node->pivot[mid]);
        if (index <= piv)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* ====================================================================== */
/*  Walk / lookup                                                          */
/* ====================================================================== */

static void *__mt_lookup(struct maple_tree *mt, uint64_t index)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return NULL;
    if (!mt_is_node(root))
        return root;

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (1) {
        uint8_t len = READ_ONCE(node->slot_len);
        if (len == 0)
            return NULL;

        uint8_t slot = mn_find_slot(node, index, len);
        void *entry = mt_rcu_dereference(&node->slot[slot]);

        if (mn_is_leaf(node))
            return entry;

        if (entry == NULL)
            return NULL;

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)entry;
    }
}

void *mtree_load(struct maple_tree *mt, uint64_t index)
{
    void *entry;
    mt_rcu_lock();
    entry = __mt_lookup(mt, index);
    mt_rcu_unlock();
    return entry;
}

/* ====================================================================== */
/*  MAS walk                                                               */
/* ====================================================================== */

void *mas_walk(struct ma_state *mas)
{
    struct maple_tree *mt = mas->tree;
    void *root = mt_rcu_dereference(&mt->ma_root);

    mas->depth = 0;

    if (root == NULL) {
        mas->node = NULL;
        return NULL;
    }

    if (!mt_is_node(root)) {
        mas->node = NULL;
        mas->min = 0;
        mas->max = MAPLE_MAX;
        mas->offset = 0;
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (1) {
        mas->depth++;
        uint8_t len = READ_ONCE(node->slot_len);
        if (len == 0) {
            mas->node = node;
            mas->min = node_min;
            mas->max = node_max;
            mas->offset = 0;
            return NULL;
        }

        uint8_t slot = mn_find_slot(node, mas->index, len);

        if (mn_is_leaf(node)) {
            void *entry = mt_rcu_dereference(&node->slot[slot]);
            mas->node = node;
            mas->min = mn_slot_min(node, slot, node_min);
            mas->max = mn_pivot(node, slot, node_max);
            mas->offset = slot;
            return entry;
        }

        void *child = mt_rcu_dereference(&node->slot[slot]);
        if (child == NULL) {
            mas->node = node;
            mas->min = mn_slot_min(node, slot, node_min);
            mas->max = mn_pivot(node, slot, node_max);
            mas->offset = slot;
            return NULL;
        }

        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);
        node = (struct maple_node *)child;
    }
}

/* ====================================================================== */
/*  Node splitting                                                         */
/* ====================================================================== */

static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                           uint64_t node_min, uint64_t node_max);

static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                               uint64_t *out_min, uint64_t *out_max);

static uint64_t __mt_range_size(uint64_t start, uint64_t end)
{
    if (end < start)
        return 0;
    if (start == 0 && end == MAPLE_MAX)
        return MAPLE_MAX;
    return end - start + 1;
}

static uint64_t __mt_node_max_gap(struct maple_node *node,
                                   uint64_t node_min, uint64_t node_max)
{
    if (node == NULL)
        return __mt_range_size(node_min, node_max);

    uint64_t max_gap = 0;

    if (mn_is_leaf(node)) {
        for (uint8_t i = 0; i < node->slot_len; i++) {
            if (node->slot[i] != NULL)
                continue;
            uint64_t slot_min = mn_slot_min(node, i, node_min);
            uint64_t slot_max = mn_pivot(node, i, node_max);
            uint64_t gap = __mt_range_size(slot_min, slot_max);
            if (gap > max_gap)
                max_gap = gap;
        }
        return max_gap;
    }

    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t slot_min = mn_slot_min(node, i, node_min);
        uint64_t slot_max = mn_pivot(node, i, node_max);
        struct maple_node *child = (struct maple_node *)node->slot[i];
        uint64_t gap;
        if (child == NULL) {
            gap = __mt_range_size(slot_min, slot_max);
        } else if (mn_is_leaf(child)) {
            gap = __mt_node_max_gap(child, slot_min, slot_max);
        } else {
            gap = 0;
            for (uint8_t j = 0; j < child->slot_len; j++) {
                uint64_t child_gap;
                if (child->slot[j] == NULL) {
                    uint64_t child_slot_min = mn_slot_min(child, j, slot_min);
                    uint64_t child_slot_max = mn_pivot(child, j, slot_max);
                    child_gap = __mt_range_size(child_slot_min, child_slot_max);
                } else {
                    child_gap = child->gap[j];
                }
                if (child_gap > gap)
                    gap = child_gap;
            }
        }
        if (gap > max_gap)
            max_gap = gap;
    }
    return max_gap;
}

static uint64_t __mt_refresh_node_gap(struct maple_node *node,
                                       uint64_t node_min, uint64_t node_max)
{
    if (node == NULL)
        return __mt_range_size(node_min, node_max);

    if (mn_is_leaf(node))
        return __mt_node_max_gap(node, node_min, node_max);

    uint64_t max_gap = 0;
    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t slot_min = mn_slot_min(node, i, node_min);
        uint64_t slot_max = mn_pivot(node, i, node_max);
        struct maple_node *child = (struct maple_node *)node->slot[i];
        uint64_t gap = __mt_node_max_gap(child, slot_min, slot_max);
        node->gap[i] = gap;
        if (gap > max_gap)
            max_gap = gap;
    }
    for (uint8_t i = node->slot_len; i < MAPLE_NODE_SLOTS; i++)
        node->gap[i] = 0;
    return max_gap;
}

static void __mt_propagate_gaps_from(struct maple_tree *mt,
                                      struct maple_node *node)
{
    while (node != NULL) {
        uint64_t node_min, node_max;
        __find_node_bounds(mt, node, &node_min, &node_max);
        __mt_refresh_node_gap(node, node_min, node_max);

        if (mn_is_root(node))
            break;
        node = mn_get_parent(node);
    }
}

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

static int __mt_find_gap_fwd(struct maple_node *node, uint64_t node_min,
                              uint64_t node_max, uint64_t size,
                              uint64_t search_min, uint64_t search_max,
                              uint64_t *gap_start)
{
    if (node == NULL)
        return -EBUSY;

    if (mn_is_leaf(node)) {
        for (uint8_t i = 0; i < node->slot_len; i++) {
            uint64_t slot_min = mn_slot_min(node, i, node_min);
            uint64_t slot_max = mn_pivot(node, i, node_max);
            if (slot_max < search_min || slot_min > search_max)
                continue;
            if (node->slot[i] != NULL)
                continue;
            uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
            uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
            if (__mt_range_size(clipped_min, clipped_max) >= size) {
                *gap_start = clipped_min;
                return 0;
            }
        }
        return -EBUSY;
    }

    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t slot_min = mn_slot_min(node, i, node_min);
        uint64_t slot_max = mn_pivot(node, i, node_max);
        if (slot_max < search_min || slot_min > search_max)
            continue;

        uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
        uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
        if (__mt_range_size(clipped_min, clipped_max) < size)
            continue;

        struct maple_node *child = (struct maple_node *)node->slot[i];
        if (child == NULL) {
            *gap_start = clipped_min;
            return 0;
        }
        if (node->gap[i] < size)
            continue;
        if (__mt_find_gap_fwd(child, slot_min, slot_max, size,
                               search_min, search_max, gap_start) == 0)
            return 0;
    }
    return -EBUSY;
}

static int __mt_find_gap_rev(struct maple_node *node, uint64_t node_min,
                              uint64_t node_max, uint64_t size,
                              uint64_t search_min, uint64_t search_max,
                              uint64_t *gap_start)
{
    if (node == NULL)
        return -EBUSY;

    if (mn_is_leaf(node)) {
        for (int i = (int)node->slot_len - 1; i >= 0; i--) {
            uint64_t slot_min = mn_slot_min(node, (uint8_t)i, node_min);
            uint64_t slot_max = mn_pivot(node, (uint8_t)i, node_max);
            if (slot_max < search_min || slot_min > search_max)
                continue;
            if (node->slot[i] != NULL)
                continue;
            uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
            uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
            if (__mt_range_size(clipped_min, clipped_max) >= size) {
                *gap_start = clipped_max - size + 1;
                return 0;
            }
        }
        return -EBUSY;
    }

    for (int i = (int)node->slot_len - 1; i >= 0; i--) {
        uint64_t slot_min = mn_slot_min(node, (uint8_t)i, node_min);
        uint64_t slot_max = mn_pivot(node, (uint8_t)i, node_max);
        if (slot_max < search_min || slot_min > search_max)
            continue;

        uint64_t clipped_min = slot_min < search_min ? search_min : slot_min;
        uint64_t clipped_max = slot_max > search_max ? search_max : slot_max;
        if (__mt_range_size(clipped_min, clipped_max) < size)
            continue;

        struct maple_node *child = (struct maple_node *)node->slot[i];
        if (child == NULL) {
            *gap_start = clipped_max - size + 1;
            return 0;
        }
        if (node->gap[i] < size)
            continue;
        if (__mt_find_gap_rev(child, slot_min, slot_max, size,
                               search_min, search_max, gap_start) == 0)
            return 0;
    }
    return -EBUSY;
}

/* ====================================================================== */
/*  Insert into parent / split helpers                                     */
/* ====================================================================== */

static int __mt_insert_into_parent(struct maple_tree *mt,
                                    struct maple_node *parent,
                                    uint8_t slot,
                                    uint64_t pivot_val,
                                    struct maple_node *right,
                                    uint64_t parent_min,
                                    uint64_t parent_max)
{
    if (parent->slot_len < MAPLE_NODE_SLOTS) {
        uint8_t len = parent->slot_len;
        for (int i = len; i > (int)slot + 1; i--) {
            parent->slot[i] = parent->slot[i - 1];
            if (i - 1 < MAPLE_NODE_PIVOTS)
                parent->pivot[i - 1] = parent->pivot[i - 2];
            if (!mn_is_leaf(parent))
                parent->gap[i] = parent->gap[i - 1];
        }
        parent->slot[slot + 1] = right;
        parent->pivot[slot] = pivot_val;
        parent->slot_len = len + 1;

        mn_set_parent(right, parent, slot + 1);

        if (!mn_is_leaf(parent)) {
            for (uint8_t i = slot + 2; i < parent->slot_len; i++) {
                struct maple_node *child = parent->slot[i];
                if (child != NULL)
                    mn_set_parent(child, parent, i);
            }
        }
        return 0;
    }

    /* Parent full — split it first. */
    struct maple_node *left_child = parent->slot[slot];

    int ret = __mt_split_node(mt, parent, parent_min, parent_max);
    if (ret != 0)
        return ret;

    struct maple_node *new_parent = mn_get_parent(left_child);
    uint8_t new_slot = mn_get_parent_slot(left_child);

    uint64_t new_pmin, new_pmax;
    __find_node_bounds(mt, new_parent, &new_pmin, &new_pmax);

    return __mt_insert_into_parent(mt, new_parent, new_slot, pivot_val,
                                    right, new_pmin, new_pmax);
}

static void __find_node_bounds(struct maple_tree *mt, struct maple_node *node,
                                uint64_t *out_min, uint64_t *out_max)
{
    if (mn_is_root(node)) {
        *out_min = 0;
        *out_max = MAPLE_MAX;
        return;
    }

    struct maple_node *parent = mn_get_parent(node);
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    uint8_t slot = mn_get_parent_slot(node);
    *out_min = mn_slot_min(parent, slot, pmin);
    *out_max = mn_pivot(parent, slot, pmax);
}

static int __mt_split_node(struct maple_tree *mt, struct maple_node *node,
                            uint64_t node_min, uint64_t node_max)
{
    (void)node_min;
    (void)node_max;

    struct maple_node *right = mt_alloc_node();
    if (right == NULL)
        return -ENOMEM;

    uint8_t split = node->slot_len >> 1;
    uint64_t median = node->pivot[split - 1];
    right->type = node->type;

    uint8_t right_len = node->slot_len - split;
    for (uint8_t i = 0; i < right_len; i++) {
        right->slot[i] = node->slot[split + i];
        if (i < right_len - 1 && (split + i) < MAPLE_NODE_PIVOTS)
            right->pivot[i] = node->pivot[split + i];
        if (!mn_is_leaf(node))
            right->gap[i] = node->gap[split + i];
    }
    right->slot_len = right_len;

    if (!mn_is_leaf(right)) {
        for (uint8_t i = 0; i < right_len; i++) {
            struct maple_node *child = right->slot[i];
            if (child != NULL)
                mn_set_parent(child, right, i);
        }
    }

    node->slot_len = split;

    if (mn_is_root(node)) {
        struct maple_node *new_root = mt_alloc_node();
        if (new_root == NULL) {
            /* Undo split. */
            for (uint8_t i = 0; i < right_len; i++) {
                node->slot[split + i] = right->slot[i];
                if (i < right_len - 1 && (split + i) < MAPLE_NODE_PIVOTS)
                    node->pivot[split + i] = right->pivot[i];
                if (!mn_is_leaf(node))
                    node->gap[split + i] = right->gap[i];
            }
            node->slot_len = split + right_len;
            if (!mn_is_leaf(node)) {
                for (uint8_t i = split; i < node->slot_len; i++) {
                    struct maple_node *child = node->slot[i];
                    if (child != NULL)
                        mn_set_parent(child, node, i);
                }
            }
            mt_free_node_now(right);
            return -ENOMEM;
        }
        new_root->type = maple_arange_64;
        new_root->slot[0] = node;
        new_root->slot[1] = right;
        new_root->pivot[0] = median;
        new_root->slot_len = 2;

        mn_set_root(new_root);
        mn_set_parent(node, new_root, 0);
        mn_set_parent(right, new_root, 1);

        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(new_root));
        return 0;
    }

    struct maple_node *parent = mn_get_parent(node);
    uint8_t pslot = mn_get_parent_slot(node);

    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);

    int ret = __mt_insert_into_parent(mt, parent, pslot, median, right,
                                       pmin, pmax);
    if (ret != 0) {
        /* Undo split. */
        for (uint8_t i = 0; i < right_len; i++) {
            node->slot[split + i] = right->slot[i];
            if (i < right_len - 1 && (split + i) < MAPLE_NODE_PIVOTS)
                node->pivot[split + i] = right->pivot[i];
            if (!mn_is_leaf(node))
                node->gap[split + i] = right->gap[i];
        }
        node->slot_len = split + right_len;
        if (!mn_is_leaf(node)) {
            for (uint8_t i = split; i < node->slot_len; i++) {
                struct maple_node *child = node->slot[i];
                if (child != NULL)
                    mn_set_parent(child, node, i);
            }
        }
        mt_free_node_now(right);
        return ret;
    }
    return 0;
}

/* ====================================================================== */
/*  Store (insert / overwrite)                                             */
/* ====================================================================== */

static int __mt_store_leaf(struct maple_tree *mt, struct maple_node *node,
                            uint8_t slot, uint64_t node_min, uint64_t node_max,
                            uint64_t first, uint64_t last, void *entry)
{
    uint64_t slot_min = mn_slot_min(node, slot, node_min);
    uint64_t slot_max = mn_pivot(node, slot, node_max);

    /* Exact match — just overwrite. */
    if (slot_min == first && slot_max == last) {
        mt_rcu_assign_pointer(&node->slot[slot], entry);
        return 0;
    }

    void *old_entry = node->slot[slot];
    int need_left  = (first > slot_min) ? 1 : 0;
    int need_right = (last < slot_max) ? 1 : 0;
    int extra = need_left + need_right;

    if (node->slot_len + extra > MAPLE_NODE_SLOTS) {
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        return mtree_store_range(mt, first, last, entry);
    }

    int n_pieces = 1 + need_left + need_right;
    void  *p_slot[3];
    uint64_t p_pivot[3];

    int pi = 0;
    if (need_left) {
        p_slot[pi]  = old_entry;
        p_pivot[pi] = first - 1;
        pi++;
    }
    p_slot[pi]  = entry;
    p_pivot[pi] = last;
    pi++;
    if (need_right) {
        p_slot[pi]  = old_entry;
        p_pivot[pi] = slot_max;
        pi++;
    }

    uint8_t old_len = node->slot_len;
    uint8_t new_len = old_len + (uint8_t)extra;

    for (int i = new_len - 1; i >= (int)slot + n_pieces; i--) {
        int src = i - extra;
        node->slot[i] = node->slot[src];
        if (src < (int)old_len - 1)
            node->pivot[i] = node->pivot[src];
    }

    for (int i = 0; i < n_pieces; i++) {
        int dst = slot + i;
        node->slot[dst] = p_slot[i];
        if (dst < (int)new_len - 1)
            node->pivot[dst] = p_pivot[i];
    }

    node->slot_len = new_len;
    return 0;
}

static void __mt_coalesce_leaf(struct maple_node *node, uint64_t node_max)
{
    (void)node_max;
    if (node->slot_len <= 1)
        return;

    uint8_t dst = 0;
    for (uint8_t src = 1; src < node->slot_len; src++) {
        if (node->slot[src] == node->slot[dst]) {
            if (src < node->slot_len - 1)
                node->pivot[dst] = node->pivot[src];
        } else {
            dst++;
            if (dst != src) {
                node->slot[dst] = node->slot[src];
                if (src < node->slot_len - 1 && dst < MAPLE_NODE_PIVOTS)
                    node->pivot[dst] = node->pivot[src];
            }
        }
    }
    node->slot_len = dst + 1;
}

int mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last,
                       void *entry)
{
    if (first > last)
        return -EINVAL;

    void *root = mt_rcu_dereference(&mt->ma_root);

    /* Empty tree: create first node. */
    if (root == NULL) {
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        node->type = maple_leaf_64;
        if (first > 0) {
            node->slot[0] = NULL;
            node->pivot[0] = first - 1;
            node->slot[1] = entry;
            if (last < MAPLE_MAX) {
                node->pivot[1] = last;
                node->slot[2] = NULL;
                node->slot_len = 3;
            } else {
                node->slot_len = 2;
            }
        } else {
            node->slot[0] = entry;
            if (last < MAPLE_MAX) {
                node->pivot[0] = last;
                node->slot[1] = NULL;
                node->slot_len = 2;
            } else {
                node->slot_len = 1;
            }
        }
        mn_set_root(node);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(node));
        return 0;
    }

    if (!mt_is_node(root)) {
        struct maple_node *node = mt_alloc_node();
        if (node == NULL)
            return -ENOMEM;
        node->type = maple_leaf_64;
        node->slot[0] = root;
        node->slot_len = 1;
        mn_set_root(node);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(node));
        root = mt_rcu_dereference(&mt->ma_root);
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (!mn_is_leaf(node)) {
        uint8_t slot = mn_find_slot(node, first, node->slot_len);
        uint64_t child_min = mn_slot_min(node, slot, node_min);
        uint64_t child_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL) {
            child = mt_alloc_node();
            if (child == NULL)
                return -ENOMEM;
            child->type = maple_leaf_64;
            child->slot[0] = NULL;
            child->slot_len = 1;
            mn_set_parent(child, node, slot);
            mt_rcu_assign_pointer(&node->slot[slot], child);
        }
        node_min = child_min;
        node_max = child_max;
        node = child;
    }

    uint64_t orig_last = last;
    if (last > node_max)
        last = node_max;

    uint8_t start_slot = mn_find_slot(node, first, node->slot_len);
    uint8_t end_slot = mn_find_slot_from(node, last, start_slot,
                                          node->slot_len);

    if (start_slot == end_slot) {
        int ret = __mt_store_leaf(mt, node, start_slot, node_min, node_max,
                                  first, last, entry);
        if (ret != 0)
            return ret;
        __mt_coalesce_leaf(node, node_max);
        if (orig_last > node_max)
            return mtree_store_range(mt, node_max + 1, orig_last, entry);
        __mt_propagate_gaps_from(mt, node);
        return 0;
    }

    /* Multiple slots affected. */
    uint64_t start_slot_min = mn_slot_min(node, start_slot, node_min);
    uint64_t end_slot_max = mn_pivot(node, end_slot, node_max);

    void *left_entry = node->slot[start_slot];
    void *right_entry = node->slot[end_slot];

    int need_left   = (first > start_slot_min) ? 1 : 0;
    int need_right  = (last < end_slot_max) ? 1 : 0;
    int new_range_slots = need_left + 1 + need_right;
    int old_range_slots = end_slot - start_slot + 1;
    int delta = new_range_slots - old_range_slots;

    if (node->slot_len + delta > MAPLE_NODE_SLOTS) {
        int ret = __mt_split_node(mt, node, node_min, node_max);
        if (ret != 0)
            return ret;
        return mtree_store_range(mt, first, orig_last, entry);
    }

    void  *p_slot[3];
    uint64_t p_pivot[3];
    int n_pieces = 0;

    if (need_left) {
        p_slot[n_pieces]  = left_entry;
        p_pivot[n_pieces] = first - 1;
        n_pieces++;
    }
    p_slot[n_pieces]  = entry;
    p_pivot[n_pieces] = last;
    n_pieces++;
    if (need_right) {
        p_slot[n_pieces]  = right_entry;
        p_pivot[n_pieces] = end_slot_max;
        n_pieces++;
    }

    uint8_t old_len = node->slot_len;
    uint8_t new_len = (uint8_t)((int)old_len + delta);

    int trail_start_src = end_slot + 1;

    if (delta > 0) {
        for (int i = (int)old_len - 1; i >= trail_start_src; i--) {
            int d = i + delta;
            node->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                node->pivot[d] = node->pivot[i];
        }
    } else if (delta < 0) {
        for (int i = trail_start_src; i < (int)old_len; i++) {
            int d = i + delta;
            node->slot[d] = node->slot[i];
            if (i < (int)old_len - 1)
                node->pivot[d] = node->pivot[i];
        }
    }

    for (int i = 0; i < n_pieces; i++) {
        int dst = start_slot + i;
        node->slot[dst] = p_slot[i];
        if (dst < (int)new_len - 1)
            node->pivot[dst] = p_pivot[i];
    }

    node->slot_len = new_len;
    __mt_coalesce_leaf(node, node_max);
    if (orig_last > node_max)
        return mtree_store_range(mt, node_max + 1, orig_last, entry);
    __mt_propagate_gaps_from(mt, node);
    return 0;
}

/* ====================================================================== */
/*  Rebalancing — merging, redistribution, and tree shrinking              */
/* ====================================================================== */

/**
 * Minimum number of slots a non-root node should have after erasure.
 * Below this threshold, try to merge or redistribute with a sibling.
 */
#define MAPLE_NODE_MIN  (MAPLE_NODE_SLOTS / 4)  /* 4 */

/**
 * __mt_shrink_root - Collapse a single-child root to reduce tree height.
 *
 * If the root is an internal node with exactly one child, replace the root
 * with that child.  Repeat until the root has >1 child or is a leaf.
 */
static void __mt_shrink_root(struct maple_tree *mt)
{
    while (1) {
        void *root = mt_rcu_dereference(&mt->ma_root);
        if (root == NULL || !mt_is_node(root))
            return;

        struct maple_node *node = mt_to_node(root);
        if (mn_is_leaf(node))
            return;
        if (node->slot_len != 1)
            return;

        struct maple_node *child = node->slot[0];
        if (child == NULL)
            return;

        /* Promote child to root. */
        mn_set_root(child);
        mt_rcu_assign_pointer(&mt->ma_root, mt_mk_root(child));
        mt_free_node_rcu(node);
    }
}

/**
 * __mt_get_sibling - Find a sibling node and its slot bounds.
 *
 * Prefers the left sibling; falls back to the right sibling.
 * Returns the sibling node, or NULL if none exists (i.e. node is root or
 * parent has only one child).
 *
 * @node:         The node that needs rebalancing.
 * @sib_out:      Returned sibling.
 * @sib_slot_out: Sibling's slot index within the parent.
 * @is_right_out: True if the returned sibling is to the right of @node.
 */
static struct maple_node *
__mt_get_sibling(struct maple_node *node,
                 uint8_t *sib_slot_out, bool *is_right_out)
{
    if (mn_is_root(node))
        return NULL;

    struct maple_node *parent = mn_get_parent(node);
    uint8_t pslot = mn_get_parent_slot(node);

    /* Prefer left sibling. */
    if (pslot > 0) {
        struct maple_node *left = parent->slot[pslot - 1];
        if (left != NULL) {
            *sib_slot_out = pslot - 1;
            *is_right_out = false;
            return left;
        }
    }
    /* Fall back to right sibling. */
    if (pslot + 1 < parent->slot_len) {
        struct maple_node *right = parent->slot[pslot + 1];
        if (right != NULL) {
            *sib_slot_out = pslot + 1;
            *is_right_out = true;
            return right;
        }
    }
    return NULL;
}

/**
 * __mt_merge_nodes - Merge two leaf siblings into one and remove the
 * empty node from their parent.
 *
 * @mt:      The tree.
 * @left:    Left sibling leaf (destination).
 * @right:   Right sibling leaf (source — will be freed).
 * @parent:  Parent of both nodes.
 * @left_slot: @left's slot index in @parent.
 *
 * Precondition: left->slot_len + right->slot_len <= MAPLE_NODE_SLOTS.
 */
static void __mt_merge_leaves(struct maple_tree *mt,
                              struct maple_node *left,
                              struct maple_node *right,
                              struct maple_node *parent,
                              uint8_t left_slot)
{
    uint8_t right_slot = left_slot + 1;
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);
    uint64_t right_max = mn_pivot(parent, right_slot, pmax);

    /* Append right's entries after left's. */
    uint8_t dst = left->slot_len;
    /* The last pivot in left needs to be set to left_max (boundary). */
    if (dst > 0 && dst - 1 < MAPLE_NODE_PIVOTS)
        left->pivot[dst - 1] = left_max;

    for (uint8_t i = 0; i < right->slot_len; i++) {
        left->slot[dst + i] = right->slot[i];
        if (i < right->slot_len - 1 && (dst + i) < MAPLE_NODE_PIVOTS)
            left->pivot[dst + i] = right->pivot[i];
    }
    left->slot_len = dst + right->slot_len;

    /* Coalesce adjacent identical entries in the merged leaf. */
    __mt_coalesce_leaf(left, right_max);

    /*
     * Update parent pivot[left_slot] to cover the right node's range,
     * since left now spans [left_min, right_max].
     */
    if (left_slot < MAPLE_NODE_PIVOTS)
        parent->pivot[left_slot] = right_max;

    /* Remove right_slot from the parent. */
    for (uint8_t i = right_slot; i + 1 < parent->slot_len; i++) {
        parent->slot[i] = parent->slot[i + 1];
        if (i < MAPLE_NODE_PIVOTS && i + 1 < parent->slot_len - 1)
            parent->pivot[i] = parent->pivot[i + 1];
        if (!mn_is_leaf(parent))
            parent->gap[i] = parent->gap[i + 1];
    }
    parent->slot_len--;

    /* Clear the now-unused last slot. */
    parent->slot[parent->slot_len] = NULL;

    /* Fix parent_slot for children after the removed slot. */
    for (uint8_t i = right_slot; i < parent->slot_len; i++) {
        struct maple_node *child = parent->slot[i];
        if (child != NULL)
            mn_set_parent(child, parent, i);
    }

    mt_free_node_rcu(right);
}

/**
 * __mt_merge_internal - Merge two internal siblings into one.
 *
 * Same idea as leaf merge, but also re-parents the children.
 */
static void __mt_merge_internal(struct maple_tree *mt,
                                struct maple_node *left,
                                struct maple_node *right,
                                struct maple_node *parent,
                                uint8_t left_slot)
{
    uint8_t right_slot = left_slot + 1;
    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);
    uint64_t right_max = mn_pivot(parent, right_slot, pmax);

    uint8_t dst = left->slot_len;
    /* Set the pivot between the last left entry and first right entry. */
    if (dst > 0 && dst - 1 < MAPLE_NODE_PIVOTS)
        left->pivot[dst - 1] = left_max;

    for (uint8_t i = 0; i < right->slot_len; i++) {
        left->slot[dst + i] = right->slot[i];
        if (i < right->slot_len - 1 && (dst + i) < MAPLE_NODE_PIVOTS)
            left->pivot[dst + i] = right->pivot[i];
        left->gap[dst + i] = right->gap[i];
    }
    left->slot_len = dst + right->slot_len;

    /* Re-parent all children that moved from right to left. */
    for (uint8_t i = dst; i < left->slot_len; i++) {
        struct maple_node *child = left->slot[i];
        if (child != NULL)
            mn_set_parent(child, left, i);
    }

    /* Update parent pivot to cover right's range. */
    if (left_slot < MAPLE_NODE_PIVOTS)
        parent->pivot[left_slot] = right_max;

    /* Remove right_slot from parent. */
    for (uint8_t i = right_slot; i + 1 < parent->slot_len; i++) {
        parent->slot[i] = parent->slot[i + 1];
        if (i < MAPLE_NODE_PIVOTS && i + 1 < parent->slot_len - 1)
            parent->pivot[i] = parent->pivot[i + 1];
        parent->gap[i] = parent->gap[i + 1];
    }
    parent->slot_len--;
    parent->slot[parent->slot_len] = NULL;

    for (uint8_t i = right_slot; i < parent->slot_len; i++) {
        struct maple_node *child = parent->slot[i];
        if (child != NULL)
            mn_set_parent(child, parent, i);
    }

    mt_free_node_rcu(right);
}

/**
 * __mt_redistribute_leaves - Balance entries between two leaf siblings.
 *
 * Moves entries from the heavier sibling to the lighter one so that both
 * have approximately equal occupancy.
 *
 * @mt:      The tree.
 * @node:    The underfull node.
 * @sibling: Its sibling.
 * @parent:  Parent of both.
 * @node_slot: @node's slot index in @parent.
 * @sib_slot:  @sibling's slot index in @parent.
 */
static void __mt_redistribute_leaves(struct maple_tree *mt,
                                     struct maple_node *node,
                                     struct maple_node *sibling,
                                     struct maple_node *parent,
                                     uint8_t node_slot,
                                     uint8_t sib_slot)
{
    /* Determine left/right ordering. */
    struct maple_node *left, *right;
    uint8_t left_slot;
    if (node_slot < sib_slot) {
        left = node;
        right = sibling;
        left_slot = node_slot;
    } else {
        left = sibling;
        right = node;
        left_slot = sib_slot;
    }

    /*
     * Collect all entries from both nodes into a temporary buffer,
     * then split them evenly.  This is simpler and less error-prone
     * than shifting entries one-by-one.
     */
    uint8_t total = left->slot_len + right->slot_len;
    void    *tmp_slot[MAPLE_NODE_SLOTS * 2];
    uint64_t tmp_pivot[MAPLE_NODE_SLOTS * 2];

    uint64_t pmin, pmax;
    __find_node_bounds(mt, parent, &pmin, &pmax);
    uint64_t left_max = mn_pivot(parent, left_slot, pmax);

    /* Copy left entries. */
    uint8_t ti = 0;
    for (uint8_t i = 0; i < left->slot_len; i++, ti++) {
        tmp_slot[ti] = left->slot[i];
        if (i < left->slot_len - 1)
            tmp_pivot[ti] = left->pivot[i];
        else
            tmp_pivot[ti] = left_max;
    }
    /* Copy right entries. */
    uint64_t right_max = mn_pivot(parent, left_slot + 1, pmax);
    for (uint8_t i = 0; i < right->slot_len; i++, ti++) {
        tmp_slot[ti] = right->slot[i];
        if (i < right->slot_len - 1)
            tmp_pivot[ti] = right->pivot[i];
        else
            tmp_pivot[ti] = right_max;
    }

    /* Split: left gets first half, right gets second half. */
    uint8_t left_new = total >> 1;
    uint8_t right_new = total - left_new;

    /* Refill left. */
    for (uint8_t i = 0; i < left_new; i++) {
        left->slot[i] = tmp_slot[i];
        if (i < left_new - 1)
            left->pivot[i] = tmp_pivot[i];
    }
    left->slot_len = left_new;
    /* Clear old trailing slots. */
    for (uint8_t i = left_new; i < MAPLE_NODE_SLOTS; i++)
        left->slot[i] = NULL;

    /* Refill right. */
    for (uint8_t i = 0; i < right_new; i++) {
        right->slot[i] = tmp_slot[left_new + i];
        if (i < right_new - 1)
            right->pivot[i] = tmp_pivot[left_new + i];
    }
    right->slot_len = right_new;
    for (uint8_t i = right_new; i < MAPLE_NODE_SLOTS; i++)
        right->slot[i] = NULL;

    /* Update parent pivot: left's new upper bound. */
    parent->pivot[left_slot] = tmp_pivot[left_new - 1];
}

/**
 * __mt_rebalance_node - Try to rebalance a node after erasure.
 *
 * Called when a node drops below MAPLE_NODE_MIN entries.
 * Tries to merge with a sibling first (if combined <= MAPLE_NODE_SLOTS).
 * Falls back to redistribution otherwise.
 * After merging, rebalances the parent recursively if it becomes underfull.
 * Finally, shrinks the tree root if it has only one child.
 */
static void __mt_rebalance_node(struct maple_tree *mt,
                                struct maple_node *node)
{
    /* Never rebalance the root itself (shrinking handles that). */
    if (mn_is_root(node))
        return;

    /* Only rebalance if underfull. */
    if (node->slot_len >= MAPLE_NODE_MIN)
        return;

    uint8_t sib_slot;
    bool is_right;
    struct maple_node *sibling = __mt_get_sibling(node, &sib_slot, &is_right);
    if (sibling == NULL)
        return;

    struct maple_node *parent = mn_get_parent(node);
    uint8_t node_slot = mn_get_parent_slot(node);
    uint8_t combined = node->slot_len + sibling->slot_len;

    if (combined <= MAPLE_NODE_SLOTS) {
        /* Merge: put everything into the left node, free the right. */
        struct maple_node *left, *right;
        uint8_t left_slot;
        if (!is_right) {
            /* sibling is to the left of node */
            left = sibling;
            right = node;
            left_slot = sib_slot;
        } else {
            left = node;
            right = sibling;
            left_slot = node_slot;
        }

        if (mn_is_leaf(left))
            __mt_merge_leaves(mt, left, right, parent, left_slot);
        else
            __mt_merge_internal(mt, left, right, parent, left_slot);

        /* Propagate gaps from the merged node upward. */
        __mt_propagate_gaps_from(mt, left);

        /* Parent may now be underfull — rebalance recursively. */
        if (!mn_is_root(parent) && parent->slot_len < MAPLE_NODE_MIN)
            __mt_rebalance_node(mt, parent);

        /* Root may now have only one child. */
        __mt_shrink_root(mt);
    } else {
        /* Cannot merge — redistribute entries between the two nodes. */
        if (mn_is_leaf(node)) {
            __mt_redistribute_leaves(mt, node, sibling, parent,
                                     node_slot, sib_slot);
        }
        /* Note: internal-node redistribution is complex and rarely needed
         * since parent underflow is handled by recursive merge above.
         * Leaf redistribution covers the common case. */
        __mt_propagate_gaps_from(mt, node);
        __mt_propagate_gaps_from(mt, sibling);
    }
}

/* ====================================================================== */
/*  Erase                                                                  */
/* ====================================================================== */

void *mtree_erase(struct maple_tree *mt, uint64_t index)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return NULL;

    if (!mt_is_node(root)) {
        mt_rcu_assign_pointer(&mt->ma_root, NULL);
        return root;
    }

    struct maple_node *node = mt_to_node(root);
    uint64_t node_min = 0;
    uint64_t node_max = MAPLE_MAX;

    while (!mn_is_leaf(node)) {
        uint8_t slot = mn_find_slot(node, index, node->slot_len);
        node_min = mn_slot_min(node, slot, node_min);
        node_max = mn_pivot(node, slot, node_max);

        struct maple_node *child = node->slot[slot];
        if (child == NULL)
            return NULL;
        node = child;
    }

    uint8_t slot = mn_find_slot(node, index, node->slot_len);

    void *old = node->slot[slot];
    node->slot[slot] = NULL;

    __mt_coalesce_leaf(node, node_max);
    __mt_propagate_gaps_from(mt, node);

    /* Rebalance if the leaf is underfull and not the root. */
    if (!mn_is_root(node) && node->slot_len < MAPLE_NODE_MIN)
        __mt_rebalance_node(mt, node);

    /* Even without rebalance, the root might have become single-child. */
    __mt_shrink_root(mt);

    return old;
}

/* ====================================================================== */
/*  Partial erase (single-index hole punch)                                */
/* ====================================================================== */

void *mtree_erase_index(struct maple_tree *mt, uint64_t index)
{
    void *old = mtree_load(mt, index);
    if (old != NULL)
        mtree_store_range(mt, index, index, NULL);
    return old;
}

/* ====================================================================== */
/*  Destroy                                                                */
/* ====================================================================== */

static void __mt_destroy_walk(struct maple_node *node)
{
    if (node == NULL)
        return;

    if (!mn_is_leaf(node)) {
        for (uint8_t i = 0; i < node->slot_len; i++) {
            struct maple_node *child = node->slot[i];
            if (child != NULL)
                __mt_destroy_walk(child);
        }
    }
    mt_free_node_now(node);
}

void mtree_destroy(struct maple_tree *mt)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    if (root == NULL)
        return;

    if (mt_is_node(root))
        __mt_destroy_walk(mt_to_node(root));

    mt_rcu_assign_pointer(&mt->ma_root, NULL);
}

/* ====================================================================== */
/*  Debug dump                                                             */
/* ====================================================================== */

static void __mt_dump_node(struct maple_node *node, uint64_t node_min,
                            uint64_t node_max, int depth)
{
    if (node == NULL) {
        printf("%*s(null node)\n", depth * 2, "");
        return;
    }
    printf("%*snode=%p %s len=%d range=[%" PRIx64 ", %" PRIx64 "]"
           " parent=%" PRIx64 " pslot=%d\n",
           depth * 2, "", (void *)node,
           mn_is_leaf(node) ? "LEAF" : "INTERNAL",
           node->slot_len, node_min, node_max,
           node->parent, node->parent_slot);
    for (uint8_t i = 0; i < node->slot_len; i++) {
        uint64_t smin = mn_slot_min(node, i, node_min);
        uint64_t smax = mn_pivot(node, i, node_max);
        if (mn_is_leaf(node)) {
            printf("%*s  [%d] [%" PRIx64 ", %" PRIx64 "] -> %p\n",
                   depth * 2, "", i, smin, smax, node->slot[i]);
        } else {
            printf("%*s  [%d] pivot_upper=%" PRIx64 " -> child=%p\n",
                   depth * 2, "", i, smax, node->slot[i]);
            if (node->slot[i] != NULL)
                __mt_dump_node((struct maple_node *)node->slot[i],
                                smin, smax, depth + 1);
        }
    }
    printf("%*s  raw pivots:", depth * 2, "");
    for (uint8_t i = 0; i < node->slot_len && i < MAPLE_NODE_PIVOTS; i++)
        printf(" [%d]=%" PRIx64, i, node->pivot[i]);
    printf("\n");
}

void mt_dump_tree(struct maple_tree *mt)
{
    void *root = mt_rcu_dereference(&mt->ma_root);
    printf("=== MAPLE TREE DUMP mt=%p root=%p ===\n", (void *)mt, root);
    if (root == NULL) {
        printf("  (empty tree)\n");
        return;
    }
    if (!mt_is_node(root)) {
        printf("  (single entry: %p)\n", root);
        return;
    }
    __mt_dump_node(mt_to_node(root), 0, MAPLE_MAX, 0);
    printf("=== END MAPLE TREE DUMP ===\n");
}

/* ====================================================================== */
/*  Cursor next / prev                                                     */
/* ====================================================================== */

static void *__mas_next_slot(struct ma_state *mas)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    uint8_t next = mas->offset + 1;
    if (next >= node->slot_len)
        return NULL;

    uint64_t node_min, node_max;
    __find_node_bounds(mas->tree, node, &node_min, &node_max);

    mas->offset = next;
    mas->min = mn_slot_min(node, next, node_min);
    mas->max = mn_pivot(node, next, node_max);
    mas->index = mas->min;
    return node->slot[next];
}

static void *__mas_next_node(struct ma_state *mas, uint64_t limit)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    while (!mn_is_root(node)) {
        struct maple_node *parent = mn_get_parent(node);
        uint8_t pslot = mn_get_parent_slot(node);

        for (uint8_t next = pslot + 1; next < parent->slot_len; next++) {
            uint64_t node_min, node_max;
            __find_node_bounds(mas->tree, parent, &node_min, &node_max);

            uint64_t child_min = mn_slot_min(parent, next, node_min);
            if (child_min > limit) {
                mas->node = NULL;
                return NULL;
            }

            struct maple_node *child = parent->slot[next];
            if (child == NULL)
                continue;

            if (mn_is_leaf(parent)) {
                uint64_t child_max = mn_pivot(parent, next, node_max);
                mas->node = parent;
                mas->offset = next;
                mas->min = child_min;
                mas->max = child_max;
                mas->index = child_min;
                return parent->slot[next];
            }

            node = (struct maple_node *)child;
            while (1) {
                if (mn_is_leaf(node)) {
                    uint64_t nmin, nmax;
                    __find_node_bounds(mas->tree, node, &nmin, &nmax);
                    for (uint8_t i = 0; i < node->slot_len; i++) {
                        if (node->slot[i] == NULL)
                            continue;
                        mas->node = node;
                        mas->offset = i;
                        mas->min = mn_slot_min(node, i, nmin);
                        mas->max = mn_pivot(node, i, nmax);
                        mas->index = mas->min;
                        return node->slot[i];
                    }
                    break;
                }

                struct maple_node *next_child = NULL;
                for (uint8_t i = 0; i < node->slot_len; i++) {
                    if (node->slot[i] != NULL) {
                        next_child = (struct maple_node *)node->slot[i];
                        break;
                    }
                }
                if (next_child == NULL)
                    break;
                node = next_child;
            }
        }
        node = parent;
    }

    mas->node = NULL;
    return NULL;
}

void *mas_next(struct ma_state *mas, uint64_t max)
{
    while (1) {
        void *entry = __mas_next_slot(mas);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            return entry;
        }

        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        entry = __mas_next_node(mas, max);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            return entry;
        }
        if (mas->node == NULL)
            return NULL;
    }
}

static void *__mas_prev_slot(struct ma_state *mas)
{
    struct maple_node *node = mas->node;
    if (node == NULL || mas->offset == 0)
        return NULL;

    uint64_t node_min, node_max;
    __find_node_bounds(mas->tree, node, &node_min, &node_max);

    uint8_t prev = mas->offset - 1;
    mas->offset = prev;
    mas->min = mn_slot_min(node, prev, node_min);
    mas->max = mn_pivot(node, prev, node_max);
    mas->index = mas->min;
    return node->slot[prev];
}

static void *__mas_prev_node(struct ma_state *mas, uint64_t limit)
{
    struct maple_node *node = mas->node;
    if (node == NULL)
        return NULL;

    while (!mn_is_root(node)) {
        struct maple_node *parent = mn_get_parent(node);
        uint8_t pslot = mn_get_parent_slot(node);

        if (pslot > 0) {
            uint64_t pmin, pmax;
            __find_node_bounds(mas->tree, parent, &pmin, &pmax);

            for (int prev = (int)pslot - 1; prev >= 0; prev--) {
                uint64_t child_max = mn_pivot(parent, (uint8_t)prev, pmax);
                if (child_max < limit) {
                    mas->node = NULL;
                    return NULL;
                }

                void *child = parent->slot[prev];
                if (child == NULL)
                    continue;

                if (mn_is_leaf(parent)) {
                    uint64_t child_min = mn_slot_min(parent, (uint8_t)prev,
                                                      pmin);
                    mas->node = parent;
                    mas->offset = (uint8_t)prev;
                    mas->min = child_min;
                    mas->max = child_max;
                    mas->index = child_min;
                    return child;
                }

                struct maple_node *cursor = (struct maple_node *)child;
                while (!mn_is_leaf(cursor)) {
                    uint8_t clen = cursor->slot_len;
                    if (clen == 0) {
                        cursor = NULL;
                        break;
                    }

                    struct maple_node *nxt = NULL;
                    for (int i = (int)clen - 1; i >= 0; i--) {
                        if (cursor->slot[i] != NULL) {
                            nxt = (struct maple_node *)cursor->slot[i];
                            break;
                        }
                    }
                    if (nxt == NULL) {
                        cursor = NULL;
                        break;
                    }
                    cursor = nxt;
                }

                if (cursor == NULL)
                    continue;

                uint64_t nmin, nmax;
                __find_node_bounds(mas->tree, cursor, &nmin, &nmax);
                uint8_t last = cursor->slot_len > 0 ?
                               cursor->slot_len - 1 : 0;
                mas->node = cursor;
                mas->offset = last;
                mas->min = mn_slot_min(cursor, last, nmin);
                mas->max = mn_pivot(cursor, last, nmax);
                mas->index = mas->min;
                if (mas->max < limit) {
                    mas->node = NULL;
                    return NULL;
                }
                return cursor->slot[last];
            }
        }
        node = parent;
    }

    mas->node = NULL;
    return NULL;
}

void *mas_prev(struct ma_state *mas, uint64_t min)
{
    while (1) {
        /* Try stepping backward within the current leaf node. */
        while (mas->node != NULL && mas->offset > 0) {
            void *entry = __mas_prev_slot(mas);
            if (entry != NULL) {
                if (mas->max < min)
                    return NULL;
                return entry;
            }
            /* Slot was NULL — keep trying earlier slots. */
        }

        /* At start of node or no node — ascend to previous node. */
        void *entry = __mas_prev_node(mas, min);
        if (mas->node == NULL)
            return NULL;
        if (entry != NULL) {
            if (mas->max < min)
                return NULL;
            return entry;
        }
        /* __mas_prev_node positioned us on a new node but the entry
         * was NULL; loop back to try slots in this new node. */
    }
}

/* ====================================================================== */
/*  mas_find                                                               */
/* ====================================================================== */

void *mas_find(struct ma_state *mas, uint64_t max)
{
    if (mas->index > max)
        return NULL;

    if (mas->node == NULL) {
        void *entry = mas_walk(mas);
        if (entry != NULL) {
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }
    }

    while (1) {
        void *entry = __mas_next_slot(mas);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }

        if (mas->node != NULL && mas->offset + 1 < mas->node->slot_len)
            continue;

        entry = __mas_next_node(mas, max);
        if (entry != NULL) {
            if (mas->min > max)
                return NULL;
            if (mas->max == MAPLE_MAX)
                mas->index = MAPLE_MAX;
            else
                mas->index = mas->max + 1;
            return entry;
        }
        if (mas->node == NULL)
            return NULL;
    }
}

/* ====================================================================== */
/*  mas_store / mas_erase                                                  */
/* ====================================================================== */

int mas_store(struct ma_state *mas, void *entry)
{
    return mtree_store_range(mas->tree, mas->index, mas->last, entry);
}

void *mas_erase(struct ma_state *mas)
{
    return mtree_erase(mas->tree, mas->index);
}

/* ====================================================================== */
/*  Gap search                                                             */
/* ====================================================================== */

int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max,
                    uint64_t size)
{
    if (size == 0 || min > max)
        return -EBUSY;

    if ((size - 1) > (max - min))
        return -EBUSY;

    void *root = mt_rcu_dereference(&mas->tree->ma_root);
    if (root == NULL) {
        mas->index = min;
        mas->last = min + size - 1;
        return 0;
    }

    if (!mt_is_node(root)) {
        if (min == 0 && max == MAPLE_MAX)
            return -EBUSY;
        mas->index = min;
        mas->last = min + size - 1;
        return 0;
    }

    uint64_t gap_start;
    if (__mt_find_gap_fwd(mt_to_node(root), 0, MAPLE_MAX, size,
                           min, max, &gap_start) != 0)
        return -EBUSY;

    mas->index = gap_start;
    mas->last = gap_start + size - 1;
    return 0;
}

int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max,
                        uint64_t size)
{
    if (size == 0 || min > max)
        return -EBUSY;

    if ((size - 1) > (max - min))
        return -EBUSY;

    void *root = mt_rcu_dereference(&mas->tree->ma_root);
    if (root == NULL) {
        mas->index = max - size + 1;
        mas->last = max;
        return 0;
    }

    if (!mt_is_node(root)) {
        if (min == 0 && max == MAPLE_MAX)
            return -EBUSY;
        mas->index = max - size + 1;
        mas->last = max;
        return 0;
    }

    uint64_t gap_start;
    if (__mt_find_gap_rev(mt_to_node(root), 0, MAPLE_MAX, size,
                           min, max, &gap_start) != 0)
        return -EBUSY;

    mas->index = gap_start;
    mas->last = gap_start + size - 1;
    return 0;
}

/* ====================================================================== */
/*  RCU read-side helpers                                                  */
/* ====================================================================== */

void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max)
{
    if (*index > max)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, *index, *index);
    void *entry = mas_find(&mas, max);
    if (entry != NULL) {
        if (mas.max == MAPLE_MAX)
            *index = MAPLE_MAX;
        else
            *index = mas.max + 1;
    }
    mt_rcu_unlock();
    return entry;
}

void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max)
{
    if (index >= max)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, index + 1, index + 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        mt_rcu_unlock();
        return entry;
    }
    entry = mas_find(&mas, max);
    mt_rcu_unlock();
    return entry;
}

void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min)
{
    if (index <= min)
        return NULL;

    mt_rcu_lock();
    MA_STATE(mas, mt, index - 1, index - 1);
    void *entry = mas_walk(&mas);
    if (entry != NULL) {
        mt_rcu_unlock();
        return entry;
    }
    entry = mas_prev(&mas, min);
    mt_rcu_unlock();
    return entry;
}
