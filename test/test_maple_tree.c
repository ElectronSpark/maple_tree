/**
 * @file test_maple_tree.c
 * @brief Comprehensive tests for the standalone maple tree library.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "maple_tree.h"

/* -- Helpers ----------------------------------------------------------- */

#define VAL(n) ((void *)(uintptr_t)(n))

static void init_tree(struct maple_tree *mt) { mt_init(mt); }

/* -- Basic tests ------------------------------------------------------- */

static void test_empty_tree(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_true(mt_empty(&mt));
    assert_null(mtree_load(&mt, 0));
    assert_null(mtree_load(&mt, 42));
    assert_null(mtree_load(&mt, MAPLE_MAX));
    mtree_destroy(&mt);
}

static void test_single_store_load(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store(&mt, 5, VAL(100)));
    assert_ptr_equal(VAL(100), mtree_load(&mt, 5));
    assert_null(mtree_load(&mt, 4));
    assert_null(mtree_load(&mt, 6));
    mtree_destroy(&mt);
}

static void test_store_at_zero(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store(&mt, 0, VAL(1)));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 0));
    assert_null(mtree_load(&mt, 1));
    mtree_destroy(&mt);
}

static void test_store_range(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store_range(&mt, 10, 19, VAL(42)));
    for (uint64_t i = 10; i <= 19; i++)
        assert_ptr_equal(VAL(42), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 9));
    assert_null(mtree_load(&mt, 20));
    mtree_destroy(&mt);
}

static void test_overwrite(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 5, VAL(1));
    mtree_store(&mt, 5, VAL(2));
    assert_ptr_equal(VAL(2), mtree_load(&mt, 5));
    mtree_destroy(&mt);
}

static void test_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 5, VAL(1));
    void *old = mtree_erase(&mt, 5);
    assert_ptr_equal(VAL(1), old);
    assert_null(mtree_load(&mt, 5));
    mtree_destroy(&mt);
}

static void test_erase_nonexistent(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    void *old = mtree_erase(&mt, 99);
    assert_null(old);
    mtree_destroy(&mt);
}

/* -- Sequential insert (triggers node splitting) ----------------------- */

static void test_sequential_insert(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 100; i++)
        assert_int_equal(0, mtree_store(&mt, i, VAL(i + 1)));
    for (uint64_t i = 0; i < 100; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 100));
    mtree_destroy(&mt);
}

static void test_reverse_insert(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (int i = 99; i >= 0; i--)
        assert_int_equal(0, mtree_store(&mt, (uint64_t)i, VAL(i + 1)));
    for (uint64_t i = 0; i < 100; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* -- Range operations -------------------------------------------------- */

static void test_overlapping_range_store(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    mtree_store_range(&mt, 5, 14, VAL(2));
    for (uint64_t i = 0; i <= 4; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    for (uint64_t i = 5; i <= 14; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 15));
    mtree_destroy(&mt);
}

static void test_range_erase_via_store_null(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    mtree_store_range(&mt, 3, 6, NULL);
    assert_ptr_equal(VAL(1), mtree_load(&mt, 0));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 2));
    assert_null(mtree_load(&mt, 3));
    assert_null(mtree_load(&mt, 6));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 7));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 9));
    mtree_destroy(&mt);
}

static void test_adjacent_ranges(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    mtree_store_range(&mt, 10, 19, VAL(2));
    mtree_store_range(&mt, 20, 29, VAL(3));
    for (uint64_t i = 0; i <= 9; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    for (uint64_t i = 10; i <= 19; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    for (uint64_t i = 20; i <= 29; i++)
        assert_ptr_equal(VAL(3), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* -- Cursor API -------------------------------------------------------- */

static void test_mas_walk(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 10, 19, VAL(42));
    MA_STATE(mas, &mt, 15, 15);
    void *entry = mas_walk(&mas);
    assert_ptr_equal(VAL(42), entry);
    assert_true(mas.min >= 10 && mas.min <= 15);
    assert_true(mas.max >= 15 && mas.max <= 19);
    mtree_destroy(&mt);
}

static void test_mas_find_iteration(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 20; i += 2)
        mtree_store(&mt, i, VAL(i + 1));

    MA_STATE(mas, &mt, 0, 0);
    int count = 0;
    void *entry;
    mas_for_each(&mas, entry, MAPLE_MAX) {
        assert_non_null(entry);
        count++;
    }
    assert_int_equal(10, count);
    mtree_destroy(&mt);
}

static void test_mt_for_each(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    mtree_store_range(&mt, 10, 19, VAL(2));
    mtree_store_range(&mt, 20, 29, VAL(3));

    int count = 0;
    uint64_t index = 0;
    void *entry;
    mt_for_each(&mt, entry, index, MAPLE_MAX) {
        assert_non_null(entry);
        count++;
    }
    assert_int_equal(3, count);
    mtree_destroy(&mt);
}

/* -- Gap search -------------------------------------------------------- */

static void test_gap_search_empty(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 1000, 10);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 0 && mas.index <= 991);
    assert_int_equal(mas.last - mas.index + 1, 10);
    mtree_destroy(&mt);
}

static void test_gap_search_with_entries(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 200, 299, VAL(2));

    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 500, 50);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 100 && mas.index + 49 <= 199);
    mtree_destroy(&mt);
}

static void test_gap_search_rev(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 200, 299, VAL(2));

    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area_rev(&mas, 0, 500, 50);
    assert_int_equal(0, ret);
    assert_true(mas.index + 49 <= 500);
    assert_true(mas.index >= 100);
    mtree_destroy(&mt);
}

static void test_gap_search_no_space(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 999, VAL(1));

    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 999, 1);
    assert_int_not_equal(0, ret);
    mtree_destroy(&mt);
}

/* -- RCU helpers ------------------------------------------------------- */

static void test_mt_find(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 5, VAL(50));
    mtree_store(&mt, 15, VAL(150));
    mtree_store(&mt, 25, VAL(250));

    uint64_t index = 0;
    void *entry = mt_find(&mt, &index, MAPLE_MAX);
    assert_ptr_equal(VAL(50), entry);

    entry = mt_find(&mt, &index, MAPLE_MAX);
    assert_ptr_equal(VAL(150), entry);

    entry = mt_find(&mt, &index, MAPLE_MAX);
    assert_ptr_equal(VAL(250), entry);

    entry = mt_find(&mt, &index, MAPLE_MAX);
    assert_null(entry);
    mtree_destroy(&mt);
}

static void test_mt_next_prev(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(10));
    mtree_store(&mt, 20, VAL(20));
    mtree_store(&mt, 30, VAL(30));

    void *entry = mt_next(&mt, 10, 50);
    assert_ptr_equal(VAL(20), entry);

    entry = mt_prev(&mt, 30, 0);
    assert_ptr_equal(VAL(20), entry);
    mtree_destroy(&mt);
}

/* -- Destroy ----------------------------------------------------------- */

static void test_destroy(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 200; i++)
        mtree_store(&mt, i, VAL(i + 1));
    mtree_destroy(&mt);
    assert_true(mt_empty(&mt));
}

/* -- Scale / stress ---------------------------------------------------- */

static void test_scale_1000(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 1000; i++)
        assert_int_equal(0, mtree_store(&mt, i * 10, VAL(i + 1)));
    for (uint64_t i = 0; i < 1000; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 10));
    for (uint64_t i = 0; i < 1000; i++)
        assert_null(mtree_load(&mt, i * 10 + 1));
    mtree_destroy(&mt);
}

