#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <thread>

/* Test70 - Pool: pool_t::thread_sched() from another thread
================================================================================================= */

/* With COLIB_ENABLE_MULTITHREAD_SCHED, another thread can hand a new coroutine to a running pool
with thread_sched(). The pool picks it up the next time it looks for a ready task: here the pool
keeps itself busy with a coroutine that sleeps in short steps until the thread's coroutine ran (or
a deadline passes), so it doesn't depend on thread_sched() waking a pool blocked in its io wait. */

static std::atomic<bool> test70_scheduled = false;
static std::atomic<bool> test70_ran = false;
static std::thread::id test70_ran_on;

static co::task_t test70_from_thread() {
    test70_ran_on = std::this_thread::get_id();
    test70_ran = true;
    co_return 0;
}

static co::task_t test70_keep_busy() {
    for (int i = 0; i < 2000 && !test70_ran; i++)
        co_await co::sleep_ms(1);
    co_return 0;
}

int test70_thread_sched() {
    auto pool = co::create_pool();
    pool->sched(test70_keep_busy());

    std::thread other([&pool] {
        pool->thread_sched(test70_from_thread());
        test70_scheduled = true;
    });

    ASSERT_FN(pool->run());
    other.join();

    ASSERT_FN(CHK_BOOL(test70_scheduled));
    ASSERT_FN(CHK_BOOL(test70_ran));
    ASSERT_FN(CHK_BOOL(test70_ran_on == std::this_thread::get_id()));    /* on the pool's thread */
    return 0;
}

int main() {
    int ret = test70_thread_sched();
    print_test_result("004-003-pool_thread_sched.cpp", ret >= 0);
    return ret;
}
