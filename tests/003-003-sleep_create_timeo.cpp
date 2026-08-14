#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test27 - Timing: create_timeo
================================================================================================= */

/* create_timeo(t, pool, timeo) races task `t` against a timer. If `t` finishes first: result is
{t's return value, ERROR_OK}. If the timer elapses first: `t` is forcibly killed (same mechanism
as create_killer, see 2-2-flowctrl_create_killer.cpp) and the result is {default T, ERROR_TIMEO}. */

co::task<int> test27_fast_task() {
    co_await co::sleep_ms(10);
    co_return 42;
}

int test27_destruct_cnt = 0;

struct test27_marker_t {
    ~test27_marker_t() { test27_destruct_cnt++; }
};

co::task<int> test27_slow_task() {
    test27_marker_t marker;
    co_await co::sleep_s(10); /* far longer than the 50ms timeout used below */
    test27_destruct_cnt += 1000; /* must NOT run: killed by the timeout before this */
    co_return 99;
}

co::task_t test27_case_completes(co::pool_p pool) {
    auto [val, err] = co_await co::create_timeo(
            test27_fast_task(), pool.get(), std::chrono::milliseconds(200));
    ASSERT_COFN(CHK_BOOL(err == co::ERROR_OK));
    ASSERT_COFN(CHK_BOOL(val == 42));
    co_return 0;
}

co::task_t test27_case_times_out(co::pool_p pool) {
    auto [val, err] = co_await co::create_timeo(
            test27_slow_task(), pool.get(), std::chrono::milliseconds(50));
    (void)val;
    ASSERT_COFN(CHK_BOOL(err == co::ERROR_TIMEO));
    co_return 0;
}

int test27_create_timeo() {
    auto pool = co::create_pool();
    pool->sched(test27_case_completes(pool));
    pool->sched(test27_case_times_out(pool));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test27_destruct_cnt == 1)); /* slow_task's stack was force-destroyed */

    return 0;
}

int main() {
    int ret = test27_create_timeo();
    print_test_result("003-003-sleep_create_timeo.cpp", ret >= 0);
    return ret;
}
