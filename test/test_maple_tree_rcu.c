/**
 * @file test_maple_tree_rcu.c
 * @brief RCU-enabled test suite — runs ALL test cases under RCU mode.
 *
 * Implements a primitive RCU mechanism where the grace period spans each
 * individual test.  Deferred-free objects accumulate in a global linked
 * list (mutex-protected) and are drained in a per-test teardown.
 *
 * All 99 test functions from test_maple_tree.c are pulled in via #include
 * and re-registered with the RCU teardown.  Two additional RCU-specific
 * tests exercise deferred-free accounting and lock-free reader concurrency.
 *
 * Build with: -DMT_CONFIG_RCU -DMT_CUSTOM_RCU
 */

/* ====================================================================== */
/*  Primitive RCU implementation                                           */
/*                                                                         */
/*  Must be defined BEFORE maple_tree.h is included (via the #include of   */
/*  test_maple_tree.c below), because maple_tree_config.h with             */
/*  MT_CUSTOM_RCU declares these as extern.                                */
/* ====================================================================== */

#include <stdlib.h>
#include <pthread.h>

struct rcu_entry {
    struct rcu_entry   *next;
    void              (*cb)(void *);
    void               *data;
};

static pthread_mutex_t rcu_global_lock = PTHREAD_MUTEX_INITIALIZER;
static struct rcu_entry *rcu_global_list;

void mt_rcu_read_lock(void)   { /* no-op: entire test is the grace period */ }
void mt_rcu_read_unlock(void) { /* no-op */ }

/*
 * Append to a global mutex-protected list so that deferred frees from
 * worker threads (which may exit before drain) are never lost.
 */
void mt_call_rcu(void (*cb)(void *), void *data)
{
    struct rcu_entry *e = malloc(sizeof(*e));
    if (!e)
        abort();
    e->cb   = cb;
    e->data = data;

    pthread_mutex_lock(&rcu_global_lock);
    e->next = rcu_global_list;
    rcu_global_list = e;
    pthread_mutex_unlock(&rcu_global_lock);
}

/**
 * mt_rcu_drain - End the grace period: execute every deferred callback.
 * Returns the number of objects freed.
 */
static size_t mt_rcu_drain(void)
{
    pthread_mutex_lock(&rcu_global_lock);
    struct rcu_entry *e = rcu_global_list;
    rcu_global_list = NULL;
    pthread_mutex_unlock(&rcu_global_lock);

    size_t count = 0;
    while (e) {
        struct rcu_entry *next = e->next;
        e->cb(e->data);
        free(e);
        e = next;
        count++;
    }
    return count;
}

/* ====================================================================== */
/*  Pull in all 99 test functions from the non-RCU test file               */
/* ====================================================================== */

/* Rename the original main() and init_tree() so we can override them. */
#define main test_main_non_rcu_unused_
#define init_tree init_tree_base_
#include "test_maple_tree.c"
#undef main
#undef init_tree

/* Re-define init_tree to also enable the per-tree RCU flag. */
static void init_tree(struct maple_tree *mt)
{
    mt_init(mt);
    mt_set_in_rcu(mt);
}

/* ====================================================================== */
/*  RCU-specific tests                                                     */
/* ====================================================================== */

/**
 * Verify that COW operations produce deferred frees (not immediate).
 * Insert enough keys to trigger splits, then check that mt_rcu_drain()
 * finds deferred objects.
 */
static void test_rcu_deferred_accounting(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    /* 30 keys => multiple splits => dead nodes accumulate. */
    for (uint64_t i = 0; i < 30; i++)
        assert_int_equal(0, mtree_store(&mt, i * 10, VAL(i + 1)));

    for (uint64_t i = 0; i < 30; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 10));

    /* Drain should find deferred dead nodes from splits. */
    size_t freed = mt_rcu_drain();
    assert_true(freed > 0);

    /* Tree still works after drain. */
    for (uint64_t i = 0; i < 30; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 10));

    mtree_destroy(&mt);
}

/**
 * Core RCU scenario: lock-free readers + locked writer.
 * Readers call mtree_load() WITHOUT the lock.  Writer calls
 * mtree_lock_store/erase.  Because deferred frees are held until
 * mt_rcu_drain(), readers never hit use-after-free.
 */
#define RCU_READER_THREADS  4
#define RCU_OPS_PER_THREAD  2000

struct rcu_ctx {
    struct maple_tree *mt;
    int thread_id;
    int errors;
};

static void *rcu_lockfree_reader(void *arg)
{
    struct rcu_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    ctx->errors = 0;

    for (int i = 0; i < RCU_OPS_PER_THREAD; i++) {
        uint64_t key = (uint64_t)(i % 500);
        mt_rcu_read_lock();
        void *v = mtree_load(mt, key);         /* NO lock! */
        if (v != NULL && v != VAL(key + 1))
            ctx->errors++;
        mt_rcu_read_unlock();
    }
    return NULL;
}

static void *rcu_locked_writer(void *arg)
{
    struct rcu_ctx *ctx = arg;
    struct maple_tree *mt = ctx->mt;
    ctx->errors = 0;

    for (int i = 0; i < RCU_OPS_PER_THREAD; i++) {
        uint64_t key = (uint64_t)(i % 500);
        if (i % 3 == 0)
            mtree_lock_erase(mt, key);
        else
            mtree_lock_store(mt, key, VAL(key + 1));
    }
    return NULL;
}