static void test_interleaved_store_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 100; i++)
        mtree_store(&mt, i, VAL(i + 1));
    for (uint64_t i = 0; i < 100; i += 2)
        mtree_erase(&mt, i);
    for (uint64_t i = 0; i < 100; i++) {
        if (i % 2 == 0)
            assert_null(mtree_load(&mt, i));
        else
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    }
    mtree_destroy(&mt);
}

static void test_range_iteration(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* 5 disjoint ranges */
    mtree_store_range(&mt, 100, 199, VAL(1));
    mtree_store_range(&mt, 300, 399, VAL(2));
    mtree_store_range(&mt, 500, 599, VAL(3));
    mtree_store_range(&mt, 700, 799, VAL(4));
    mtree_store_range(&mt, 900, 999, VAL(5));

    int count = 0;
    uint64_t index = 0;
    void *entry;
    mt_for_each(&mt, entry, index, 1000) {
        assert_non_null(entry);
        count++;
    }
    assert_int_equal(5, count);
    mtree_destroy(&mt);
}

static void test_mas_store(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    MA_STATE(mas, &mt, 10, 19);
    assert_int_equal(0, mas_store(&mas, VAL(42)));
    for (uint64_t i = 10; i <= 19; i++)
        assert_ptr_equal(VAL(42), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

static void test_mas_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 5, VAL(55));
    MA_STATE(mas, &mt, 5, 5);
    void *old = mas_erase(&mas);
    assert_ptr_equal(VAL(55), old);
    assert_null(mtree_load(&mt, 5));
    mtree_destroy(&mt);
}

static void test_large_range(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store_range(&mt, 0, 999999, VAL(1)));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 0));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 500000));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 999999));
    assert_null(mtree_load(&mt, 1000000));
    mtree_destroy(&mt);
}

static void test_coalesce(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Store individual entries and then overwrite all at once. */
    for (uint64_t i = 0; i < 10; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Overwrite entire range with single value — should coalesce. */
    mtree_store_range(&mt, 0, 9, VAL(99));
    for (uint64_t i = 0; i <= 9; i++)
        assert_ptr_equal(VAL(99), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

static void test_sparse_keys(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    uint64_t keys[] = { 100, 10000, 1000000, 100000000UL, 10000000000UL };
    for (int i = 0; i < 5; i++)
        assert_int_equal(0, mtree_store(&mt, keys[i], VAL(i + 1)));
    for (int i = 0; i < 5; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, keys[i]));
    assert_null(mtree_load(&mt, 50));
    mtree_destroy(&mt);
}

static void test_store_max_key(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store(&mt, MAPLE_MAX, VAL(1)));
    assert_ptr_equal(VAL(1), mtree_load(&mt, MAPLE_MAX));
    assert_null(mtree_load(&mt, MAPLE_MAX - 1));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Node splitting & multi-level tree                                  */
/* ================================================================== */

/* Force >16 single-slot entries so we get a split and multi-level tree. */
static void test_split_single_node(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* 17 entries each at their own index forces at least one split. */
    for (uint64_t i = 0; i < 17; i++)
        assert_int_equal(0, mtree_store(&mt, i * 100, VAL(i + 1)));
    for (uint64_t i = 0; i < 17; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 100));
    mtree_destroy(&mt);
}

/* Trigger several levels of splitting to exercise recursive split. */
static void test_deep_tree(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 500; i++)
        assert_int_equal(0, mtree_store(&mt, i, VAL(i + 1)));
    /* Verify every entry survives multiple splits. */
    for (uint64_t i = 0; i < 500; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    /* Boundary: nothing beyond range. */
    assert_null(mtree_load(&mt, 500));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Range overlap edge cases                                           */
/* ================================================================== */

/* Overwrite with a superset range. */
static void test_range_superset_overwrite(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 10, 19, VAL(1));
    mtree_store_range(&mt, 5, 25, VAL(2));
    for (uint64_t i = 5; i <= 25; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 4));
    assert_null(mtree_load(&mt, 26));
    mtree_destroy(&mt);
}

/* Overwrite subset in the middle of an existing range. */
static void test_range_subset_overwrite(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 30, 59, VAL(2));
    for (uint64_t i = 0; i <= 29; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    for (uint64_t i = 30; i <= 59; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    for (uint64_t i = 60; i <= 99; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* Overlap from the left edge. */
static void test_range_overlap_left(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 50, 99, VAL(1));
    mtree_store_range(&mt, 30, 60, VAL(2));
    for (uint64_t i = 30; i <= 60; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    for (uint64_t i = 61; i <= 99; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 29));
    mtree_destroy(&mt);
}

/* Overlap from the right edge. */
static void test_range_overlap_right(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 49, VAL(1));
    mtree_store_range(&mt, 40, 60, VAL(2));
    for (uint64_t i = 0; i <= 39; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    for (uint64_t i = 40; i <= 60; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 61));
    mtree_destroy(&mt);
}

/* Store same value in adjacent ranges — should coalesce into one. */
static void test_adjacent_same_value_coalesce(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    mtree_store_range(&mt, 10, 19, VAL(1));
    /* Both ranges have the same value — the tree should coalesce. */
    for (uint64_t i = 0; i <= 19; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 20));
    mtree_destroy(&mt);
}

/* Layer three overlapping ranges. */
static void test_triple_overlap(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 25, 74, VAL(2));
    mtree_store_range(&mt, 40, 59, VAL(3));
    for (uint64_t i = 0; i <= 24; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    for (uint64_t i = 25; i <= 39; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    for (uint64_t i = 40; i <= 59; i++)
        assert_ptr_equal(VAL(3), mtree_load(&mt, i));
    for (uint64_t i = 60; i <= 74; i++)
        assert_ptr_equal(VAL(2), mtree_load(&mt, i));
    for (uint64_t i = 75; i <= 99; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Boundary value tests                                               */
/* ================================================================== */

/* Store at index 0 and 1. */
static void test_boundary_low(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 0, VAL(10));
    mtree_store(&mt, 1, VAL(20));
    assert_ptr_equal(VAL(10), mtree_load(&mt, 0));
    assert_ptr_equal(VAL(20), mtree_load(&mt, 1));
    assert_null(mtree_load(&mt, 2));
    mtree_destroy(&mt);
}

/* Store at MAPLE_MAX and MAPLE_MAX-1. */
static void test_boundary_high(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, MAPLE_MAX, VAL(10));
    mtree_store(&mt, MAPLE_MAX - 1, VAL(20));
    assert_ptr_equal(VAL(10), mtree_load(&mt, MAPLE_MAX));
    assert_ptr_equal(VAL(20), mtree_load(&mt, MAPLE_MAX - 1));
    assert_null(mtree_load(&mt, MAPLE_MAX - 2));
    mtree_destroy(&mt);
}

/* Range that spans index 0. */
static void test_range_from_zero(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store_range(&mt, 0, 4, VAL(1)));
    for (uint64_t i = 0; i <= 4; i++)
        assert_ptr_equal(VAL(1), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 5));
    mtree_destroy(&mt);
}

/* Invalid range (first > last). */
static void test_store_range_invalid(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    int ret = mtree_store_range(&mt, 10, 5, VAL(1));
    assert_int_not_equal(0, ret);
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Erase edge cases                                                   */
/* ================================================================== */

/* Erase from empty tree. */
static void test_erase_empty_tree(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_null(mtree_erase(&mt, 0));
    assert_null(mtree_erase(&mt, MAPLE_MAX));
    mtree_destroy(&mt);
}

/* Erase all entries and verify tree is clean. */
static void test_erase_all(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 50; i++)
        mtree_store(&mt, i, VAL(i + 1));
    for (uint64_t i = 0; i < 50; i++)
        mtree_erase(&mt, i);
    for (uint64_t i = 0; i < 50; i++)
        assert_null(mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* Re-insert after erase. */
static void test_reinsert_after_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(1));
    mtree_erase(&mt, 10);
    assert_null(mtree_load(&mt, 10));
    mtree_store(&mt, 10, VAL(2));
    assert_ptr_equal(VAL(2), mtree_load(&mt, 10));
    mtree_destroy(&mt);
}

