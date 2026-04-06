# Maple Tree API 参考

[English](API.md) | 中文

Maple tree 是一种 B 树，将不重叠的 `uint64_t` 索引区间映射到 `void*` 条目。
支持自动节点分裂/合并、O(log n) 空隙搜索，以及可选的 RCU 安全读取。

基于 Linux 内核 maple tree (6.x)，适配为独立的 C11 实现，无内核依赖。

---

## 目录

1. [数据结构](#数据结构)
2. [初始化](#1-初始化)
3. [简单 API](#2-简单-api)
4. [游标 API](#3-游标-api)
5. [空隙搜索](#4-空隙搜索)
6. [RCU 读侧辅助函数](#5-rcu-读侧辅助函数)
7. [加锁](#6-加锁)
8. [加锁封装](#7-加锁便捷封装)
9. [迭代宏](#8-迭代宏)
10. [调试](#9-调试)
11. [构建配置](#构建配置)

---

## 数据结构

### `struct maple_tree`

Maple tree 的根结构体。

| 字段       | 类型          | 说明                                      |
|------------|---------------|-------------------------------------------|
| `ma_root`  | `void *`      | 带标记的根指针（空树时为 NULL）。           |
| `ma_flags` | `unsigned int`| 保留的特性标志。                           |
| `ma_lock`  | `mt_lock_t`   | 每棵树的锁（仅当定义 `MT_CONFIG_LOCK`）。  |

在栈上分配或嵌入到你自己的结构体中，然后在首次使用前调用 `mt_init()`。

### `struct ma_state`（游标）

栈上分配的游标，用于在树遍历中追踪位置，并缓存完整的根到当前节点路径，便于高效
顺序访问。始终通过 `MA_STATE()` 宏在栈上分配。

| 字段     | 类型                 | 说明                                              |
|----------|----------------------|---------------------------------------------------|
| `tree`   | `struct maple_tree*` | 该游标操作的树。                                   |
| `index`  | `uint64_t`           | 搜索/存储区间的起始索引，或下一次重试的位置。       |
| `last`   | `uint64_t`           | 搜索/存储区间的结束索引（包含）。                   |
| `node`   | `struct maple_node*` | 当前定位的节点，通常是拥有 `offset` 的叶子节点。    |
| `min`    | `uint64_t`           | 当前槽位覆盖的最小索引（包含）。                    |
| `max`    | `uint64_t`           | 当前槽位覆盖的最大索引（包含）。                    |
| `offset` | `uint8_t`            | 在 `node` 中的槽位偏移。                           |
| `depth`  | `uint8_t`            | `path[]` 中当前有效缓存帧的数量。                   |
| `path`   | `struct maple_path_frame[32]` | `mas_next()` / `mas_prev()` 使用的根到节点缓存路径。 |

### `MA_STATE(name, mt, first, end)`

`struct ma_state` 的栈初始化宏。

```c
MA_STATE(mas, &mt, 0, 0);   // 创建名为 'mas' 的游标，从索引 0 开始
```

### 常量

| 名称               | 值                | 说明                          |
|--------------------|-------------------|-------------------------------|
| `MAPLE_NODE_SLOTS` | 10                | 分支因子（每节点的槽位数）。    |
| `MAPLE_NODE_PIVOTS`| 9                 | 每节点的分界键数。              |
| `MAPLE_CURSOR_MAX_DEPTH` | 32         | 游标缓存路径的最大深度。        |
| `MAPLE_MIN`        | 0                 | 最小索引值。                    |
| `MAPLE_MAX`        | `~(uint64_t)0`   | 最大索引值。                    |

---

## 1. 初始化

在首次使用前设置 maple tree，或检查其是否为空。

### 函数

#### `void mt_init(struct maple_tree *mt)`

初始化一棵空的 maple tree。将根指针设为 NULL，标志设为 0，并初始化每棵树的锁
（当定义了 `MT_CONFIG_LOCK` 时）。

**必须在任何其他操作之前调用。**

#### `void mt_init_flags(struct maple_tree *mt, unsigned int flags)`

与 `mt_init()` 相同，但会将 `flags` 存储到 `mt->ma_flags` 中。标志目前保留
供将来使用。

#### `bool mt_empty(const struct maple_tree *mt)`

如果树中没有条目则返回 `true`。

### 组合使用

```c
struct maple_tree mt;
mt_init(&mt);                   // 首次使用前必须调用
assert(mt_empty(&mt));          // 初始为空

mtree_store(&mt, 0, ptr);       // 插入条目
assert(!mt_empty(&mt));

mtree_destroy(&mt);             // 释放所有内部节点
assert(mt_empty(&mt));          // 再次为空

// 可以重新初始化并复用：
mt_init(&mt);
```

---

## 2. 简单 API

最常用的操作，每个函数一次调用完成。这些函数**不会**在内部获取任何锁——调用方
必须自行串行化写操作（参见[加锁](#6-加锁)）。

### 函数

#### `void *mtree_load(struct maple_tree *mt, uint64_t index)`

查找范围包含 `index` 的条目。

- **返回值：** 存储的 `void*` 条目；如果该索引落在空隙中（没有条目覆盖），
  返回 `NULL`。

#### `int mtree_store_range(struct maple_tree *mt, uint64_t first, uint64_t last, void *entry)`

为连续区间 `[first, last]` 插入或覆盖条目。

- `entry` 可以为 `NULL`——这会在树中打一个空洞（显式空隙），实质上删除区间
  内重叠的所有条目。
- 重叠的条目会被自动拆分或替换。
- 具有相同指针值的相邻槽位会被自动合并。
- 节点满时会触发分裂；树高度会按需增长。
- **返回值：** 成功返回 `0`，`first > last` 返回 `-EINVAL`，分配失败返回
  `-ENOMEM`。

#### `int mtree_store(struct maple_tree *mt, uint64_t index, void *entry)`

便捷封装：`mtree_store_range(mt, index, index, entry)`。

- **返回值：** 成功返回 `0`，失败返回负的 errno。

#### `void *mtree_erase(struct maple_tree *mt, uint64_t index)`

删除范围包含 `index` 的**整个**条目。

> **重要：** 此函数删除整个区间，而不仅仅是单个索引。如果存储了 `[10, 29]`，
> 调用 `mtree_erase(mt, 20)` 会删除整个 `[10, 29]` 条目。如果只想打一个
> 单索引空洞，请使用 `mtree_erase_index(mt, 20)`。

删除后，不足的节点会被重新平衡（与兄弟节点合并或重新分配），树高度可能会缩小。

- **返回值：** 之前存储的 `void*`；如果没有条目覆盖 `index`，返回 `NULL`。

#### `void *mtree_erase_index(struct maple_tree *mt, uint64_t index)`

从树中删除单个索引，保留区间中其余部分。

与 `mtree_erase()` 不同，此函数仅在 `index` 位置打一个空洞。例如，如果存储了
`[10, 29]`，调用 `mtree_erase_index(mt, 20)` 后结果为两个条目：`[10, 19]`
和 `[21, 29]`，各自保留原始指针值。索引 20 变为空隙。

如果 `index` 是条目中唯一的索引（点条目），行为与 `mtree_erase()` 相同。

- **返回值：** `index` 处之前存储的条目；如果没有条目覆盖 `index`，返回 `NULL`。

#### `void mtree_destroy(struct maple_tree *mt)`

递归释放所有内部节点。返回后 `mt_empty()` 为 `true`。

- **不会**释放用户提供的条目指针——只释放内部节点结构。
- 对已经为空的树调用是安全的。
- 调用方必须确保没有并发的读取者或写入者。

### 组合使用

```c
struct maple_tree mt;
mt_init(&mt);

// --- 存储条目 ---
mtree_store_range(&mt, 100, 199, region_a);   // 区间 [100, 199]
mtree_store(&mt, 42, single_obj);             // 单索引 42

// --- 查找 ---
void *v = mtree_load(&mt, 150);               // 返回 region_a
assert(mtree_load(&mt, 50) == NULL);           // 空隙 → NULL

// --- 删除整个区间条目 ---
void *old = mtree_erase(&mt, 150);             // 删除整个 [100, 199]
assert(old == region_a);
assert(mtree_load(&mt, 100) == NULL);          // 已删除

// --- 打单索引空洞 ---
mtree_store_range(&mt, 100, 199, region_b);    // 重新插入
void *prev = mtree_erase_index(&mt, 150);      // 仅删除索引 150
assert(prev == region_b);
assert(mtree_load(&mt, 149) == region_b);       // 仍然存在
assert(mtree_load(&mt, 150) == NULL);           // 空洞
assert(mtree_load(&mt, 151) == region_b);       // 仍然存在

// --- 清理 ---
mtree_destroy(&mt);
```

---

## 3. 游标 API

游标 API 使用栈上的 `struct ma_state`（通过 `MA_STATE()` 宏创建）来追踪树内
的位置。游标会缓存精确的根到节点路径，因此顺序操作可以复用该路径横向移动、再
次下探，而不必在每一步都重新推导父节点边界。

### 函数

#### `void *mas_walk(struct ma_state *mas)`

从根节点下降到覆盖 `mas->index` 的叶节点槽位。

返回时，`mas->node`、`mas->offset`、`mas->min`、`mas->max`、`mas->depth`
以及缓存的 `mas->path[]` 帧都会更新。

- **返回值：** 该位置的条目；如果是空隙则返回 `NULL`。

#### `int mas_store(struct ma_state *mas, void *entry)`

在 `[mas->index, mas->last]` 存储条目，等同于
`mtree_store_range(mas->tree, mas->index, mas->last, entry)`。

- **返回值：** 成功返回 `0`，失败返回负的 errno。

#### `void *mas_erase(struct ma_state *mas)`

删除 `mas->index` 处的条目，等同于 `mtree_erase(mas->tree, mas->index)`。

- **返回值：** 之前存储的条目；如果为空则返回 `NULL`。

#### `void *mas_find(struct ma_state *mas, uint64_t max)`

查找 `mas->index` 处或之后（直到 `max`）的下一个非 NULL 条目。

如果游标尚未 walk 过（`mas->node == NULL`），会先自动执行 `mas_walk()`。

返回后，`mas->index` 会前进到找到的条目之后（到 `mas->max + 1`），因此重复
调用可以向前迭代。如果下一个候选条目超出边界，游标会被失效，而不是停留在越界
槽位上。

- **返回值：** 下一个非 NULL 条目；如果在 `[mas->index, max]` 内不存在则返回
  `NULL`。

#### `void *mas_next(struct ma_state *mas, uint64_t max)`

前进到当前位置之后的下一个非 NULL 条目。

游标必须已经定位（例如通过 `mas_walk()`）。实现会复用缓存路径，向上回溯到兄弟
子树，再下探到下一个条目。

- **返回值：** 下一个非 NULL 条目；如果到 `max` 都没有则返回 `NULL`。若因边界
    命中失败而停止，游标会被失效。

#### `void *mas_prev(struct ma_state *mas, uint64_t min)`

移动到当前位置之前的上一个非 NULL 条目。

- **返回值：** 上一个非 NULL 条目；如果到 `min` 都没有则返回 `NULL`。若因边界
    命中失败而停止，游标会被失效。

### 组合使用

```c
// --- Walk + 条件更新 ---
MA_STATE(mas, &mt, target_idx, target_idx);
void *cur = mas_walk(&mas);
if (cur == stale_ptr)
    mas_store(&mas, new_ptr);       // 原地覆盖

// --- 使用游标正向扫描 ---
MA_STATE(mas, &mt, 0, 0);
void *entry;
while ((entry = mas_find(&mas, UINT64_MAX)) != NULL) {
    // mas.index / mas.last 给出条目的区间边界
    printf("[%lu, %lu] -> %p\n", mas.index, mas.last, entry);
}

// --- 反向扫描 ---
MA_STATE(mas, &mt, UINT64_MAX, UINT64_MAX);
mas_walk(&mas);                     // 定位到末尾
while ((entry = mas_prev(&mas, 0)) != NULL) {
    // 以反向顺序处理条目
}

// --- 在迭代中删除 ---
MA_STATE(mas, &mt, 0, 0);
while ((entry = mas_find(&mas, 1000)) != NULL) {
    if (should_remove(entry))
        mas_erase(&mas);            // 在迭代中安全删除
}

// --- 使用游标批量插入 ---
MA_STATE(mas, &mt, 0, 9);
mas_store(&mas, region_a);           // [0, 9]
mas.index = 10; mas.last = 19;
mas_store(&mas, region_b);           // [10, 19]
```

---

## 4. 空隙搜索

空隙搜索函数在指定的索引范围内查找连续的 NULL 条目（空隙）。它们利用每个节点
的空隙元数据实现 O(log n) 搜索，非常适合 VM 风格的地址空间分配器。

### 函数

#### `int mas_empty_area(struct ma_state *mas, uint64_t min, uint64_t max, uint64_t size)`

在 `[min, max]` 范围内查找第一个（最低地址）至少有 `size` 个连续 NULL 条目
的空隙。

成功时，`mas->index` 和 `mas->last` 被设置为找到的空隙范围
`[start, start + size - 1]`。

- `size` 必须 > 0。
- **返回值：** 成功返回 `0`；如果没有合适的空隙返回 `-EBUSY`。

#### `int mas_empty_area_rev(struct ma_state *mas, uint64_t min, uint64_t max, uint64_t size)`

与 `mas_empty_area()` 相同，但反向搜索，返回满足大小要求的最高地址空隙。

- **返回值：** 成功返回 `0`；如果没有合适的空隙返回 `-EBUSY`。

### 组合使用

```c
// --- 自底向上（首次适配）分配器 ---
MA_STATE(mas, &mt, 0, 0);
int ret = mas_empty_area(&mas, 0, UINT64_MAX, page_count);
if (ret == 0) {
    // 找到：空隙在 [mas.index, mas.last]
    uint64_t alloc_start = mas.index;
    mtree_store_range(&mt, alloc_start,
                      alloc_start + page_count - 1, vma);
}

// --- 自顶向下（反向）分配器 ---
MA_STATE(mas, &mt, 0, 0);
ret = mas_empty_area_rev(&mas, 0, UINT64_MAX, page_count);
if (ret == 0) {
    uint64_t alloc_start = mas.index;
    mtree_store_range(&mt, alloc_start,
                      alloc_start + page_count - 1, vma);
}

// --- 释放区域（空隙回收） ---
mtree_store_range(&mt, alloc_start,
                  alloc_start + page_count - 1, NULL);
// 后续的空隙搜索将再次找到此区域

// --- 分配循环：填满为止 ---
while (1) {
    MA_STATE(mas, &mt, 0, 0);
    if (mas_empty_area(&mas, 0, MAX_ADDR, 4096) != 0)
        break;  // -EBUSY，没有更多空间
    mtree_store_range(&mt, mas.index, mas.last, new_region());
}
```

---

## 5. RCU 读侧辅助函数

这些函数提供了一条自包含的读取路径，在内部获取 RCU 读锁（当启用
`MT_CONFIG_RCU` 时）。适合读取线程与写入者共存时的一次性查找或邻居查询。

未启用 `MT_CONFIG_RCU` 时，RCU 锁调用编译为空操作，这些函数的行为与基于游标
的查找完全相同——但调用方仍须针对并发写操作进行串行化。

### 函数

#### `void *mt_find(struct maple_tree *mt, uint64_t *index, uint64_t max)`

在 `*index` 处或之后查找下一个非 NULL 条目，最远到 `max`。

返回时 `*index` 被更新为找到条目的区间末尾之后的位置，因此重复调用可以遍历
所有条目。

- **返回值：** 找到的条目；如果在 `[*index, max]` 中不存在非 NULL 条目，
  返回 `NULL`。

#### `void *mt_next(struct maple_tree *mt, uint64_t index, uint64_t max)`

返回严格在 `index` 之后的下一个非 NULL 条目。

- **返回值：** 下一个条目；如果 `index >= max` 或 `(index, max]` 中不存在
  条目，返回 `NULL`。

#### `void *mt_prev(struct maple_tree *mt, uint64_t index, uint64_t min)`

返回严格在 `index` 之前的上一个非 NULL 条目。

- **返回值：** 上一个条目；如果 `index <= min` 或 `[min, index)` 中不存在
  条目，返回 `NULL`。

### RCU 锁函数

#### `void mt_rcu_lock(void)` / `void mt_rcu_unlock(void)`

进入/离开 RCU 读侧临界区。仅在 RCU 区段内跨多次调用使用游标 API 时需要。
`mt_find`/`mt_next`/`mt_prev` 在内部自动获取 RCU。

### 组合使用

```c
// --- 使用 mt_find 遍历所有条目 ---
uint64_t idx = 0;
void *entry;
while ((entry = mt_find(&mt, &idx, UINT64_MAX)) != NULL)
    process(entry);        // idx 自动前进到每个条目之后

// --- 获取已知索引的邻居 ---
void *next = mt_next(&mt, current_idx, UINT64_MAX);
void *prev = mt_prev(&mt, current_idx, 0);

// --- 有界搜索：[1000, 2000] 中的第一个条目 ---
uint64_t start = 1000;
void *e = mt_find(&mt, &start, 2000);
if (e)
    printf("在位置 %lu 之前找到\n", start);

// --- 显式 RCU 区段用于基于游标的读取 ---
mt_rcu_lock();
MA_STATE(mas, &mt, 0, 0);
void *entry = mas_find(&mas, UINT64_MAX);
// ... 安全地使用 entry ...
mt_rcu_unlock();
```

---

## 6. 加锁

每棵树互斥锁的低级加锁/解锁。当定义了 `MT_CONFIG_LOCK` 时可用；否则两者都
编译为空操作。

大多数调用方应优先使用 `mtree_lock_*` 封装（参见
[加锁便捷封装](#7-加锁便捷封装)）进行单操作加锁。仅在需要将多个操作批量放入
一个临界区时才直接使用 `mt_lock()`/`mt_unlock()`。

### 函数

#### `void mt_lock(struct maple_tree *mt)`

获取树的内部锁。

#### `void mt_unlock(struct maple_tree *mt)`

释放树的内部锁。

### 组合使用

```c
// 原子地批量执行多次写操作：
mt_lock(&mt);
mtree_store(&mt, 10, a);
mtree_store(&mt, 20, b);
void *old = mtree_erase(&mt, 5);
mt_unlock(&mt);

// 在锁保护下进行读-改-写：
mt_lock(&mt);
void *cur = mtree_load(&mt, 42);
if (cur == old_val)
    mtree_store(&mt, 42, new_val);
mt_unlock(&mt);

// 单操作替代方案（不需要手动加锁）：
mtree_lock_store(&mt, 10, a);
```

---

## 7. 加锁便捷封装

`mtree_lock_*` 系列在每个操作前后获取/释放树的内部锁，提供完全串行化的接口，
可在多线程中安全使用，无需任何外部同步。

未启用 `MT_CONFIG_LOCK` 时，锁调用编译为空操作，因此这些封装仍然可用（等同于
普通 API）。

对于需要原子地批量执行多个操作的调用方，请改用 `mt_lock()` / `mt_unlock()`
手动加锁并包裹普通 API。

### 函数

#### `int mtree_lock_store_range(struct maple_tree *mt, uint64_t first, uint64_t last, void *entry)`

`mtree_store_range()` 的加锁版本。

#### `int mtree_lock_store(struct maple_tree *mt, uint64_t index, void *entry)`

`mtree_store()` 的加锁版本。

#### `void *mtree_lock_load(struct maple_tree *mt, uint64_t index)`

`mtree_load()` 的加锁版本。

#### `void *mtree_lock_erase(struct maple_tree *mt, uint64_t index)`

`mtree_erase()` 的加锁版本。

#### `void *mtree_lock_erase_index(struct maple_tree *mt, uint64_t index)`

`mtree_erase_index()` 的加锁版本。

#### `void mtree_lock_destroy(struct maple_tree *mt)`

`mtree_destroy()` 的加锁版本。获取锁，销毁所有节点，然后释放锁。

### 组合使用

```c
// 每次调用都是独立线程安全的：
mtree_lock_store(&mt, 1, a);
mtree_lock_store(&mt, 2, b);
void *v = mtree_lock_load(&mt, 1);
mtree_lock_erase(&mt, 2);
mtree_lock_erase_index(&mt, 1);    // 仅删除单个索引

// 多线程示例：
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
        // v 要么是预期值，要么是 NULL（尚未存储）
    }
    return NULL;
}

// 不再有线程活动时：
mtree_lock_destroy(&mt);

// 对于原子批操作（例如交换两个条目），降级到原始 API：
mt_lock(&mt);
void *a = mtree_load(&mt, 10);
void *b = mtree_load(&mt, 20);
mtree_store(&mt, 10, b);
mtree_store(&mt, 20, a);
mt_unlock(&mt);
```

---

## 8. 迭代宏

将查找/游标函数包装成标准 C 循环的便捷宏。

### 宏

#### `mt_for_each(mt, entry, index, max)`

使用 `mt_find()` 遍历 `[0, max]` 中的所有非 NULL 条目。

| 参数    | 类型                  | 说明                                    |
|---------|-----------------------|-----------------------------------------|
| `mt`    | `struct maple_tree *` | 要遍历的树。                             |
| `entry` | `void *`              | 接收每个条目的循环变量。                  |
| `index` | `uint64_t`            | 内部索引追踪器（每次循环都会修改）。       |
| `max`   | `uint64_t`            | 索引上界（包含）。                        |

#### `mas_for_each(mas, entry, max)`

使用 `mas_find()` 从当前游标位置向前遍历条目。

| 参数    | 类型                | 说明                                    |
|---------|---------------------|-----------------------------------------|
| `mas`   | `struct ma_state *` | 游标（必须已初始化）。                   |
| `entry` | `void *`            | 接收每个条目的循环变量。                  |
| `max`   | `uint64_t`          | 索引上界（包含）。                        |

### 组合使用

```c
// --- 使用 mt_for_each 全树扫描 ---
uint64_t idx = 0;
void *entry;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    printf("条目: %p\n", entry);
}

// --- 有界扫描（仅 [500, 1000] 中的条目） ---
idx = 500;
mt_for_each(&mt, entry, idx, 1000) {
    handle(entry);
}

// --- 基于游标的扫描，支持提前退出 ---
MA_STATE(mas, &mt, 100, 0);      // 从索引 100 开始扫描
mas_for_each(&mas, entry, 999) {
    if (some_condition(entry))
        break;                    // 游标会记住位置
}
// break 后，mas.index / mas.last 仍反映最后一个条目

// --- break 后继续迭代 ---
// 游标保留了位置，可以直接继续下一轮扫描
mas_for_each(&mas, entry, 999) {
    process_remaining(entry);
}

// --- 计数条目 ---
int count = 0;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    count++;
}

// --- 收集到数组 ---
void *entries[100];
int n = 0;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    if (n < 100)
        entries[n++] = entry;
}

// --- 按条件过滤 ---
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    struct vma *vma = entry;
    if (vma->flags & VM_WRITE)
        printf("可写区域: %p\n", vma);
}

// --- 查找第一个满足条件的条目 ---
void *found = NULL;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    if (matches(entry)) {
        found = entry;
        break;
    }
}

// --- 在迭代中删除（必须用 mas_for_each） ---
// mt_for_each 无法安全删除，因为没有游标。
// 使用 mas_for_each 配合 mas_erase() 实现：
MA_STATE(mas, &mt, 0, 0);
mas_for_each(&mas, entry, UINT64_MAX) {
    if (should_remove(entry))
        mas_erase(&mas);          // 安全：游标会正确前进
}

// --- 在迭代中替换条目 ---
MA_STATE(mas, &mt, 0, 0);
mas_for_each(&mas, entry, UINT64_MAX) {
    if (needs_update(entry)) {
        void *new_entry = transform(entry);
        mas_store(&mas, new_entry);
    }
}

// --- 聚合计算：求总大小 ---
uint64_t total_size = 0;
idx = 0;
mt_for_each(&mt, entry, idx, UINT64_MAX) {
    struct region *r = entry;
    total_size += r->size;
}

// --- 分页遍历：每次最多处理 N 个条目 ---
#define PAGE_SIZE 64
uint64_t cursor = 0;
int processed;
do {
    processed = 0;
    mt_for_each(&mt, entry, cursor, UINT64_MAX) {
        process(entry);
        if (++processed >= PAGE_SIZE)
            break;                // cursor 已前进，下次从这里继续
    }
} while (processed == PAGE_SIZE);

// --- 查找特定范围内的最后一个条目 ---
void *last_entry = NULL;
idx = 0;
mt_for_each(&mt, entry, idx, 1000) {
    last_entry = entry;           // 不断覆盖，循环结束后即为最后一个
}

// --- 两棵树的交集：查找两棵树中都存在条目的索引 ---
uint64_t i = 0;
mt_for_each(&mt_a, entry, i, UINT64_MAX) {
    // mt_for_each 将 i 推进到条目之后，回退到条目起始
    void *other = mtree_load(&mt_b, i - 1);
    if (other != NULL)
        handle_overlap(entry, other);
}

// --- 使用 mas_for_each 执行带上下文的批量操作 ---
MA_STATE(mas, &mt, 0, 0);
int batch = 0;
mas_for_each(&mas, entry, UINT64_MAX) {
    enqueue(entry);
    if (++batch == 32) {
        flush_batch();            // 每 32 个条目刷新一次
        batch = 0;
    }
}
if (batch > 0)
    flush_batch();                // 处理剩余条目
```

---

## 9. 调试

用于检查内部树结构的诊断输出。在开发和测试时有用——不应用于生产环境。

### 函数

#### `void mt_dump_tree(struct maple_tree *mt)`

将每个节点打印到标准输出：节点类型（LEAF / INTERNAL）、范围、父节点信息、
槽位内容和原始分界键。

### 组合使用

```c
mtree_store_range(&mt, 0, 99, region);
mtree_store(&mt, 200, single);
mt_dump_tree(&mt);   // 查看树结构

// 在复杂操作后验证内部状态很有用：
for (int i = 0; i < 100; i++)
    mtree_store(&mt, i * 10, ptrs[i]);
for (int i = 0; i < 50; i++)
    mtree_erase(&mt, i * 10);
mt_dump_tree(&mt);   // 验证重新平衡是否正确
```

---

## 构建配置

构建时选项在 `maple_tree_config.h` 中设置，或通过编译器标志
（`-DMT_CONFIG_LOCK`、`-DMT_CONFIG_RCU`）指定。CMake 构建系统将这些暴露为
选项：

| CMake 选项       | 默认值 | 激活宏              | 说明                                     |
|------------------|--------|---------------------|------------------------------------------|
| `MT_ENABLE_LOCK` | ON     | `MT_CONFIG_LOCK`    | 在 `struct maple_tree` 中启用 pthread 互斥锁。 |
| `MT_ENABLE_RCU`  | OFF    | `MT_CONFIG_RCU`     | 启用 RCU 桩函数。                         |
| `MT_BUILD_TESTS` | ON     | —                   | 构建测试可执行文件。                       |
| `ENABLE_ASAN`    | ON     | —                   | 启用 AddressSanitizer。                    |

### 可插拔接口

每个子系统都可以通过在引入 `maple_tree_config.h` 之前定义 `MT_CUSTOM_*` 宏来
替换：

| 宏                  | 需要提供的内容                                            |
|---------------------|----------------------------------------------------------|
| `MT_CUSTOM_ALLOC`   | `mt_alloc_fn(size)`（必须返回清零的内存）、`mt_free_fn(ptr)` |
| `MT_CUSTOM_LOCK`    | `mt_lock_t` 类型定义、`mt_lock_init()`、`mt_spin_lock()`、`mt_spin_unlock()` |
| `MT_CUSTOM_RCU`     | `mt_rcu_read_lock()`、`mt_rcu_read_unlock()`、`mt_call_rcu()` |
| `MT_CUSTOM_BARRIERS`| `mt_smp_rmb()`、`mt_smp_wmb()`、`mt_smp_mb()`            |
