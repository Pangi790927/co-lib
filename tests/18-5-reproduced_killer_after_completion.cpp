#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test35 - Reproduced Bugs: killer() called after the target already completed naturally
================================================================================================= */

/* Found while chasing a heap-corruption crash surfacing through create_timeo/create_killer's
force-stop machinery: calling a create_killer() kill_fn AFTER its target coroutine has already run
to completion on its own (not force-stopped) must be a clean no-op - never a double-free/UAF on the
already-destroyed coroutine's state. This isn't a single-line diff like the other 18-* tests; it's a
standing regression check for the invariant the io_pool_t/timer_pool_t fixes in this revision
(fd_data_slow iteration-safety, timer_pool_t::get_timer's off-by-one, the timer-start failure code
now correctly propagating ERROR_GENERIC) were made in service of. Companion coverage:
2-2-flowctrl_create_killer.cpp exercises the same kill_fn()-with-nothing-left-to-kill return code,
but on a coroutine parked on a semaphore rather than one that ran to a natural, timer-driven
co_return. */

co::task<int> test35_sleeper() {
    co_await co::sleep_ms(10);
    co_return 123;
}

co::task_t test35_setup_and_run() {
    auto pool = co_await co::get_pool();
    auto t = test35_sleeper();

    auto [mods, kill_fn] = co::create_killer(pool, co::ERROR_WAKEUP);
    co::add_modifs(pool, t, mods); /* not co_awaited: awaiting it here would run/destroy it early */
    co_await co::sched(t);

    /* give the sleeper long enough to finish naturally before we try to kill it */
    co_await co::sleep_ms(50);

    /* nothing left to kill: must return cleanly, not crash */
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_GENERIC));

    co_return 0;
}

int test35_killer_after_completion() {
    auto pool = co::create_pool();
    pool->sched(test35_setup_and_run());
    ASSERT_FN(pool->run());
    return 0;
}

int main() {
    int ret = test35_killer_after_completion();
    print_test_result("18-5-reproduced_killer_after_completion.cpp", ret >= 0);
    return ret;
}