/* Erase from a multi-level tree. */
static void test_erase_deep(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 200; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Erase the middle entries. */
    for (uint64_t i = 50; i < 150; i++)
        mtree_erase(&mt, i);
    for (uint64_t i = 0; i < 50; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    for (uint64_t i = 50; i < 150; i++)
        assert_null(mtree_load(&mt, i));
    for (uint64_t i = 150; i < 200; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Cursor traversal                                                   */
/* ================================================================== */

/* mas_walk to a gap returns NULL. */
static void test_mas_walk_gap(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(1));
    mtree_store(&mt, 20, VAL(2));
    MA_STATE(mas, &mt, 15, 15);
    void *entry = mas_walk(&mas);
    assert_null(entry);
    mtree_destroy(&mt);
}

/* mas_next across all entries of a large tree. */
static void test_mas_next_full_scan(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 50; i++)
        mtree_store(&mt, i * 10, VAL(i + 1));

    MA_STATE(mas, &mt, 0, 0);
    void *e = mas_walk(&mas);
    assert_ptr_equal(VAL(1), e);

    int count = 1;
    while ((e = mas_next(&mas, MAPLE_MAX)) != NULL)
        count++;
    assert_int_equal(50, count);
    mtree_destroy(&mt);
}

/* mas_prev across all entries of a large tree. */
static void test_mas_prev_full_scan(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 50; i++)
        mtree_store(&mt, i * 10, VAL(i + 1));

    /* Start at the last entry. */
    MA_STATE(mas, &mt, 490, 490);
    void *e = mas_walk(&mas);
    assert_ptr_equal(VAL(50), e);

    int count = 1;
    while ((e = mas_prev(&mas, 0)) != NULL)
        count++;
    assert_int_equal(50, count);
    mtree_destroy(&mt);
}

/* mas_next respects max boundary. */
static void test_mas_next_bounded(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 10; i++)
        mtree_store(&mt, i * 10, VAL(i + 1));

    MA_STATE(mas, &mt, 0, 0);
    mas_walk(&mas);
    int count = 1;
    while (mas_next(&mas, 50) != NULL)
        count++;
    /* 0, 10, 20, 30, 40, 50 = 6 entries */
    assert_int_equal(6, count);
    mtree_destroy(&mt);
}

/* mas_prev respects min boundary. */
static void test_mas_prev_bounded(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 10; i++)
        mtree_store(&mt, i * 10, VAL(i + 1));

    MA_STATE(mas, &mt, 90, 90);
    mas_walk(&mas);
    int count = 1;
    while (mas_prev(&mas, 50) != NULL)
        count++;
    /* 90, 80, 70, 60, 50 = 5 entries */
    assert_int_equal(5, count);
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  mt_find / mt_next / mt_prev edge cases                             */
/* ================================================================== */

/* mt_find with bounded range stops. */
static void test_mt_find_bounded(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 20; i++)
        mtree_store(&mt, i * 5, VAL(i + 1));

    uint64_t index = 0;
    int count = 0;
    while (mt_find(&mt, &index, 50) != NULL)
        count++;
    /* 0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50 = 11 */
    assert_int_equal(11, count);
    mtree_destroy(&mt);
}

/* mt_find on empty tree returns NULL. */
static void test_mt_find_empty(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    uint64_t index = 0;
    assert_null(mt_find(&mt, &index, MAPLE_MAX));
    mtree_destroy(&mt);
}

/* mt_next at last entry returns NULL. */
static void test_mt_next_at_end(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(1));
    assert_null(mt_next(&mt, 10, 100));
    mtree_destroy(&mt);
}

/* mt_prev at first entry returns NULL. */
static void test_mt_prev_at_start(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(1));
    assert_null(mt_prev(&mt, 10, 0));
    mtree_destroy(&mt);
}

/* mt_next / mt_prev when index equals limit. */
static void test_mt_next_prev_at_limit(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 10, VAL(1));
    mtree_store(&mt, 20, VAL(2));
    /* next with index == max should return NULL. */
    assert_null(mt_next(&mt, 20, 20));
    /* prev with index == min should return NULL. */
    assert_null(mt_prev(&mt, 10, 10));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Gap search edge cases                                              */
/* ================================================================== */

/* Gap exactly fits requested size. */
static void test_gap_exact_fit(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 49, VAL(1));
    mtree_store_range(&mt, 60, 99, VAL(2));
    /* Gap is [50,59] = 10 slots. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 99, 10);
    assert_int_equal(0, ret);
    assert_int_equal(50, (int)mas.index);
    assert_int_equal(59, (int)mas.last);
    mtree_destroy(&mt);
}

/* Gap too small for request. */
static void test_gap_too_small(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 49, VAL(1));
    mtree_store_range(&mt, 60, 99, VAL(2));
    /* Gap is 10, but we need 11. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 99, 11);
    assert_int_not_equal(0, ret);
    mtree_destroy(&mt);
}

/* Multiple gaps — forward picks the first. */
static void test_gap_fwd_first(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    /* Gap: 10..19 */
    mtree_store_range(&mt, 20, 29, VAL(2));
    /* Gap: 30..39 */
    mtree_store_range(&mt, 40, 49, VAL(3));

    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 49, 5);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 10 && mas.index + 4 <= 19);
    mtree_destroy(&mt);
}

/* Multiple gaps — reverse picks the last. */
static void test_gap_rev_last(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 9, VAL(1));
    /* Gap: 10..19 */
    mtree_store_range(&mt, 20, 29, VAL(2));
    /* Gap: 30..39 */
    mtree_store_range(&mt, 40, 49, VAL(3));

    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area_rev(&mas, 0, 49, 5);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 30 && mas.index + 4 <= 39);
    mtree_destroy(&mt);
}

/* Gap at the very start of the range. */
static void test_gap_at_start(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 10, 99, VAL(1));
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 99, 5);
    assert_int_equal(0, ret);
    assert_int_equal(0, (int)mas.index);
    mtree_destroy(&mt);
}

/* Gap at the very end of the range. */
static void test_gap_at_end(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 89, VAL(1));
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 99, 10);
    assert_int_equal(0, ret);
    assert_int_equal(90, (int)mas.index);
    assert_int_equal(99, (int)mas.last);
    mtree_destroy(&mt);
}

/* Gap search with size=1 (minimum). */
static void test_gap_size_one(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 8, VAL(1));
    /* Single gap at index 9. */
    mtree_store_range(&mt, 10, 19, VAL(2));
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 19, 1);
    assert_int_equal(0, ret);
    assert_int_equal(9, (int)mas.index);
    mtree_destroy(&mt);
}

/* Gap search with size=0 returns EBUSY. */
static void test_gap_size_zero(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 100, 0);
    assert_int_not_equal(0, ret);
    mtree_destroy(&mt);
}

/* Gap search on large tree with gaps in the middle. */
static void test_gap_after_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Fill [0..99] as a solid range, then punch a hole. */
    mtree_store_range(&mt, 0, 99, VAL(1));
    /* Erase [40..59] to create a 20-slot gap. */
    mtree_store_range(&mt, 40, 59, NULL);
    /* Verify the gap exists. */
    assert_ptr_equal(VAL(1), mtree_load(&mt, 39));
    assert_null(mtree_load(&mt, 40));
    assert_null(mtree_load(&mt, 59));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 60));
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 99, 15);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 40 && mas.index + 14 <= 59);
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Iteration edge cases                                               */
/* ================================================================== */