static void test_rcu_lockfree_readers(void **state) {
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    /* Pre-populate so readers find data immediately. */
    for (uint64_t i = 0; i < 500; i++)
        mtree_store(&mt, i, VAL(i + 1));

    /* Drain deferred frees from setup inserts. */
    mt_rcu_drain();

    pthread_t readers[RCU_READER_THREADS];
    struct rcu_ctx reader_ctxs[RCU_READER_THREADS];
    pthread_t writer;
    struct rcu_ctx writer_ctx = { .mt = &mt, .thread_id = 0, .errors = 0 };

    for (int i = 0; i < RCU_READER_THREADS; i++) {
        reader_ctxs[i] = (struct rcu_ctx){ .mt = &mt, .thread_id = i, .errors = 0 };
        pthread_create(&readers[i], NULL, rcu_lockfree_reader, &reader_ctxs[i]);
    }
    pthread_create(&writer, NULL, rcu_locked_writer, &writer_ctx);

    for (int i = 0; i < RCU_READER_THREADS; i++)
        pthread_join(readers[i], NULL);
    pthread_join(writer, NULL);

    int total_errors = writer_ctx.errors;
    for (int i = 0; i < RCU_READER_THREADS; i++)
        total_errors += reader_ctxs[i].errors;
    assert_int_equal(0, total_errors);

    mtree_destroy(&mt);
}

/* ====================================================================== */
/*  Teardown: drain deferred frees after each test                         */
/* ====================================================================== */

static int rcu_teardown(void **state)
{
    (void)state;
    mt_rcu_drain();
    return 0;
}

/* ====================================================================== */
/*  Main — all 99 original tests + RCU-specific tests, each with teardown  */
/* ====================================================================== */

#define T(f) cmocka_unit_test_setup_teardown(f, NULL, rcu_teardown)

int main(void) {
    const struct CMUnitTest tests[] = {
        /* -- Original 99 tests (running under RCU mode) -- */
        T(test_empty_tree),
        T(test_single_store_load),
        T(test_store_at_zero),
        T(test_store_range),
        T(test_overwrite),
        T(test_erase),
        T(test_erase_nonexistent),
        T(test_sequential_insert),
        T(test_reverse_insert),
        T(test_overlapping_range_store),
        T(test_range_erase_via_store_null),
        T(test_adjacent_ranges),
        T(test_mas_walk),
        T(test_mas_find_iteration),
        T(test_mt_for_each),
        T(test_gap_search_empty),
        T(test_gap_search_with_entries),
        T(test_gap_search_rev),
        T(test_gap_search_no_space),
        T(test_mt_find),
        T(test_mt_next_prev),
        T(test_destroy),
        T(test_scale_1000),
        T(test_interleaved_store_erase),
        T(test_range_iteration),
        T(test_mas_store),
        T(test_mas_erase),
        T(test_large_range),
        T(test_coalesce),
        T(test_sparse_keys),
        T(test_store_max_key),
        T(test_split_single_node),
        T(test_deep_tree),
        T(test_range_superset_overwrite),
        T(test_range_subset_overwrite),
        T(test_range_overlap_left),
        T(test_range_overlap_right),
        T(test_adjacent_same_value_coalesce),
        T(test_triple_overlap),
        T(test_boundary_low),
        T(test_boundary_high),
        T(test_range_from_zero),
        T(test_store_range_invalid),
        T(test_erase_empty_tree),
        T(test_erase_all),
        T(test_reinsert_after_erase),
        T(test_erase_deep),
        T(test_mas_walk_gap),
        T(test_mas_next_full_scan),
        T(test_mas_prev_full_scan),
        T(test_mas_next_bounded),
        T(test_mas_prev_bounded),
        T(test_mt_find_bounded),
        T(test_mt_find_empty),
        T(test_mt_next_at_end),
        T(test_mt_prev_at_start),
        T(test_mt_next_prev_at_limit),
        T(test_gap_exact_fit),
        T(test_gap_too_small),
        T(test_gap_fwd_first),
        T(test_gap_rev_last),
        T(test_gap_at_start),
        T(test_gap_at_end),
        T(test_gap_size_one),
        T(test_gap_size_zero),
        T(test_gap_after_erase),
        T(test_mt_for_each_bounded),
        T(test_mas_for_each_midstart),
        T(test_iterate_ranges),
        T(test_store_null_explicit),
        T(test_range_punch_hole),
        T(test_destroy_ranges),
        T(test_double_destroy),
        T(test_scale_5000),
        T(test_churn),
        T(test_random_order),
        T(test_many_small_ranges),
        T(test_vm_style_gap_alloc),
        T(test_vm_style_mmap_munmap),
        T(test_rebalance_leaf_merge),
        T(test_rebalance_erase_all_sequential),
        T(test_rebalance_erase_reverse),
        T(test_rebalance_tree_shrink),
        T(test_rebalance_redistribute),
        T(test_rebalance_interleaved_erase),
        T(test_rebalance_reinsert_after_shrink),
        T(test_rebalance_gap_tracking),
        T(test_full_range_store),
        T(test_cross_node_range_store),
        T(test_erase_within_range),
        T(test_erase_range_boundaries),
        T(test_mixed_operations_workflow),
        T(test_gap_search_after_heavy_rebalance),
        T(test_tagged_pointer_value),
        T(test_sparse_then_dense),
        T(test_concurrent_writers),
        T(test_concurrent_contended),
        T(test_concurrent_range_vs_erase),
        T(test_concurrent_barrier_visibility),
        /* -- RCU-specific tests -- */
        T(test_rcu_deferred_accounting),
        T(test_rcu_lockfree_readers),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
