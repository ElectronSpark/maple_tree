# Maple Tree — 独立 B 树库

[English](README.md) | 中文

一个独立的、可移植的 C 语言实现，基于 Linux 内核的 maple tree 数据结构。Maple tree 是一种 B 树，专门用于存储不重叠的整数索引区间到 `void*` 条目的映射，并内置空隙追踪（gap tracking）以支持高效的空闲区间搜索。

> 完整的 API 参考文档请见 [API.zh-CN.md](API.zh-CN.md)。

## 特性

- **10 槽位 B 树节点**，9 个分界键 —— 紧凑且缓存友好
- **区间存储**：`[first, last] → void*`，自动合并相邻同值条目
- **空隙追踪**：O(log n) 查找空闲区间（用于虚拟内存子系统）
- **可选 RCU 安全读取**：可插拔的读侧保护
- **可选加锁**：可插拔的写侧串行化
- **游标 API**（`ma_state`）：基于缓存路径的高效顺序访问和迭代
- **零外部依赖**，仅需 libc（测试需要 cmocka）

## 构建

```bash
mkdir build && cd build
cmake ..
make
```

### CMake 选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `MT_ENABLE_LOCK` | ON | 启用 `maple_tree` 中基于 pthread 的互斥锁 |
| `MT_ENABLE_RCU`  | OFF | 启用 RCU 桩函数 |
| `MT_BUILD_TESTS` | ON  | 构建测试可执行文件 |
| `ENABLE_ASAN`    | ON  | 启用 AddressSanitizer |

### 运行测试

```bash
cd build && ./test_maple_tree
```

## 移植到你的项目

### 1. 复制文件

```
include/maple_tree.h          — 公开 API
include/maple_tree_type.h     — 结构体定义
include/maple_tree_config.h   — 可插拔配置
src/maple_tree.c              — 实现
```

### 2. 自定义分配器

定义 `MT_CUSTOM_ALLOC` 并提供：
```c
#define MT_CUSTOM_ALLOC
void *mt_alloc_fn(size_t size);   // 必须返回清零后的内存
void  mt_free_fn(void *ptr);
```

### 3. 自定义加锁（可选）

定义 `MT_CUSTOM_LOCK` 并提供：
```c
#define MT_CUSTOM_LOCK
typedef your_lock_t mt_lock_t;
void mt_lock_init(mt_lock_t *lock);
void mt_spin_lock(mt_lock_t *lock);
void mt_spin_unlock(mt_lock_t *lock);
```

同时定义 `MT_CONFIG_LOCK` 以在 `struct maple_tree` 中包含锁字段。

### 4. 自定义 RCU（可选）

定义 `MT_CUSTOM_RCU` 并提供：
```c
#define MT_CUSTOM_RCU
void mt_rcu_read_lock(void);
void mt_rcu_read_unlock(void);
void mt_call_rcu(void (*fn)(void*), void *data);
```

同时定义 `MT_CONFIG_RCU` 以启用 RCU 保护的指针访问。

### 5. 自定义内存屏障（可选）

定义 `MT_CUSTOM_BARRIERS` 并提供：
```c
#define MT_CUSTOM_BARRIERS
void mt_smp_rmb(void);   // load-load 屏障
void mt_smp_wmb(void);   // store-store 屏障
void mt_smp_mb(void);    // 全屏障
```

## API 快速参考

完整文档请见 [API 参考](API.zh-CN.md)。

### 简单 API
```c
mt_init(&mt);
mtree_store(&mt, index, entry);
mtree_store_range(&mt, first, last, entry);
void *e = mtree_load(&mt, index);
void *old = mtree_erase(&mt, index);           // 删除整个区间条目
void *old2 = mtree_erase_index(&mt, index);    // 仅删除单个索引
mtree_destroy(&mt);
```

### 游标 API
```c
MA_STATE(mas, &mt, index, last);
mas_walk(&mas);
mas_store(&mas, entry);
mas_erase(&mas);
mas_find(&mas, max);
mas_next(&mas, max);
mas_prev(&mas, min);
```

`ma_state` 会在固定大小的路径缓存中保存根到当前节点的 walk，因此 `mas_next()` /
`mas_prev()` 可以跨相邻子树移动，而不必每一步都完整重走一遍树。带边界的游标操
作在越界 miss 时会使游标失效，而不会把它留在过期槽位上。

### 空隙搜索
```c
MA_STATE(mas, &mt, 0, 0);
mas_empty_area(&mas, min, max, size);       // 正向首次适配
mas_empty_area_rev(&mas, min, max, size);   // 反向首次适配
// 结果：mas.index .. mas.last
```

### 迭代
```c
void *entry;
uint64_t index = 0;
mt_for_each(&mt, entry, index, max) {
    // 处理 entry
}
```

### 加锁封装
```c
mtree_lock_store(&mt, index, entry);        // 线程安全的单操作
mtree_lock_load(&mt, index);
mtree_lock_erase(&mt, index);
mtree_lock_erase_index(&mt, index);
```

## 许可证

MIT 许可证。
