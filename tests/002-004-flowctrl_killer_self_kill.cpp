#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test49 - Flow Control: a coroutine that kills itself
================================================================================================= */

/* A coroutine can't kill itself: the kill function throws kill_self_t and kills nothing. Caught:
the coroutine goes on as if the kill never happened, and ends by itself. Not caught: the exception
unwinds the coroutine itself, and as for any exception leaving a scheduled root, pool_t::run()
rethrows it. */

static int test49_after_kill = 0;
static int test49_destructed = 0;
static bool test49_thrown = false;

struct test49_marker_t {
    ~test49_marker_t() { test49_destructed++; }
};

static co::task_t test49_caught(std::function<co::error_e(void)> kill_fn) {
    test49_marker_t marker;
    try {
        kill_fn();
    }
    catch (co::kill_self_t &) {
        test49_thrown = true;
    }
    co_await co::yield();       /* not killed: its waits happen */
    test49_after_kill++;
    co_return 0;
}

static co::task_t test49_uncaught(std::function<co::error_e(void)> kill_fn) {
    test49_marker_t marker;
    kill_fn();                  /* throws out of this coroutine */
    test49_after_kill++;        /* must not run */
    co_return 0;
}

int test49_self_kill() {
    {
        auto pool = co::create_pool();
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(co::add_modifs(pool.get(), test49_caught(kill_fn), mods));
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test49_thrown));
        ASSERT_FN(CHK_BOOL(test49_after_kill == 1));
        ASSERT_FN(CHK_BOOL(test49_destructed == 1));
        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));   /* it ended by itself */
    }
    {
        auto pool = co::create_pool();
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(co::add_modifs(pool.get(), test49_uncaught(kill_fn), mods));
        bool escaped = false;
        try {
            pool->run();
        }
        catch (co::kill_self_t &) {
            escaped = true;
        }
        ASSERT_FN(CHK_BOOL(escaped));
        ASSERT_FN(CHK_BOOL(test49_after_kill == 1));
        ASSERT_FN(CHK_BOOL(test49_destructed == 2));
    }
    return 0;
}

int main() {
    int ret = test49_self_kill();
    print_test_result("002-004-flowctrl_killer_self_kill.cpp", ret >= 0);
    return ret;
}