/* mt_for_each with a bounded max that clips. */
static void test_mt_for_each_bounded(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 10; i++)
        mtree_store(&mt, i * 10, VAL(i + 1));

    int count = 0;
    uint64_t index = 0;
    void *entry;
    mt_for_each(&mt, entry, index, 55) {
        count++;
    }
    /* 0, 10, 20, 30, 40, 50 = 6 entries <= 55 */
    assert_int_equal(6, count);
    mtree_destroy(&mt);
}

/* mas_for_each starting in the middle. */
static void test_mas_for_each_midstart(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 20; i++)
        mtree_store(&mt, i, VAL(i + 1));

    MA_STATE(mas, &mt, 10, 10);
    int count = 0;
    void *entry;
    mas_for_each(&mas, entry, 19) {
        count++;
    }
    /* 10..19 = 10 entries */
    assert_int_equal(10, count);
    mtree_destroy(&mt);
}

/* Iterate ranges — count unique entries. */
static void test_iterate_ranges(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 200, 299, VAL(2));
    mtree_store_range(&mt, 400, 499, VAL(3));

    uint64_t index = 0;
    void *entry;
    int count = 0;
    mt_for_each(&mt, entry, index, 999) {
        count++;
    }
    assert_int_equal(3, count);
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Store NULL semantics                                               */
/* ================================================================== */

/* Storing NULL works and reads back as NULL. */
static void test_store_null_explicit(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 5, VAL(42));
    assert_ptr_equal(VAL(42), mtree_load(&mt, 5));
    mtree_store(&mt, 5, NULL);
    assert_null(mtree_load(&mt, 5));
    mtree_destroy(&mt);
}

/* Range-store NULL punches a hole. */
static void test_range_punch_hole(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 0, 99, VAL(1));
    /* Punch hole at [20..29]. */
    mtree_store_range(&mt, 20, 29, NULL);
    assert_ptr_equal(VAL(1), mtree_load(&mt, 19));
    assert_null(mtree_load(&mt, 20));
    assert_null(mtree_load(&mt, 29));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 30));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Destroy tests                                                      */
/* ================================================================== */

/* Destroy tree with ranges. */
static void test_destroy_ranges(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (int i = 0; i < 50; i++)
        mtree_store_range(&mt, (uint64_t)i * 100, (uint64_t)i * 100 + 49,
                           VAL(i + 1));
    mtree_destroy(&mt);
    assert_true(mt_empty(&mt));
    assert_null(mtree_load(&mt, 0));
}

/* Double destroy (should be safe). */
static void test_double_destroy(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store(&mt, 1, VAL(1));
    mtree_destroy(&mt);
    mtree_destroy(&mt); /* Destroying empty tree is safe. */
    assert_true(mt_empty(&mt));
}

/* ================================================================== */
/*  Stress / Scale tests                                               */
/* ================================================================== */

/* 5000 entries — exercises deep multi-level tree. */
static void test_scale_5000(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 5000; i++)
        assert_int_equal(0, mtree_store(&mt, i * 3, VAL(i + 1)));
    for (uint64_t i = 0; i < 5000; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 3));
    for (uint64_t i = 0; i < 5000; i++)
        assert_null(mtree_load(&mt, i * 3 + 1));
    mtree_destroy(&mt);
}

/* Insert-erase-reinsert cycle over many entries. */
static void test_churn(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Phase 1: insert 200 entries. */
    for (uint64_t i = 0; i < 200; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Phase 2: erase even entries. */
    for (uint64_t i = 0; i < 200; i += 2)
        mtree_erase(&mt, i);
    /* Phase 3: reinsert at erased locations with new values. */
    for (uint64_t i = 0; i < 200; i += 2)
        mtree_store(&mt, i, VAL(i + 1000));
    /* Verify. */
    for (uint64_t i = 0; i < 200; i++) {
        if (i % 2 == 0)
            assert_ptr_equal(VAL(i + 1000), mtree_load(&mt, i));
        else
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    }
    mtree_destroy(&mt);
}

/* Pseudo-random insert order using a simple LCG. */
static void test_random_order(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    uint64_t keys[500];
    uint64_t seed = 12345;
    for (int i = 0; i < 500; i++) {
        seed = (seed * 6364136223846793005ULL + 1442695040888963407ULL);
        keys[i] = (seed >> 16) % 100000;
    }
    for (int i = 0; i < 500; i++)
        mtree_store(&mt, keys[i], VAL(i + 1));
    /* Last write for each key wins — verify backward. */
    for (int i = 499; i >= 0; i--) {
        void *v = mtree_load(&mt, keys[i]);
        /* We only check non-null because later stores may overwrite. */
        assert_non_null(v);
    }
    mtree_destroy(&mt);
}

/* Many small ranges. */
static void test_many_small_ranges(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 200; i++)
        mtree_store_range(&mt, i * 10, i * 10 + 4, VAL(i + 1));
    for (uint64_t i = 0; i < 200; i++) {
        for (uint64_t j = i * 10; j <= i * 10 + 4; j++)
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, j));
        /* Gap between ranges. */
        assert_null(mtree_load(&mt, i * 10 + 5));
    }
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  VM-style usage patterns                                            */
/* ================================================================== */

/* Simulates VMA insertion + gap lookup (how a VM subsystem uses MT). */
static void test_vm_style_gap_alloc(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Simulate existing VMAs at fixed addresses. */
    uint64_t vma_addrs[][2] = {
        {0x1000, 0x1fff},   /* 4K text */
        {0x2000, 0x3fff},   /* 8K data */
        {0x10000, 0x1ffff}, /* 64K heap */
    };
    for (int i = 0; i < 3; i++)
        mtree_store_range(&mt, vma_addrs[i][0], vma_addrs[i][1], VAL(i + 1));

    /* Now find a 0x1000-byte gap (4K page) after the data segment. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0x4000, 0xfffff, 0x1000);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 0x4000);
    assert_true(mas.last <= 0xfffff);
    assert_int_equal(0x1000, (int)(mas.last - mas.index + 1));
    /* The gap should be in [0x4000, 0xffff] (before heap). */
    assert_true(mas.last < 0x10000);

    /* "Map" the new VMA. */
    mtree_store_range(&mt, mas.index, mas.last, VAL(100));
    assert_ptr_equal(VAL(100), mtree_load(&mt, mas.index));
    mtree_destroy(&mt);
}

