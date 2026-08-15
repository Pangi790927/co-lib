#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test15 - Modifs: CO_MODIF_CALL_CBK and CO_MODIF_SCHED_CBK
================================================================================================= */

/* CO_MODIF_CALL_CBK: "called when a task is called (on the task), via 'co_await task'" - fires on
a coroutine that is directly co_await-ed by another.
CO_MODIF_SCHED_CBK: "called on the corutine that is scheduled" - fires on a coroutine started via
pool->sched(). These are the two distinct coroutine start paths; this test checks each modif fires
only for its own path, not both. */

int test15_call_cnt = 0;
int test15_sched_cnt = 0;

co::task_t test15_awaited_child() {
    co_return 0;
}

co::task_t test15_scheduled_child() {
    co_return 0;
}

co::task_t test15_parent(co::task_t child) {
    co_await child; /* directly awaited (not scheduled): should trigger CALL_CBK on `child` */
    co_return 0;
}

int test15_modifs() {
    auto pool = co::create_pool();

    auto call_modif = co::create_modif<co::CO_MODIF_CALL_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e {
            test15_call_cnt++;
            return co::ERROR_OK;
        });
    auto sched_modif = co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e {
            test15_sched_cnt++;
            return co::ERROR_OK;
        });

    /* attach call_modif to both children: the awaited one should trigger it, the scheduled one
    should NOT, proving CALL_CBK is specific to the co_await path */
    auto awaited_child = co::add_modifs(pool.get(), test15_awaited_child(), {call_modif});
    auto scheduled_child = co::add_modifs(pool.get(), test15_scheduled_child(), {call_modif});

    pool->sched(test15_parent(awaited_child));
    pool->sched(scheduled_child, {sched_modif});

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    ASSERT_FN(CHK_BOOL(test15_call_cnt == 1));  /* only awaited_child's CALL_CBK fired */
    ASSERT_FN(CHK_BOOL(test15_sched_cnt == 1)); /* only scheduled_child's SCHED_CBK fired */

    return 0;
}

int main() {
    int ret = test15_modifs();
    print_test_result("011-001-modifs.cpp", ret >= 0);
    return ret;
}
