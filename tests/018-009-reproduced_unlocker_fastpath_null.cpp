#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test41 - Reproduced Bugs: unlocker_t is null even for a legitimate fast-path acquire
================================================================================================= */

/* OPEN BUG - regression introduced by the in-progress fix for BUGS.md #3 (see
018-008-reproduced_unlocker_spurious_signal.cpp). Not resolved yet: this test is expected to fail
until it is. Once resolved, this file stays as-is so the bug can't silently come back.

sem_awaiter_t::triggered is false for two DIFFERENT reasons, not one:
  1. The wait was aborted by a CO_MODIF_WAIT_SEM_CBK modif returning an error (the case
     018-008 covers) - the counter was never touched, a null/no-op unlocker is correct here.
  2. The fast path: await_ready() itself returned true because the counter was already > 0, and
     decremented it right there. await_suspend() (the only place that sets triggered = true) never
     even runs in this case, per the normal C++ coroutine protocol - so triggered stays false here
     too, even though a real, legitimate acquire just happened.

await_resume() currently can't tell these two apart - it only has `triggered` to go on, and both
paths leave it false. Returning unlocker_t(nullptr) unconditionally whenever triggered is false (the
fix applied for BUGS.md #3) correctly handles case 1, but wrongly nulls out case 2 as well: the
counter was genuinely decremented, but the caller gets a null unlocker back. unlocker_t::unlock() is
now a safe no-op on a null sem (a separate, already-applied fix - it no longer crashes/UB's), but
that safety doesn't help here: it just means case 2's .unlock() call quietly does nothing instead of
restoring the counter. Net effect: every legitimate fast-path acquire+release permanently leaks one
count off the semaphore, since the release that's supposed to balance it does nothing. */

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
