#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdint.h>

#include "maple_tree.h"

#define VAL(n) ((void *)(uintptr_t)(n))

static void init_tree(struct maple_tree *mt)
{
    mt_init(mt);
    mt_set_in_rcu(mt);
}

static void test_rebalance_dead_batch_guard_merge(void **state)
{
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    for (uint64_t i = 0; i < 100; i++)
        assert_int_equal(0, mtree_store(&mt, i, VAL(i + 1)));

    for (uint64_t i = 0; i < 14; i++)
        assert_ptr_equal(VAL(i + 1), mtree_erase(&mt, i));

    for (uint64_t i = 0; i < 14; i++)
        assert_null(mtree_load(&mt, i));
    for (uint64_t i = 14; i < 100; i++)
        assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i));

    mtree_destroy(&mt);
}

static void test_rebalance_dead_batch_guard_redistribute(void **state)
{
    (void)state;
    struct maple_tree mt;
    init_tree(&mt);

    for (uint64_t i = 0; i < 200; i++)
        assert_int_equal(0, mtree_store(&mt, i * 2, VAL(i + 1)));

    for (uint64_t i = 20; i < 28; i++)
        assert_ptr_equal(VAL(i + 1), mtree_erase(&mt, i * 2));

    for (uint64_t i = 0; i < 200; i++) {
        if (i >= 20 && i < 28)
            assert_null(mtree_load(&mt, i * 2));
        else
            assert_ptr_equal(VAL(i + 1), mtree_load(&mt, i * 2));
    }

    mtree_destroy(&mt);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_rebalance_dead_batch_guard_merge),
        cmocka_unit_test(test_rebalance_dead_batch_guard_redistribute),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}
