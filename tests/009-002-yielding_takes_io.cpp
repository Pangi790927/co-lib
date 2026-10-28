#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <chrono>

/* Test93 - Yielding: a yield lets the pool take the io that is ready
=================================================================================================
A coroutine that yields in a loop keeps the pool's ready queue from ever running empty. A yield
still lets the pool take the io and the timers that are ready, without waiting for any, so a
coroutine that yields until a sleeping one sets a flag sees the flag once the sleep ends, and
create_timeo() ends a computation that yields once its timer expires. Each case gives up after
two seconds of yielding, so a pool that never takes the io fails the case instead of hanging. */

using namespace std::chrono_literals;

static constexpr auto test93_budget = 2s;

/* case 1: a yield loop sees a sleep end */
static bool test93_flag = false;
static bool test93_flag_seen = false;

static co::task_t test93_setter() {
    co_await co::sleep_ms(10);
    test93_flag = true;
    co_return 0;
}

static co::task_t test93_spinner() {
    auto start = std::chrono::steady_clock::now();
    while (!test93_flag && std::chrono::steady_clock::now() - start < test93_budget)
        co_await co::yield();
    test93_flag_seen = test93_flag;
    co_return 0;
}

int test93_yield_sees_sleep() {
    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test93_spinner()));
    pool->sched(COLIB_REGNAME(test93_setter()));
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    if (!test93_flag_seen)
        DBG("the yield loop gave up before it saw the sleep end");
    ASSERT_FN(CHK_BOOL(test93_flag_seen));
    return 0;
}

/* case 2: create_timeo() ends a computation that yields */
static co::task<int> test93_crunch() {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < test93_budget)
        co_await co::yield();
    co_return 1;
}

static int test93_timeo_ret = -1;
static co::error_e test93_timeo_err = co::ERROR_OK;
static int64_t test93_timeo_ms = -1;

static co::task_t test93_timeout_user() {
    co::pool_t *pool = co_await co::get_pool();
    auto start = std::chrono::steady_clock::now();
    auto [ret, err] = co_await co::create_timeo(COLIB_REGNAME(test93_crunch()), pool, 20ms);
    test93_timeo_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
    test93_timeo_ret = ret;
    test93_timeo_err = err;
    co_return 0;
}

int test93_timeout_ends_yielder() {
    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test93_timeout_user()));
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    DBG("create_timeo: ret %d, err %s, after %lld ms", test93_timeo_ret,
            co::dbg_enum(test93_timeo_err).c_str(), (long long)test93_timeo_ms);
    ASSERT_FN(CHK_BOOL(test93_timeo_err == co::ERROR_TIMEO));
    ASSERT_FN(CHK_BOOL(test93_timeo_ret == 0));
    ASSERT_FN(CHK_BOOL(test93_timeo_ms < 1000));
    return 0;
}

int main() {
    int ret1 = test93_yield_sees_sleep();
    int ret2 = test93_timeout_ends_yielder();
    int ret = ret1 < 0 ? ret1 : ret2;
    print_test_result("009-002-yielding_takes_io.cpp", ret >= 0);
    return ret;
}
