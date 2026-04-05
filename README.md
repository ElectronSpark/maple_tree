# Maple Tree — Standalone B-tree Library

A standalone, portable C implementation of the Linux kernel maple tree data structure. The maple tree is a B-tree optimized for storing non-overlapping index ranges mapping to `void*` entries, with gap tracking for efficient free-range search.

## Features

- **16-slot B-tree nodes** with 15 pivots — compact and cache-friendly
- **Range storage**: `[first, last] → void*` with automatic coalescing
- **Gap tracking**: O(log n) search for free ranges (used by VM subsystems)
- **Optional RCU-safe reads**: pluggable read-side protection
- **Optional locking**: pluggable write-side serialization
- **Cursor API** (`ma_state`): efficient sequential access and iteration
- **Zero external dependencies** beyond libc (and cmocka for tests)

## Building

```bash
mkdir build && cd build
cmake ..
make
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `MT_ENABLE_LOCK` | OFF | Enable pthread-based spin lock in `maple_tree` |
| `MT_ENABLE_RCU`  | OFF | Enable RCU stub functions |
| `MT_BUILD_TESTS` | ON  | Build test executable |
| `ENABLE_ASAN`    | ON  | Enable AddressSanitizer |

### Running Tests

```bash
cd build && ./test_maple_tree
```

## Porting to Your Project

### 1. Copy files

```
include/maple_tree.h          — public API
include/maple_tree_type.h     — struct definitions
include/maple_tree_config.h   — pluggable configuration
src/maple_tree.c              — implementation
```

### 2. Customize the allocator

Define `MT_CUSTOM_ALLOC` and provide:
```c
#define MT_CUSTOM_ALLOC
void *mt_alloc_fn(size_t size);   // must return zeroed memory
void  mt_free_fn(void *ptr);
```

### 3. Customize locking (optional)

Define `MT_CUSTOM_LOCK` and provide:
```c
#define MT_CUSTOM_LOCK
typedef your_lock_t mt_lock_t;
void mt_lock_init(mt_lock_t *lock);
void mt_spin_lock(mt_lock_t *lock);
void mt_spin_unlock(mt_lock_t *lock);
```

Also define `MT_CONFIG_LOCK` to include the lock field in `struct maple_tree`.

### 4. Customize RCU (optional)

Define `MT_CUSTOM_RCU` and provide:
```c
#define MT_CUSTOM_RCU
void mt_rcu_read_lock(void);
void mt_rcu_read_unlock(void);
void mt_call_rcu(void (*fn)(void*), void *data);
```

Also define `MT_CONFIG_RCU` to enable RCU-protected pointer access.

### 5. Customize memory barriers (optional)

Define `MT_CUSTOM_BARRIERS` and provide:
```c
#define MT_CUSTOM_BARRIERS
void mt_smp_rmb(void);   // load-load barrier
void mt_smp_wmb(void);   // store-store barrier
void mt_smp_mb(void);    // full barrier
```

## API Quick Reference

### Simple API
```c
mt_init(&mt);
mtree_store(&mt, index, entry);
mtree_store_range(&mt, first, last, entry);
void *e = mtree_load(&mt, index);
void *old = mtree_erase(&mt, index);
mtree_destroy(&mt);
```

### Cursor API
```c
MA_STATE(mas, &mt, index, last);
mas_walk(&mas);
mas_store(&mas, entry);
mas_erase(&mas);
mas_find(&mas, max);
mas_next(&mas, max);
mas_prev(&mas, min);
```

### Gap Search
```c
MA_STATE(mas, &mt, 0, 0);
mas_empty_area(&mas, min, max, size);       // first fit forward
mas_empty_area_rev(&mas, min, max, size);   // first fit reverse
// Result: mas.index .. mas.last
```

### Iteration
```c
void *entry;
uint64_t index = 0;
mt_for_each(&mt, entry, index, max) {
    // process entry
}
```

## License

MIT License.
