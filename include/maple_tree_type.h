/**
 * @file maple_tree_type.h
 * @brief Maple tree type definitions — Linux-style B-tree for ranges.
 *
 * The maple tree is an RCU-safe B-tree that stores non-overlapping ranges
 * [index, last] → void* entry.  Designed for VM area management.
 *
 * Simplified from the Linux kernel maple tree (6.x):
 *  - 16 slots per node, 15 pivots.
 *  - Gap tracking for O(log n) free-range search.
 *  - Optional RCU-safe reads; write-side uses external locking.
 */

#ifndef MAPLE_TREE_TYPE_H
#define MAPLE_TREE_TYPE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "maple_tree_config.h"

/* ----------------------------------------------------------------------- */
/*  Constants                                                               */
/* ----------------------------------------------------------------------- */

/** Number of child/entry pointers per node (branching factor). */
#define MAPLE_NODE_SLOTS    16

/** Number of pivot (separator) keys per node. */
#define MAPLE_NODE_PIVOTS   (MAPLE_NODE_SLOTS - 1)

/** Tagged-pointer flag: ma_root contains a node pointer. */
#define MAPLE_ROOT_NODE     0x02UL

/** Minimum / maximum index values. */
#define MAPLE_MIN           0UL
#define MAPLE_MAX           (~(uint64_t)0)

/* ----------------------------------------------------------------------- */
/*  Node type                                                               */
/* ----------------------------------------------------------------------- */

/**
 * enum maple_type - Node type discriminator.
 * @maple_leaf_64:   Leaf node — slots point to user entries.
 * @maple_arange_64: Internal node — slots point to children, gap[] tracks
 *                   the largest gap in each subtree.
 */
enum maple_type {
    maple_leaf_64   = 0,
    maple_arange_64 = 1,
};

/* ----------------------------------------------------------------------- */
/*  Node structure                                                          */
/* ----------------------------------------------------------------------- */

/**
 * struct maple_node - A single B-tree node.
 *
 * @parent:      Tagged pointer to parent.  Bit 0 = "is root".
 * @type:        maple_leaf_64 or maple_arange_64.
 * @slot_len:    Number of slots in use (1..MAPLE_NODE_SLOTS).
 * @parent_slot: Slot index within the parent node (0..15).
 * @pivot:       Separating keys — pivot[i] is the inclusive upper bound
 *               of slot[i].  The last slot extends to the node's max.
 * @slot:        Pointers — child nodes (internal) or entries (leaf).
 * @gap:         (arange_64 only) Largest gap under each child subtree.
 */
struct maple_node {
    uint64_t parent;

    uint8_t  type;
    uint8_t  slot_len;
    uint8_t  parent_slot;
    uint8_t  __pad[5];

    uint64_t pivot[MAPLE_NODE_PIVOTS];
    void    *slot[MAPLE_NODE_SLOTS];
    uint64_t gap[MAPLE_NODE_SLOTS];
};

/* ----------------------------------------------------------------------- */
/*  Tree root                                                               */
/* ----------------------------------------------------------------------- */

/**
 * struct maple_tree - Root structure.
 *
 * @ma_root:  Tagged pointer — NULL (empty), or a node pointer | MAPLE_ROOT_NODE.
 * @ma_flags: Reserved for future flags.
 * @ma_lock:  (MT_CONFIG_LOCK only) Per-tree lock.
 */
struct maple_tree {
    void        *ma_root;
    unsigned int ma_flags;
#ifdef MT_CONFIG_LOCK
    mt_lock_t    ma_lock;
#endif
};

/* ----------------------------------------------------------------------- */
/*  Cursor (walk state)                                                     */
/* ----------------------------------------------------------------------- */

/**
 * struct ma_state (MAS) - Walk / cursor state.
 *
 * @tree:   Pointer to the maple tree.
 * @index:  Start of the search/store range.
 * @last:   End of the search/store range (inclusive).
 * @node:   Current node (or NULL).
 * @min:    Minimum index reachable through @node.
 * @max:    Maximum index reachable through @node.
 * @offset: Slot offset within @node.
 * @depth:  Current depth (0 = root).
 */
struct ma_state {
    struct maple_tree *tree;
    uint64_t index;
    uint64_t last;
    struct maple_node *node;
    uint64_t min;
    uint64_t max;
    uint8_t  offset;
    uint8_t  depth;
};

/** Stack initialiser for an ma_state. */
#define MA_STATE(name, mt, first, end)          \
    struct ma_state name = {                    \
        .tree   = (mt),                         \
        .index  = (first),                      \
        .last   = (end),                        \
        .node   = NULL,                         \
        .min    = 0,                            \
        .max    = MAPLE_MAX,                    \
        .offset = 0,                            \
        .depth  = 0,                            \
    }

#endif /* MAPLE_TREE_TYPE_H */
