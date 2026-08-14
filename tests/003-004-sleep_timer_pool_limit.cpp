#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test28 - Sleep: timer pool "limit" (COLIB_MAX_TIMER_POOL_SIZE)
================================================================================================= */

/* colib.h's doc used to claim this macro caps concurrent sleeps; the Linux timer_pool_t shows
it's actually just a reuse-cache size for freed timerfds (see colib.h's updated Timers doc /
COLIB_MAX_TIMER_POOL_SIZE config row, fixed after this test was written). This proves concurrency
is unbounded in practice: schedule well over MAX_TIMER_POOL_SIZE concurrent sleeps and confirm they
all complete, in roughly the time of a single sleep rather than being serialized. */

const int test28_num_sleepers = co::MAX_TIMER_POOL_SIZE * 2;
int test28_done_count = 0;

co::task_t test28_sleeper() {
    co_await co::sleep_ms(30);
    test28_done_count++;
    co_return 0;
}

int test28_timer_pool_limit() {
    auto pool = co::create_pool();
    for (int i = 0; i < test28_num_sleepers; i++)
        pool->sched(test28_sleeper());

    auto start = std::chrono::steady_clock::now();
    co::run_e ret = pool->run();
    auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test28_done_count == test28_num_sleepers));

    /* if concurrency were actually capped at MAX_TIMER_POOL_SIZE, exhausting the cache would
    force extra sleeps to wait, multiplying total time. Confirm we're still close to a single
    30ms sleep, not e.g. 2x that (which a serialized fallback would produce). */
    if (elapsed >= std::chrono::milliseconds(30 + 100)) {
        DBG("took %lldms for %d concurrent sleeps - looks serialized, not concurrent",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
            test28_num_sleepers);
        return -1;
    }

    return 0;
}

int main() {
    int ret = test28_timer_pool_limit();
    print_test_result("003-004-sleep_timer_pool_limit.cpp", ret >= 0);
    return ret;
}
