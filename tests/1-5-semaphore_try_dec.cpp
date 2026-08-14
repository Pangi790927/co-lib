#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test14 - Semaphores: sem_try_dec
================================================================================================= */

int test14_val = 0;

co::task_t test14_coro() {
    auto sem = co_await co::create_sem(3);

    for (int i = 0; i < 6; i++)
        if (sem->try_dec())
            test14_val++;

    co_return 0;
}

int test14_try_dec() {
    co::pool_p pool = co::create_pool();
    pool->sched(test14_coro());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test14_val == 3));
    return 0;
}

int main() {
    int ret = test14_try_dec();
    print_test_result("1-5-semaphore_try_dec.cpp", ret >= 0);
    return ret;
}
