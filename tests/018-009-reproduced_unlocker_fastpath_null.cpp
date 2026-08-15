#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test41 - Reproduced Bugs: unlocker_t is null even for a legitimate fast-path acquire
================================================================================================= */

/* Was a regression introduced partway through fixing the bug 018-008 covers (an intermediate fix
made await_resume() return a null unlocker whenever `triggered` was false, which is correct for the
aborted-wait case 018-008 covers but wrongly also caught the fast-path case below) - now fixed
alongside it, by tracking three distinct await_state_e values instead of one bool: AWAITER_NOT_CALLED
(aborted, correctly null), AWAITER_READY_LAST (fast path, now correctly a real unlocker),
AWAITER_SUSPEND_LAST (normal suspend-then-wake, unchanged). This file is the regression test for the
fast-path case specifically.

sem_awaiter_t::triggered is false for two DIFFERENT reasons, not one:
  1. The wait was aborted by a CO_MODIF_WAIT_SEM_CBK modif returning an error (the case
     018-008 covers) - the counter was never touched, a null/no-op unlocker is correct here.
  2. The fast path: await_ready() itself returned true because the counter was already > 0, and
     decremented it right there. await_suspend() (the only place that sets triggered = true) never
     even runs in this case, per the normal C++ coroutine protocol - so triggered stays false here
     too, even though a real, legitimate acquire just happened.

A single `triggered` bool can't tell these two apart - the fix replaces it with await_state_e so
await_resume() can: AWAITER_NOT_CALLED (default - case 1, aborted) gets a null unlocker,
AWAITER_READY_LAST (case 2, fast path - set directly in await_ready()) gets a real one, same as
AWAITER_SUSPEND_LAST (the normal suspend-then-wake path). */

int test41_unlocker_fastpath_null() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 1); /* val starts > 0: guarantees await_ready() takes the fast path */

    auto task = [&]() -> co::task_t {
        auto unlocker = co_await sem->wait(); /* fast path: decrements val to 0 right here */
        unlocker.unlock(); /* legitimate release - must restore the counter */
        co_return 0;
    };
    pool->sched(task());

    ASSERT_FN(pool->run());

    /* unlocking a legitimate fast-path acquire must actually restore the counter */
    ASSERT_FN(CHK_BOOL(sem->try_dec() == true));

    return 0;
}

int main() {
    int ret = test41_unlocker_fastpath_null();
    print_test_result("018-009-reproduced_unlocker_fastpath_null.cpp", ret >= 0);
    return ret;
}
