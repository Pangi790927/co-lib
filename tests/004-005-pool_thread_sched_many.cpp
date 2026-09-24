#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <thread>
#include <vector>

/* Test72 - Pool: pool_t::thread_sched() from many threads at once
================================================================================================= */

/* Several threads hand coroutines to the same running pool with thread_sched(), concurrently: every
one of them must run exactly once, on the pool's thread. The pool keeps itself busy with a
coroutine that sleeps in short steps until they all ran (or a deadline passes). */

constexpr int test72_threads = 8;
constexpr int test72_per_thread = 200;

static std::atomic<int> test72_ran = 0;
static std::atomic<int> test72_wrong_thread = 0;
static std::thread::id test72_pool_thread;

static co::task_t test72_from_thread() {
    if (std::this_thread::get_id() != test72_pool_thread)
        test72_wrong_thread++;
    test72_ran++;
    co_await co::yield();       /* and it behaves like any scheduled coroutine */
    co_return 0;
}

static co::task_t test72_keep_busy() {
    for (int i = 0; i < 5000 && test72_ran < test72_threads * test72_per_thread; i++)
        co_await co::sleep_ms(1);
    co_return 0;
}

int test72_thread_sched_many() {
    test72_pool_thread = std::this_thread::get_id();
    auto pool = co::create_pool();
    pool->sched(test72_keep_busy());

    std::vector<std::thread> threads;
    for (int t = 0; t < test72_threads; t++)
        threads.emplace_back([&pool] {
            for (int i = 0; i < test72_per_thread; i++)
                pool->thread_sched(test72_from_thread());
        });

    ASSERT_FN(pool->run());
    for (auto &t : threads)
        t.join();

    DBG("ran: %d", test72_ran.load());
    ASSERT_FN(CHK_BOOL(test72_ran == test72_threads * test72_per_thread));
    ASSERT_FN(CHK_BOOL(test72_wrong_thread == 0));
    return 0;
}

int main() {
    int ret = test72_thread_sched_many();
    print_test_result("004-005-pool_thread_sched_many.cpp", ret >= 0);
    return ret;
}
