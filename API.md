# Maple Tree API Reference

English | [中文](API.zh-CN.md)

The maple tree is a B-tree that maps non-overlapping `uint64_t` index ranges
to `void*` entries.  It features automatic node splitting/merging, gap
tracking for O(log n) free-range search, and optional RCU-safe reads.

Based on the Linux kernel maple tree (6.x), adapted to standalone C11 with
no kernel dependencies.

---

## Table of Contents

1. [Data Structures](#data-structures)
2. [Initialisation](#1-initialisation)
3. [Simple API](#2-simple-api)
4. [Cursor API](#3-cursor-api)
5. [Gap Search](#4-gap-search)
6. [RCU Read-Side Helpers](#5-rcu-read-side-helpers)
7. [Locking](#6-locking)
8. [Locked Wrappers](#7-locked-convenience-wrappers)
9. [Iteration Macros](#8-iteration-macros)
10. [Debug](#9-debug)
11. [Configuration](#configuration)

---

## Data Structures

### `struct maple_tree`

The root handle for a maple tree instance.

| Field      | Type          | Description                                   |
|------------|---------------|-----------------------------------------------|
| `ma_root`  | `void *`      | Tagged root pointer (NULL when empty).         |
| `ma_flags` | `unsigned int`| Reserved feature flags.                        |
| `ma_lock`  | `mt_lock_t`   | Per-tree lock (only when `MT_CONFIG_LOCK`).    |

Always allocate on the stack or embed in your own struct; then call
`mt_init()` before first use.

### `struct ma_state` (Cursor)

A lightweight cursor that tracks position within a tree traversal.  Always
stack-allocate via the `MA_STATE()` macro.

| Field    | Type                | Description                                 |
|----------|---------------------|---------------------------------------------|
| `tree`   | `struct maple_tree*` | The tree this cursor operates on.           |
| `index`  | `uint64_t`          | Start of the search/store range.            |
| `last`   | `uint64_t`          | End of the search/store range (inclusive).   |
| `node`   | `struct maple_node*` | Current node (NULL before first walk).      |
| `min`    | `uint64_t`          | Minimum index reachable through `node`.     |
| `max`    | `uint64_t`          | Maximum index reachable through `node`.     |
| `offset` | `uint8_t`           | Slot offset within `node`.                  |
| `depth`  | `uint8_t`           | Current depth (0 = root).                   |

### `MA_STATE(name, mt, first, end)`

Stack-initialiser macro for `struct ma_state`.

```c
MA_STATE(mas, &mt, 0, 0);   // cursor named 'mas', starting at index 0
```

### Constants

| Name               | Value             | Description                        |
|--------------------|-------------------|------------------------------------|
| `MAPLE_NODE_SLOTS` | 16                | Branching factor (slots per node). |
| `MAPLE_NODE_PIVOTS`| 15                | Pivot keys per node.               |
| `MAPLE_MIN`        | 0                 | Minimum index value.               |
| `MAPLE_MAX`        | `~(uint64_t)0`   | Maximum index value.               |

---

## 1. Initialisation

Set up a maple tree before first use, or check whether it is empty.

### Functions

#### `void mt_init(struct maple_tree *mt)`

Initialise an empty maple tree.  Sets the root to NULL, flags to 0, and
initialises the per-tree lock (when `MT_CONFIG_LOCK` is defined).

**Must be called before any other operation on the tree.**

#### `void mt_init_flags(struct maple_tree *mt, unsigned int flags)`

Same as `mt_init()` but stores `flags` in `mt->ma_flags`.  Flags are
reserved for future use.

#### `bool mt_empty(const struct maple_tree *mt)`

Returns `true` if the tree contains no entries.

### Using Together

```c
struct maple_tree mt;
mt_init(&mt);                   // required before first use
assert(mt_empty(&mt));          // starts empty

mtree_store(&mt, 0, ptr);       // insert something
assert(!mt_empty(&mt));

mtree_destroy(&mt);             // free all internal nodes
assert(mt_empty(&mt));          // empty again

// Safe to re-init and reuse:
mt_init(&mt);
```

---

## 2. Simple API

The most common operations in a single function call.  These do **not**
acquire any lock internally — the caller must serialise writes (see
[Locking](#6-locking)).

### Functions

#### `void *mtree_load(struct maple_tree *mt, uint64_t index)`

Look up the entry whose range contains `index`.

- **Returns:** The stored `void*` entry, or `NULL` if the index falls in a
  gap (no entry covers it).

#### `int mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last, void *entry)`

Insert or overwrite the entry for the contiguous range `[first, last]`.

- `entry` may be `NULL` — this punches a hole (explicit gap) in the
  tree, effectively erasing any entries that overlap the range.
- Overlapping entries are split or replaced automatically.
- Adjacent slots with the same pointer value are coalesced.
- Triggers node splits when a node is full; the tree grows in height as
  needed.
- **Returns:** `0` on success, `-EINVAL` if `first > last`, `-ENOMEM` on
  allocation failure.

#### `int mtree_store(struct maple_tree *mt, uint64_t index, void *entry)`

Convenience wrapper: `mtree_store_range(mt, index, index, entry)`.

- **Returns:** `0` on success, negative errno on failure.

#### `void *mtree_erase(struct maple_tree *mt, uint64_t index)`

Remove the **entire** entry whose range contains `index`.

> **Important:** This removes the whole range, not just the single index.
> If `[10, 29]` was stored and you call `mtree_erase(mt, 20)`, the entire
> `[10, 29]` entry is removed.  To punch a single-index hole instead, use
> `mtree_erase_index(mt, 20)`.

After removal, underfull nodes are rebalanced (merged or redistributed
with siblings) and the tree height may shrink.

- **Returns:** The previously stored `void*`, or `NULL` if no entry
  covered `index`.

#### `void *mtree_erase_index(struct maple_tree *mt, uint64_t index)`

Remove a single index from the tree, preserving the rest of the range.

Unlike `mtree_erase()`, this function only punches a hole at `index`.
For example, if `[10, 29]` was stored and `mtree_erase_index(mt, 20)`
is called, the result is two entries: `[10, 19]` and `[21, 29]`, each
retaining the original pointer value.  Index 20 becomes a gap.

If `index` is the only index in the entry (a point entry), the behaviour
is identical to `mtree_erase()`.

- **Returns:** The previously stored entry at `index`, or `NULL` if no
  entry covered `index`.

#### `void mtree_destroy(struct maple_tree *mt)`

Recursively free every internal node.  After return, `mt_empty()` is
`true`.

- Does **not** free the user-supplied entry pointers — only internal
  node structures.
- Safe to call on an already-empty tree.
- The caller must ensure no concurrent readers or writers are active.

### Using Together

```c
struct maple_tree mt;
mt_init(&mt);

// --- Store entries ---
mtree_store_range(&mt, 100, 199, region_a);   // range [100, 199]
mtree_store(&mt, 42, single_obj);             // single index 42

// --- Look up ---
void *v = mtree_load(&mt, 150);               // returns region_a
assert(mtree_load(&mt, 50) == NULL);           // gap → NULL

// --- Erase the whole range entry ---
void *old = mtree_erase(&mt, 150);             // removes ALL of [100, 199]
assert(old == region_a);
assert(mtree_load(&mt, 100) == NULL);          // gone

// --- Punch a single-index hole instead ---
mtree_store_range(&mt, 100, 199, region_b);    // re-insert
void *prev = mtree_erase_index(&mt, 150);      // only index 150 removed
assert(prev == region_b);
assert(mtree_load(&mt, 149) == region_b);       // still present
assert(mtree_load(&mt, 150) == NULL);           // hole
assert(mtree_load(&mt, 151) == region_b);       // still present

// --- Cleanup ---
mtree_destroy(&mt);
```

---

## 3. Cursor API

The cursor API uses a lightweight `struct ma_state` (stack-allocated via
`MA_STATE()`) to track position within the tree.  It avoids repeated
root-to-leaf traversals when performing sequential operations such as
scanning, batch inserts of adjacent keys, or mixed read-modify-erase
workflows.

### Functions

#### `void *mas_walk(struct ma_state *mas)`

Descend from the root to the leaf slot that covers `mas->index`.

On return, `mas->node`, `mas->offset`, `mas->min`, `mas->max`, and
`mas->depth` are updated.

- **Returns:** The entry at that position, or `NULL` if it is a gap.

#### `int mas_store(struct ma_state *mas, void *entry)`

Store `entry` at `[mas->index, mas->last]`, equivalent to
`mtree_store_range(mas->tree, mas->index, mas->last, entry)`.

- **Returns:** `0` on success, negative errno on failure.

#### `void *mas_erase(struct ma_state *mas)`

Erase the entry at `mas->index`, equivalent to
`mtree_erase(mas->tree, mas->index)`.

- **Returns:** The previously stored entry, or `NULL`.

#### `void *mas_find(struct ma_state *mas, uint64_t max)`

Find the next non-NULL entry at or after `mas->index`, up to `max`.

If the cursor has not yet been walked (`mas->node == NULL`), an implicit
`mas_walk()` is performed first.

On return, `mas->index` is advanced past the found entry (to
`mas->max + 1`) so repeated calls iterate forward.

- **Returns:** The next non-NULL entry, or `NULL` if no entry exists in
  `[mas->index, max]`.

#### `void *mas_next(struct ma_state *mas, uint64_t max)`

Advance to the next non-NULL entry after the current position.

The cursor must already be positioned (e.g. via `mas_walk()`).

- **Returns:** The next non-NULL entry, or `NULL` if none exists up to
  `max`.

#### `void *mas_prev(struct ma_state *mas, uint64_t min)`

Move to the previous non-NULL entry before the current position.

- **Returns:** The previous non-NULL entry, or `NULL` if none exists down
  to `min`.

### Using Together

```c
// --- Walk + conditional update ---
MA_STATE(mas, &mt, target_idx, target_idx);
void *cur = mas_walk(&mas);
if (cur == stale_ptr)
    mas_store(&mas, new_ptr);       // overwrite in-place

// --- Forward scan with cursor ---
MA_STATE(mas, &mt, 0, 0);
void *entry;
while ((entry = mas_find(&mas, UINT64_MAX)) != NULL) {
    // mas.index / mas.last give the entry's range boundaries
    printf("[%lu, %lu] -> %p\n", mas.index, mas.last, entry);
}

// --- Backward scan ---
MA_STATE(mas, &mt, UINT64_MAX, UINT64_MAX);
mas_walk(&mas);                     // position at the end
while ((entry = mas_prev(&mas, 0)) != NULL) {
    // process entries in reverse order
}

// --- Erase while iterating ---
MA_STATE(mas, &mt, 0, 0);
while ((entry = mas_find(&mas, 1000)) != NULL) {
    if (should_remove(entry))
        mas_erase(&mas);            // safe during iteration
}

// --- Batch insert via cursor ---
MA_STATE(mas, &mt, 0, 9);
mas_store(&mas, region_a);           // [0, 9]
mas.index = 10; mas.last = 19;
mas_store(&mas, region_b);           // [10, 19]
```

---

## 4. Gap Search

Gap-search functions locate contiguous runs of NULL entries (gaps) within a
specified index range.  They use per-node gap metadata for O(log n)
search, making them ideal for VM-style address-space allocators.

### Functions

#### `int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max, uint64_t size)`

Find the first (lowest-address) gap of at least `size` contiguous NULL
entries within `[min, max]`.

On success, `mas->index` and `mas->last` are set to the found gap range
`[start, start + size - 1]`.

- `size` must be > 0.
- **Returns:** `0` on success, `-EBUSY` if no suitable gap exists.

#### `int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max, uint64_t size)`

Same as `mas_empty_area()` but searches backward, returning the
highest-address gap that satisfies the size requirement.

- **Returns:** `0` on success, `-EBUSY` if no suitable gap exists.

### Using Together

```c
// --- Bottom-up (first-fit) allocator ---
MA_STATE(mas, &mt, 0, 0);
int ret = mas_empty_area(&mas, 0, UINT64_MAX, page_count);
if (ret == 0) {
    // Found: gap at [mas.index, mas.last]
    uint64_t alloc_start = mas.index;
    mtree_store_range(&mt, alloc_start,
                      alloc_start + page_count - 1, vma);
}

// --- Top-down (reverse) allocator ---
MA_STATE(mas, &mt, 0, 0);
ret = mas_empty_area_rev(&mas, 0, UINT64_MAX, page_count);
if (ret == 0) {
    uint64_t alloc_start = mas.index;
    mtree_store_range(&mt, alloc_start,
                      alloc_start + page_count - 1, vma);
}

// --- Free a region (gap reclaim) ---
mtree_store_range(&mt, alloc_start,
                  alloc_start + page_count - 1, NULL);
// Subsequent gap searches will find this region again.

// --- Allocator loop: fill until full ---
while (1) {
    MA_STATE(mas, &mt, 0, 0);
    if (mas_empty_area(&mas, 0, MAX_ADDR, 4096) != 0)
        break;  // -EBUSY, no more room
    mtree_store_range(&mt, mas.index, mas.last, new_region());
}
```

---

## 5. RCU Read-Side Helpers

These functions provide a self-contained read path that acquires the RCU
read lock internally (when `MT_CONFIG_RCU` is enabled).  Ideal for
one-shot lookups or neighbour queries from reader threads.

Without `MT_CONFIG_RCU`, the RCU lock calls compile to no-ops and these
functions behave identically to cursor-based lookups — but the caller must
still serialise against concurrent writes.

### Functions

#### `void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max)`

Find the next non-NULL entry at or after `*index`, up to `max`.

`*index` is updated on return to one past the end of the found entry's
range, so repeated calls iterate all entries.

- **Returns:** The found entry, or `NULL` if no non-NULL entry exists in
  `[*index, max]`.

#### `void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max)`

Return the next non-NULL entry strictly after `index`.

- **Returns:** The next entry, or `NULL` if `index >= max` or no entry
  exists in `(index, max]`.

#### `void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min)`

Return the previous non-NULL entry strictly before `index`.

- **Returns:** The previous entry, or `NULL` if `index <= min` or no
  entry exists in `[min, index)`.

### RCU Lock Functions

#### `void mt_rcu_lock(void)` / `void mt_rcu_unlock(void)`

Enter/leave an RCU read-side critical section.  Only needed when using
the cursor API across multiple calls within an RCU section.
`mt_find`/`mt_next`/`mt_prev` acquire RCU internally.

### Using Together

```c
// --- Iterate all entries with mt_find ---
uint64_t idx = 0;
void *entry;
while ((entry = mt_find(&mt, &idx, UINT64_MAX)) != NULL)
    process(entry);        // idx is auto-advanced past each entry

// --- Get neighbours of a known index ---
void *next = mt_next(&mt, current_idx, UINT64_MAX);
void *prev = mt_prev(&mt, current_idx, 0);

// --- Bounded search: first entry in [1000, 2000] ---
uint64_t start = 1000;
void *e = mt_find(&mt, &start, 2000);
if (e)
    printf("found at position before %lu\n", start);

// --- Explicit RCU section for cursor-based read ---
mt_rcu_lock();
MA_STATE(mas, &mt, 0, 0);
void *entry = mas_find(&mas, UINT64_MAX);
// ... use entry safely ...
mt_rcu_unlock();
```

---

## 6. Locking

Low-level lock/unlock for the per-tree mutex.  Available when
`MT_CONFIG_LOCK` is defined; otherwise both compile to no-ops.

Most callers should prefer the `mtree_lock_*` wrappers (see
[Locked Wrappers](#7-locked-convenience-wrappers)) for single-operation
locking.  Use `mt_lock()`/`mt_unlock()` directly only when batching
multiple operations in one critical section.

### Functions

#### `void mt_lock(struct maple_tree *mt)`

Acquire the tree's internal lock.

#### `void mt_unlock(struct maple_tree *mt)`

Release the tree's internal lock.

### Using Together

```c
// Batch several writes atomically:
mt_lock(&mt);
mtree_store(&mt, 10, a);
mtree_store(&mt, 20, b);
void *old = mtree_erase(&mt, 5);
mt_unlock(&mt);

// Read-modify-write under lock:
mt_lock(&mt);
void *cur = mtree_load(&mt, 42);
if (cur == old_val)
    mtree_store(&mt, 42, new_val);
mt_unlock(&mt);

// Single-operation alternative (no manual lock needed):
mtree_lock_store(&mt, 10, a);
```

---

## 7. Locked Convenience Wrappers

The `mtree_lock_*` family acquires the tree's internal lock around each
operation, providing a fully serialised interface safe for concurrent use
from multiple threads without any external synchronisation.

Without `MT_CONFIG_LOCK`, the lock calls compile to no-ops, so these
wrappers remain usable (equivalent to the plain API).

For callers that need to batch several operations atomically, use
`mt_lock()` / `mt_unlock()` manually around the plain API instead.

### Functions

#### `int mtree_lock_store_range(struct maple_tree *mt, uint64_t first, uint64_t last, void *entry)`

Locked version of `mtree_store_range()`.

#### `int mtree_lock_store(struct maple_tree *mt, uint64_t index, void *entry)`

Locked version of `mtree_store()`.

#### `void *mtree_lock_load(struct maple_tree *mt, uint64_t index)`

Locked version of `mtree_load()`.

#### `void *mtree_lock_erase(struct maple_tree *mt, uint64_t index)`

Locked version of `mtree_erase()`.

#### `void *mtree_lock_erase_index(struct maple_tree *mt, uint64_t index)`

Locked version of `mtree_erase_index()`.

#### `void mtree_lock_destroy(struct maple_tree *mt)`

Locked version of `mtree_destroy()`.  Acquires the lock, destroys all
nodes, then releases.

### Using Together

```c
// Each call is independently thread-safe:
mtree_lock_store(&mt, 1, a);
mtree_lock_store(&mt, 2, b);
void *v = mtree_lock_load(&mt, 1);
mtree_lock_erase(&mt, 2);

// Multi-threaded example:
void *writer_thread(void *arg) {
    struct maple_tree *mt = arg;
    for (int i = 0; i < 1000; i++)
        mtree_lock_store(mt, i, VAL(i));
    return NULL;
}

void *reader_thread(void *arg) {
    struct maple_tree *mt = arg;
    for (int i = 0; i < 1000; i++) {
        void *v = mtree_lock_load(mt, i);
        // v is either the expected value or NULL (not yet stored)
    }
    return NULL;
}

// When no more threads are active:
mtree_lock_destroy(&mt);

// For atomic batches (e.g. swap two entries), drop down to raw API:
mt_lock(&mt);
void *a = mtree_load(&mt, 10);
void *b = mtree_load(&mt, 20);
mtree_store(&mt, 10, b);
mtree_store(&mt, 20, a);
mt_unlock(&mt);
```

---

## 8. Iteration Macros

Convenience macros that wrap find/cursor functions into standard C loops.

### Macros

#### `mt_for_each(mt, entry, index, max)`

Iterate all non-NULL entries in `[0, max]` using `mt_find()`.

| Parameter | Type       | Description                                   |
|-----------|------------|-----------------------------------------------|
| `mt`      | `struct maple_tree *` | The tree to iterate.               |
| `entry`   | `void *`   | Loop variable receiving each entry.            |
| `index`   | `uint64_t` | Internal index tracker (modified each loop).   |
| `max`     | `uint64_t` | Upper index bound (inclusive).                 |

#### `mas_for_each(mas, entry, max)`

Iterate entries forward from the current cursor position using
`mas_find()`.

| Parameter | Type       | Description                                   |
|-----------|------------|-----------------------------------------------|
| `mas`     | `struct ma_state *` | Cursor (must be initialised).        |
| `entry`   | `void *`   | Loop variable receiving each entry.            |
| `max`     | `uint64_t` | Upper index bound (inclusive).                 |

### Using Together

```c
// --- Full-tree scan with mt_for_each ---
uint64_t idx = 0;
void *entry;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    printf("entry: %p\n", entry);
}

// --- Bounded scan (entries in [500, 1000] only) ---
idx = 500;
mt_for_each(&mt, entry, idx, 1000) {
    handle(entry);
}

// --- Cursor-based scan with early exit ---
MA_STATE(mas, &mt, 100, 0);      // start scanning from index 100
mas_for_each(&mas, entry, 999) {
    if (some_condition(entry))
        break;                    // cursor remembers position
}
// After break, mas.index / mas.last still reflect the last entry.

// --- Count entries ---
int count = 0;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    count++;
}

// --- Collect into array ---
void *entries[100];
int n = 0;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    if (n < 100)
        entries[n++] = entry;
}
```

---

## 9. Debug

Diagnostic output for inspecting the internal tree structure.  Useful
during development and testing — not intended for production use.

### Functions

#### `void mt_dump_tree(struct maple_tree *mt)`

Print every node to stdout: node type (LEAF / INTERNAL), range, parent
info, slot contents, and raw pivots.

### Using Together

```c
mtree_store_range(&mt, 0, 99, region);
mtree_store(&mt, 200, single);
mt_dump_tree(&mt);   // inspect the tree structure

// Useful after complex operations to verify internal state:
for (int i = 0; i < 100; i++)
    mtree_store(&mt, i * 10, ptrs[i]);
for (int i = 0; i < 50; i++)
    mtree_erase(&mt, i * 10);
mt_dump_tree(&mt);   // verify rebalancing worked correctly
```

---

## Configuration

Build-time options are set in `maple_tree_config.h` or via compiler
flags (`-DMT_CONFIG_LOCK`, `-DMT_CONFIG_RCU`).  The CMake build system
exposes these as options:

| CMake Option     | Default | Activates          | Description                           |
|------------------|---------|--------------------|---------------------------------------|
| `MT_ENABLE_LOCK` | ON      | `MT_CONFIG_LOCK`   | pthread mutex in `struct maple_tree`. |
| `MT_ENABLE_RCU`  | OFF     | `MT_CONFIG_RCU`    | RCU stub functions.                   |
| `MT_BUILD_TESTS` | ON      | —                  | Build test executable.                |
| `ENABLE_ASAN`    | ON      | —                  | Enable AddressSanitizer.              |

### Pluggable Interfaces

Each subsystem can be replaced by defining a `MT_CUSTOM_*` macro before
including `maple_tree_config.h`:

| Macro               | What to provide                                          |
|----------------------|----------------------------------------------------------|
| `MT_CUSTOM_ALLOC`    | `mt_alloc_fn(size)` (must return zeroed memory), `mt_free_fn(ptr)` |
| `MT_CUSTOM_LOCK`     | `mt_lock_t` typedef, `mt_lock_init()`, `mt_spin_lock()`, `mt_spin_unlock()` |
| `MT_CUSTOM_RCU`      | `mt_rcu_read_lock()`, `mt_rcu_read_unlock()`, `mt_call_rcu()` |
| `MT_CUSTOM_BARRIERS` | `mt_smp_rmb()`, `mt_smp_wmb()`, `mt_smp_mb()`            |
