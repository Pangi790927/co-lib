#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_MULTITHREAD_SCHED true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <thread>

/* Test71 - Pool: pool_t::thread_sched() with the debug checks on
================================================================================================= */

/* The same as 004-003, with COLIB_ENABLE_DEBUG_CHECKS: a coroutine handed over by thread_sched() is
a scheduled coroutine like any other, so it must get its SCHED before its first ENTER - the debug
checks abort ("never called, but entered") otherwise. */

static std::atomic<bool> test71_scheduled = false;
static std::atomic<bool> test71_ran = false;
static std::thread::id test71_ran_on;

static co::task_t test71_from_thread() {
    test71_ran_on = std::this_thread::get_id();
    test71_ran = true;
    co_return 0;
}

static co::task_t test71_keep_busy() {
    for (int i = 0; i < 2000 && !test71_ran; i++)
        co_await co::sleep_ms(1);
    co_return 0;
}

int test71_thread_sched() {
    auto pool = co::create_pool();
    pool->sched(test71_keep_busy());

    std::thread other([&pool] {
        pool->thread_sched(test71_from_thread());
        test71_scheduled = true;
    });

    ASSERT_FN(pool->run());
    other.join();

    ASSERT_FN(CHK_BOOL(test71_scheduled));
    ASSERT_FN(CHK_BOOL(test71_ran));
    ASSERT_FN(CHK_BOOL(test71_ran_on == std::this_thread::get_id()));    /* on the pool's thread */
    return 0;
}

int main() {
    int ret = test71_thread_sched();
    print_test_result("004-004-pool_thread_sched_debug_checks.cpp", ret >= 0);
    return ret;
}