/* Simulate mmap/munmap: alloc via gap, then erase. */
static void test_vm_style_mmap_munmap(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Reserve low addresses. */
    mtree_store_range(&mt, 0, 0xfff, VAL(1));

    /* "mmap" 10 pages of 0x1000 each. */
    uint64_t mapped[10];
    for (int i = 0; i < 10; i++) {
        MA_STATE(mas, &mt, 0, 0);
        int ret = mas_empty_area(&mas, 0x1000, 0xfffff, 0x1000);
        assert_int_equal(0, ret);
        mapped[i] = mas.index;
        mtree_store_range(&mt, mas.index, mas.last, VAL(i + 10));
    }
    /* Verify all mapped. */
    for (int i = 0; i < 10; i++)
        assert_ptr_equal(VAL(i + 10), mtree_load(&mt, mapped[i]));

    /* "munmap" every other one. */
    for (int i = 0; i < 10; i += 2)
        mtree_store_range(&mt, mapped[i], mapped[i] + 0xfff, NULL);

    /* Verify unmapped ones are NULL. */
    for (int i = 0; i < 10; i++) {
        if (i % 2 == 0)
            assert_null(mtree_load(&mt, mapped[i]));
        else
            assert_ptr_equal(VAL(i + 10), mtree_load(&mt, mapped[i]));
    }
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Rebalancing tests                                                  */
/* ================================================================== */

/*
 * After erasing enough entries from a leaf, it should merge with its
 * sibling so the tree doesn't degenerate into mostly-empty nodes.
 * We verify indirectly: all lookups still work after heavy erasure.
 */
static void test_rebalance_leaf_merge(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Fill enough entries to create a multi-level tree. */
    for (uint64_t i = 0; i < 100; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Erase most entries from the first leaf to trigger merge. */
    for (uint64_t i = 0; i < 14; i++)
        mtree_erase(&mt, i);
    /* Remaining entries must still be correct. */
    for (uint64_t i = 14; i < 100; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    for (uint64_t i = 0; i < 14; i++)
        assert_null(mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/*
 * Erase all entries one-by-one — tree should shrink and eventually become
 * empty without corrupting any remaining lookups at each step.
 */
static void test_rebalance_erase_all_sequential(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 80; i++)
        mtree_store(&mt, i, VAL(i + 1));
    for (uint64_t i = 0; i < 80; i++) {
        void *old = mtree_erase(&mt, i);
        assert_ptr_equal(VAL(i + 1), old);
        /* Spot-check a surviving entry. */
        if (i + 1 < 80)
            assert_ptr_equal(VAL(i + 2), mtree_load(&mt, i + 1));
    }
    /* Tree should be logically empty (all entries erased). */
    for (uint64_t i = 0; i < 80; i++)
        assert_null(mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/*
 * Erase in reverse order — exercises merging from the right side.
 */
static void test_rebalance_erase_reverse(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 80; i++)
        mtree_store(&mt, i, VAL(i + 1));
    for (int i = 79; i >= 0; i--) {
        void *old = mtree_erase(&mt, (uint64_t)i);
        assert_ptr_equal(VAL(i + 1), old);
        if (i > 0)
            assert_ptr_equal(VAL(i), mtree_load(&mt, (uint64_t)(i - 1)));
    }
    for (uint64_t i = 0; i < 80; i++)
        assert_null(mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/*
 * Tree height shrinking: build a deep tree, then erase enough to trigger
 * root collapse. Insert after shrinking to verify the tree is still usable.
 */
static void test_rebalance_tree_shrink(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Build a tree deep enough (>2 levels). */
    for (uint64_t i = 0; i < 300; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Erase nearly everything — should trigger merges and root shrinking. */
    for (uint64_t i = 0; i < 295; i++)
        mtree_erase(&mt, i);
    /* Remaining 5 entries still correct. */
    for (uint64_t i = 295; i < 300; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    /* Re-insert into the shrunken tree. */
    for (uint64_t i = 0; i < 10; i++)
        assert_int_equal(0, mtree_store(&mt, i, VAL(i + 500)));
    for (uint64_t i = 0; i < 10; i++)
        assert_ptr_equal(VAL(i + 500), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/*
 * Redistribution: when a sibling is too full to merge but the node is
 * underfull, entries should be borrowed so both are balanced.
 * We fill a tree, then selectively erase entries from one leaf.
 */
static void test_rebalance_redistribute(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Build a tree with well-populated leaves. */
    for (uint64_t i = 0; i < 200; i++)
        mtree_store(&mt, i * 2, VAL(i + 1));
    /* Erase entries from the middle to make one leaf underfull,
     * while its sibling should still be too large to merge. */
    for (uint64_t i = 20; i < 28; i++)
        mtree_erase(&mt, i * 2);
    /* All surviving entries still correct. */
    for (uint64_t i = 0; i < 200; i++) {
        if (i >= 20 && i < 28)
            assert_null(mtree_load(&mt, i * 2));
        else
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 2));
    }
    mtree_destroy(&mt);
}

/*
 * Stress: insert many entries, erase every other one (leaving interleaved
 * gaps), then erase the rest. Exercises repeated merge + shrink cycles.
 */
static void test_rebalance_interleaved_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 500; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Phase 1: erase even indices. */
    for (uint64_t i = 0; i < 500; i += 2)
        mtree_erase(&mt, i);
    for (uint64_t i = 0; i < 500; i++) {
        if (i % 2 == 0)
            assert_null(mtree_load(&mt, i));
        else
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    }
    /* Phase 2: erase odd indices. */
    for (uint64_t i = 1; i < 500; i += 2)
        mtree_erase(&mt, i);
    for (uint64_t i = 0; i < 500; i++)
        assert_null(mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/*
 * Re-populate after heavy erasure to verify the rebalanced tree accepts
 * new inserts correctly.
 */
static void test_rebalance_reinsert_after_shrink(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    for (uint64_t i = 0; i < 150; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Erase all. */
    for (uint64_t i = 0; i < 150; i++)
        mtree_erase(&mt, i);
    /* Re-insert a different set. */
    for (uint64_t i = 0; i < 150; i++)
        assert_int_equal(0, mtree_store(&mt, i * 3, VAL(i + 1000)));
    for (uint64_t i = 0; i < 150; i++)
        assert_ptr_equal(VAL(i + 1000), mtree_load(&mt, i * 3));
    mtree_destroy(&mt);
}

/*
 * Gap search after rebalance: ensure the gap tracking stays correct
 * through merges and shrinks.
 */
static void test_rebalance_gap_tracking(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Fill [0, 199] with entries at even indices. */
    for (uint64_t i = 0; i < 100; i++)
        mtree_store(&mt, i * 2, VAL(i + 1));
    /* Erase a contiguous block to create a large gap. */
    for (uint64_t i = 20; i < 40; i++)
        mtree_erase(&mt, i * 2);
    /* The gap of erased entries should be findable. Since entries were at
     * even indices, erasing i*2 for i in [20,39] frees indices
     * 40, 42, ..., 78.  This creates many small 1-slot gaps between surviving
     * odd indices. The gap search should find at least a size-1 gap. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 199, 1);
    assert_int_equal(0, ret);
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Additional edge cases                                              */
/* ================================================================== */

/* #2: Store a range spanning the entire index space [0, MAPLE_MAX]. */
static void test_full_range_store(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    assert_int_equal(0, mtree_store_range(&mt, 0, MAPLE_MAX, VAL(42)));
    assert_ptr_equal(VAL(42), mtree_load(&mt, 0));
    assert_ptr_equal(VAL(42), mtree_load(&mt, 1));
    assert_ptr_equal(VAL(42), mtree_load(&mt, MAPLE_MAX / 2));
    assert_ptr_equal(VAL(42), mtree_load(&mt, MAPLE_MAX - 1));
    assert_ptr_equal(VAL(42), mtree_load(&mt, MAPLE_MAX));
    /* Overwrite a sub-range in the middle. */
    assert_int_equal(0, mtree_store_range(&mt, 100, 199, VAL(99)));
    assert_ptr_equal(VAL(42), mtree_load(&mt, 99));
    assert_ptr_equal(VAL(99), mtree_load(&mt, 100));
    assert_ptr_equal(VAL(99), mtree_load(&mt, 199));
    assert_ptr_equal(VAL(42), mtree_load(&mt, 200));
    mtree_destroy(&mt);
}

/* #3: Range store that spans multiple existing leaves in a deep tree. */
static void test_cross_node_range_store(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Build a multi-level tree with 200 single-index entries. */
    for (uint64_t i = 0; i < 200; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Overwrite a range that should cross 3+ leaf boundaries. */
    assert_int_equal(0, mtree_store_range(&mt, 30, 170, VAL(999)));
    /* Verify the three zones. */
    for (uint64_t i = 0; i < 30; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    for (uint64_t i = 30; i <= 170; i++)
        assert_ptr_equal(VAL(999), mtree_load(&mt, i));
    for (uint64_t i = 171; i < 200; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    assert_null(mtree_load(&mt, 200));
    mtree_destroy(&mt);
}

/* #4: Erase a single index within a range via store_range(NULL).
 * mtree_erase removes the *entire entry* covering an index (the whole
 * range), so punching a single-index hole requires store_range(NULL). */
static void test_erase_within_range(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 10, 29, VAL(5));
    /* Punch a single-index hole at 20. */
    assert_int_equal(0, mtree_store_range(&mt, 20, 20, NULL));
    assert_ptr_equal(VAL(5), mtree_load(&mt, 10));
    assert_ptr_equal(VAL(5), mtree_load(&mt, 19));
    assert_null(mtree_load(&mt, 20));
    assert_ptr_equal(VAL(5), mtree_load(&mt, 21));
    assert_ptr_equal(VAL(5), mtree_load(&mt, 29));
    assert_null(mtree_load(&mt, 9));
    assert_null(mtree_load(&mt, 30));
    /* mtree_erase removes the whole entry that covers the index. */
    struct maple_tree mt2;
    init_tree(&mt2);
    mtree_store_range(&mt2, 10, 29, VAL(5));
    void *old = mtree_erase(&mt2, 20);
    assert_ptr_equal(VAL(5), old);
    /* Entire range [10,29] is gone. */
    for (uint64_t i = 10; i <= 29; i++)
        assert_null(mtree_load(&mt2, i));
    mtree_destroy(&mt);
    mtree_destroy(&mt2);
}

/* #4b: Punch holes at range boundaries via store_range(NULL). */
static void test_erase_range_boundaries(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    mtree_store_range(&mt, 100, 109, VAL(7));
    /* Punch hole at the first index. */
    mtree_store_range(&mt, 100, 100, NULL);
    assert_null(mtree_load(&mt, 100));
    assert_ptr_equal(VAL(7), mtree_load(&mt, 101));
    /* Punch hole at the last index. */
    mtree_store_range(&mt, 109, 109, NULL);
    assert_null(mtree_load(&mt, 109));
    assert_ptr_equal(VAL(7), mtree_load(&mt, 108));
    /* Middle still intact. */
    for (uint64_t i = 101; i <= 108; i++)
        assert_ptr_equal(VAL(7), mtree_load(&mt, i));
    mtree_destroy(&mt);
}

/* #5: Mixed store → gap-search → store-in-gap → erase → gap-search → iterate. */
static void test_mixed_operations_workflow(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Step 1: Store several ranges. */
    mtree_store_range(&mt, 0, 99, VAL(1));
    mtree_store_range(&mt, 200, 299, VAL(2));
    mtree_store_range(&mt, 400, 499, VAL(3));
    /* Step 2: Find a gap and fill it. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 0, 999, 50);
    assert_int_equal(0, ret);
    uint64_t gap_start = mas.index;
    mtree_store_range(&mt, gap_start, gap_start + 49, VAL(10));
    assert_ptr_equal(VAL(10), mtree_load(&mt, gap_start));
    /* Step 3: Erase part of the first range. */
    mtree_store_range(&mt, 40, 59, NULL);
    assert_null(mtree_load(&mt, 50));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 39));
    assert_ptr_equal(VAL(1), mtree_load(&mt, 60));
    /* Step 4: Gap search again — should find the hole we just punched. */
    MA_STATE(mas2, &mt, 0, 0);
    ret = mas_empty_area(&mas2, 0, 99, 10);
    assert_int_equal(0, ret);
    assert_true(mas2.index >= 40 && mas2.last <= 59);
    /* Step 5: Iterate all entries and count. */
    uint64_t index = 0;
    void *entry;
    int count = 0;
    mt_for_each(&mt, entry, index, 999) {
        assert_non_null(entry);
        count++;
    }
    assert_true(count >= 4); /* at least the original 3 ranges + gap fill */
    mtree_destroy(&mt);
}

/* #6: Large-scale gap search after rebalancing from heavy erasure. */
static void test_gap_search_after_heavy_rebalance(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Fill [0, 999] as a solid range. */
    mtree_store_range(&mt, 0, 999, VAL(1));
    /* Insert individual entries beyond to create a deep tree. */
    for (uint64_t i = 1000; i < 1500; i++)
        mtree_store(&mt, i, VAL(i + 1));
    /* Erase a big contiguous block to trigger multiple merges. */
    for (uint64_t i = 1000; i < 1400; i++)
        mtree_erase(&mt, i);
    /* Verify surviving entries. */
    assert_ptr_equal(VAL(1), mtree_load(&mt, 500));
    for (uint64_t i = 1400; i < 1500; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));
    /* Gap search should find the erased region. */
    MA_STATE(mas, &mt, 0, 0);
    int ret = mas_empty_area(&mas, 1000, 1999, 100);
    assert_int_equal(0, ret);
    assert_true(mas.index >= 1000 && mas.last < 1400);
    mtree_destroy(&mt);
}

/* #8: Store entry value 0x2 which matches MAPLE_ROOT_NODE tag. */
static void test_tagged_pointer_value(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* 0x2 == MAPLE_ROOT_NODE — must not cause confusion. */
    void *tricky = (void *)(uintptr_t)0x2;
    assert_int_equal(0, mtree_store(&mt, 10, tricky));
    assert_ptr_equal(tricky, mtree_load(&mt, 10));
    /* Store more entries to push tree into multi-node form. */
    for (uint64_t i = 0; i < 50; i++)
        mtree_store(&mt, i * 3, VAL(i + 100));
    /* The tricky value at index 10 should survive splits. */
    /* Note: index 10 is not a multiple of 3 that we overwrote (9,12 are neighbors). */
    /* Actually 10 is not i*3 for any integer, so it's safe. But let's verify: */
    assert_ptr_equal(tricky, mtree_load(&mt, 10));
    /* Also test 0x1 and 0x3 near the tag bits. */
    mtree_store(&mt, 5000, (void *)(uintptr_t)0x1);
    mtree_store(&mt, 5001, (void *)(uintptr_t)0x3);
    assert_ptr_equal((void *)(uintptr_t)0x1, mtree_load(&mt, 5000));
    assert_ptr_equal((void *)(uintptr_t)0x3, mtree_load(&mt, 5001));
    mtree_destroy(&mt);
}

/* #10: Sparse keys followed by dense fill between them. */
static void test_sparse_then_dense(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);
    /* Phase 1: Very sparse keys with huge gaps. */
    uint64_t sparse[] = { 0, 1000000, 2000000000UL, 3000000000000UL };
    for (int i = 0; i < 4; i++)
        assert_int_equal(0, mtree_store(&mt, sparse[i], VAL(i + 1)));
    for (int i = 0; i < 4; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, sparse[i]));
    /* Phase 2: Dense fill around the first sparse key. */
    for (uint64_t i = 1; i <= 200; i++)
        assert_int_equal(0, mtree_store(&mt, i, VAL(i + 100)));
    /* Verify sparse keys still correct. */
    assert_ptr_equal(VAL(1), mtree_load(&mt, 0));
    assert_ptr_equal(VAL(2), mtree_load(&mt, 1000000));
    assert_ptr_equal(VAL(3), mtree_load(&mt, 2000000000UL));
    assert_ptr_equal(VAL(4), mtree_load(&mt, 3000000000000UL));
    /* Verify dense fill. */
    for (uint64_t i = 1; i <= 200; i++)
        assert_ptr_equal(VAL(i + 100), mtree_load(&mt, i));
    /* No false positives in gaps. */
    assert_null(mtree_load(&mt, 500));
    assert_null(mtree_load(&mt, 999999));
    mtree_destroy(&mt);
}

/* ================================================================== */
/*  Concurrency stress tests                                           */
/* ================================================================== */

#define CONC_NUM_THREADS   8
#define CONC_OPS_PER_THREAD 2000
#define CONC_KEY_SPACE     5000

struct thread_ctx {
    struct maple_tree *mt;
    int thread_id;
    int errors;
};

/*
 * Worker: each thread stores, loads, and erases keys in a shared tree
 * using the locked API wrappers.  Keys are partitioned so that each
 * thread "owns" a range, but loads are done across the full key space
 * to exercise concurrent reads.
 */
static void *conc_writer_reader(void *arg)
{
    struct thread_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    int id = ctx->thread_id;
    ctx->errors = 0;

    /* Each thread owns keys [base, base + CONC_OPS_PER_THREAD). */
    uint64_t base = (uint64_t)id * CONC_OPS_PER_THREAD;

    /* Phase 1: Insert owned keys. */
    for (uint64_t i = 0; i < CONC_OPS_PER_THREAD; i++) {
        uint64_t key = base + i;
        int ret = mtree_lock_store(mt, key, VAL(key + 1));
        if (ret != 0)
            ctx->errors++;
    }

    /* Phase 2: Verify own keys and read-probe other threads' keys. */
    for (uint64_t i = 0; i < CONC_OPS_PER_THREAD; i++) {
        uint64_t key = base + i;
        void *v = mtree_lock_load(mt, key);
        if (v != VAL(key + 1))
            ctx->errors++;
    }
    /* Read a few keys from another thread's range (may or may not exist yet). */
    uint64_t other_base = ((uint64_t)((id + 1) % CONC_NUM_THREADS))
                          * CONC_OPS_PER_THREAD;
    for (uint64_t i = 0; i < 100; i++)
        (void)mtree_lock_load(mt, other_base + i);

    /* Phase 3: Erase half of own keys. */
    for (uint64_t i = 0; i < CONC_OPS_PER_THREAD; i += 2) {
        uint64_t key = base + i;
        mtree_lock_erase(mt, key);
    }

    /* Phase 4: Verify erased keys are NULL, others still present. */
    for (uint64_t i = 0; i < CONC_OPS_PER_THREAD; i++) {
        uint64_t key = base + i;
        void *v = mtree_lock_load(mt, key);
        if (i % 2 == 0) {
            if (v != NULL)
                ctx->errors++;
        } else {
            if (v != VAL(key + 1))
                ctx->errors++;
        }
    }

    return NULL;
}

/*
 * Parallel writers: all threads insert into partitioned key ranges,
 * then we verify every key globally.
 */
static void test_concurrent_writers(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    pthread_t threads[CONC_NUM_THREADS];
    struct thread_ctx ctxs[CONC_NUM_THREADS];

    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        ctxs[i].mt = &mt;
        ctxs[i].thread_id = i;
        ctxs[i].errors = 0;
        pthread_create(&threads[i], NULL, conc_writer_reader, &ctxs[i]);
    }

    for (int i = 0; i < CONC_NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    int total_errors = 0;
    for (int i = 0; i < CONC_NUM_THREADS; i++)
        total_errors += ctxs[i].errors;
    assert_int_equal(0, total_errors);

    mtree_destroy(&mt);
}

/*
 * Contended key space: all threads read and write to the SAME keys,
 * exercising lock contention on every operation.
 */
static void *conc_contended_worker(void *arg)
{
    struct thread_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    int id = ctx->thread_id;
    ctx->errors = 0;

    /* Simple LCG seeded per-thread for varied access patterns. */
    uint64_t seed = (uint64_t)(id + 1) * 6364136223846793005ULL + 1;

    for (int op = 0; op < CONC_OPS_PER_THREAD; op++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t key = (seed >> 16) % CONC_KEY_SPACE;
        int action = (seed >> 48) % 3;

        switch (action) {
        case 0: /* store */
            mtree_lock_store(mt, key, VAL(key + 1));
            break;
        case 1: /* load — value is either correct or NULL (erased by another) */
            {
                void *v = mtree_lock_load(mt, key);
                if (v != NULL && v != VAL(key + 1))
                    ctx->errors++;
            }
            break;
        case 2: /* erase */
            mtree_lock_erase(mt, key);
            break;
        }
    }
    return NULL;
}

static void test_concurrent_contended(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    pthread_t threads[CONC_NUM_THREADS];
    struct thread_ctx ctxs[CONC_NUM_THREADS];

    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        ctxs[i].mt = &mt;
        ctxs[i].thread_id = i;
        ctxs[i].errors = 0;
        pthread_create(&threads[i], NULL, conc_contended_worker, &ctxs[i]);
    }

    for (int i = 0; i < CONC_NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    int total_errors = 0;
    for (int i = 0; i < CONC_NUM_THREADS; i++)
        total_errors += ctxs[i].errors;
    assert_int_equal(0, total_errors);

    mtree_destroy(&mt);
}

/*
 * Range-store vs point-erase contention: some threads store ranges
 * while others erase individual keys within those ranges.
 */
static void *conc_range_store_worker(void *arg)
{
    struct thread_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    int id = ctx->thread_id;
    ctx->errors = 0;

    uint64_t base = (uint64_t)id * 500;
    for (int round = 0; round < 20; round++) {
        uint64_t first = base + (uint64_t)round * 20;
        uint64_t last  = first + 9;
        mtree_lock_store_range(mt, first, last, VAL(first + 1));
        /* Immediately verify a sample within our own range. */
        void *v = mtree_lock_load(mt, first + 5);
        /* Another thread may have erased it already, so accept NULL. */
        if (v != NULL && v != VAL(first + 1))
            ctx->errors++;
    }
    return NULL;
}

static void *conc_point_erase_worker(void *arg)
{
    struct thread_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    ctx->errors = 0;

    uint64_t seed = (uint64_t)(ctx->thread_id + 100) * 2654435761ULL;
    for (int op = 0; op < 200; op++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t key = (seed >> 16) % (CONC_NUM_THREADS * 500);
        mtree_lock_erase(mt, key);
    }
    return NULL;
}

static void test_concurrent_range_vs_erase(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    pthread_t threads[CONC_NUM_THREADS * 2];
    struct thread_ctx ctxs[CONC_NUM_THREADS * 2];

    /* First half: range store workers. */
    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        ctxs[i].mt = &mt;
        ctxs[i].thread_id = i;
        ctxs[i].errors = 0;
        pthread_create(&threads[i], NULL, conc_range_store_worker, &ctxs[i]);
    }
    /* Second half: point erase workers. */
    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        int idx = CONC_NUM_THREADS + i;
        ctxs[idx].mt = &mt;
        ctxs[idx].thread_id = i;
        ctxs[idx].errors = 0;
        pthread_create(&threads[idx], NULL, conc_point_erase_worker, &ctxs[idx]);
    }

    for (int i = 0; i < CONC_NUM_THREADS * 2; i++)
        pthread_join(threads[i], NULL);

    int total_errors = 0;
    for (int i = 0; i < CONC_NUM_THREADS * 2; i++)
        total_errors += ctxs[i].errors;
    assert_int_equal(0, total_errors);

    mtree_destroy(&mt);
}

/*
 * Sequential consistency check: N threads each insert a unique key,
 * then a barrier, then all threads verify all N keys are present.
 */
static pthread_barrier_t g_barrier;

static void *conc_barrier_worker(void *arg)
{
    struct thread_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    int id = ctx->thread_id;
    ctx->errors = 0;

    /* Each thread stores its unique key. */
    mtree_lock_store(mt, (uint64_t)id * 1000, VAL(id + 1));

    /* Wait for all threads to finish storing. */
    pthread_barrier_wait(&g_barrier);

    /* All keys from all threads should now be visible. */
    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        void *v = mtree_lock_load(mt, (uint64_t)i * 1000);
        if (v != VAL(i + 1))
            ctx->errors++;
    }
    return NULL;
}

static void test_concurrent_barrier_visibility(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    pthread_barrier_init(&g_barrier, NULL, CONC_NUM_THREADS);

    pthread_t threads[CONC_NUM_THREADS];
    struct thread_ctx ctxs[CONC_NUM_THREADS];

    for (int i = 0; i < CONC_NUM_THREADS; i++) {
        ctxs[i].mt = &mt;
        ctxs[i].thread_id = i;
        ctxs[i].errors = 0;
        pthread_create(&threads[i], NULL, conc_barrier_worker, &ctxs[i]);
    }

    for (int i = 0; i < CONC_NUM_THREADS; i++)
        pthread_join(threads[i], NULL);

    pthread_barrier_destroy(&g_barrier);

    int total_errors = 0;
    for (int i = 0; i < CONC_NUM_THREADS; i++)
        total_errors += ctxs[i].errors;
    assert_int_equal(0, total_errors);

    mtree_destroy(&mt);
}

/* -- Main -------------------------------------------------------------- */

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_empty_tree),
        cmocka_unit_test(test_single_store_load),
        cmocka_unit_test(test_store_at_zero),
        cmocka_unit_test(test_store_range),
        cmocka_unit_test(test_overwrite),
        cmocka_unit_test(test_erase),
        cmocka_unit_test(test_erase_nonexistent),
        cmocka_unit_test(test_sequential_insert),
        cmocka_unit_test(test_reverse_insert),
        cmocka_unit_test(test_overlapping_range_store),
        cmocka_unit_test(test_range_erase_via_store_null),
        cmocka_unit_test(test_adjacent_ranges),
        cmocka_unit_test(test_mas_walk),
        cmocka_unit_test(test_mas_find_iteration),
        cmocka_unit_test(test_mt_for_each),
        cmocka_unit_test(test_gap_search_empty),
        cmocka_unit_test(test_gap_search_with_entries),
        cmocka_unit_test(test_gap_search_rev),
        cmocka_unit_test(test_gap_search_no_space),
        cmocka_unit_test(test_mt_find),
        cmocka_unit_test(test_mt_next_prev),
        cmocka_unit_test(test_destroy),
        cmocka_unit_test(test_scale_1000),
        cmocka_unit_test(test_interleaved_store_erase),
        cmocka_unit_test(test_range_iteration),
        cmocka_unit_test(test_mas_store),
        cmocka_unit_test(test_mas_erase),
        cmocka_unit_test(test_large_range),
        cmocka_unit_test(test_coalesce),
        cmocka_unit_test(test_sparse_keys),
        cmocka_unit_test(test_store_max_key),
        /* Node splitting & multi-level tree */
        cmocka_unit_test(test_split_single_node),
        cmocka_unit_test(test_deep_tree),
        /* Range overlap edge cases */
        cmocka_unit_test(test_range_superset_overwrite),
        cmocka_unit_test(test_range_subset_overwrite),
        cmocka_unit_test(test_range_overlap_left),
        cmocka_unit_test(test_range_overlap_right),
        cmocka_unit_test(test_adjacent_same_value_coalesce),
        cmocka_unit_test(test_triple_overlap),
        /* Boundary values */
        cmocka_unit_test(test_boundary_low),
        cmocka_unit_test(test_boundary_high),
        cmocka_unit_test(test_range_from_zero),
        cmocka_unit_test(test_store_range_invalid),
        /* Erase edge cases */
        cmocka_unit_test(test_erase_empty_tree),
        cmocka_unit_test(test_erase_all),
        cmocka_unit_test(test_reinsert_after_erase),
        cmocka_unit_test(test_erase_deep),
        /* Cursor traversal */
        cmocka_unit_test(test_mas_walk_gap),
        cmocka_unit_test(test_mas_next_full_scan),
        cmocka_unit_test(test_mas_prev_full_scan),
        cmocka_unit_test(test_mas_next_bounded),
        cmocka_unit_test(test_mas_prev_bounded),
        /* mt_find / mt_next / mt_prev edge cases */
        cmocka_unit_test(test_mt_find_bounded),
        cmocka_unit_test(test_mt_find_empty),
        cmocka_unit_test(test_mt_next_at_end),
        cmocka_unit_test(test_mt_prev_at_start),
        cmocka_unit_test(test_mt_next_prev_at_limit),
        /* Gap search edge cases */
        cmocka_unit_test(test_gap_exact_fit),
        cmocka_unit_test(test_gap_too_small),
        cmocka_unit_test(test_gap_fwd_first),
        cmocka_unit_test(test_gap_rev_last),
        cmocka_unit_test(test_gap_at_start),
        cmocka_unit_test(test_gap_at_end),
        cmocka_unit_test(test_gap_size_one),
        cmocka_unit_test(test_gap_size_zero),
        cmocka_unit_test(test_gap_after_erase),
        /* Iteration edge cases */
        cmocka_unit_test(test_mt_for_each_bounded),
        cmocka_unit_test(test_mas_for_each_midstart),
        cmocka_unit_test(test_iterate_ranges),
        /* Store NULL semantics */
        cmocka_unit_test(test_store_null_explicit),
        cmocka_unit_test(test_range_punch_hole),
        /* Destroy tests */
        cmocka_unit_test(test_destroy_ranges),
        cmocka_unit_test(test_double_destroy),
        /* Stress / scale */
        cmocka_unit_test(test_scale_5000),
        cmocka_unit_test(test_churn),
        cmocka_unit_test(test_random_order),
        cmocka_unit_test(test_many_small_ranges),
        /* VM-style usage */
        cmocka_unit_test(test_vm_style_gap_alloc),
        cmocka_unit_test(test_vm_style_mmap_munmap),
        /* Rebalancing */
        cmocka_unit_test(test_rebalance_leaf_merge),
        cmocka_unit_test(test_rebalance_erase_all_sequential),
        cmocka_unit_test(test_rebalance_erase_reverse),
        cmocka_unit_test(test_rebalance_tree_shrink),
        cmocka_unit_test(test_rebalance_redistribute),
        cmocka_unit_test(test_rebalance_interleaved_erase),
        cmocka_unit_test(test_rebalance_reinsert_after_shrink),
        cmocka_unit_test(test_rebalance_gap_tracking),
        /* Additional edge cases */
        cmocka_unit_test(test_full_range_store),
        cmocka_unit_test(test_cross_node_range_store),
        cmocka_unit_test(test_erase_within_range),
        cmocka_unit_test(test_erase_range_boundaries),
        cmocka_unit_test(test_mixed_operations_workflow),
        cmocka_unit_test(test_gap_search_after_heavy_rebalance),
        cmocka_unit_test(test_tagged_pointer_value),
        cmocka_unit_test(test_sparse_then_dense),
        /* Concurrency stress tests */
        cmocka_unit_test(test_concurrent_writers),
        cmocka_unit_test(test_concurrent_contended),
        cmocka_unit_test(test_concurrent_range_vs_erase),
        cmocka_unit_test(test_concurrent_barrier_visibility),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
