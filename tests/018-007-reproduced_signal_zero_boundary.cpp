#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test39 - Reproduced Bugs: sem_t::signal(0) vs its own doc comment - open question, not yet fixed
================================================================================================= */

/* UNRESOLVED - see BUGS.md and the `TODO: BUG:` on sem_t::signal()'s doc comment in colib.h. This is
NOT a confirmed defect awaiting a fix - it's deliberately left open. signal()'s doc comment says: "If
increment is 0 and the internal counter is less then or equal to 0 then it will awake all the
waiters, else it does nothing." The implementation instead checks `inc == 0 && val < 0` - strictly
less than, missing the val == 0 case: the ordinary resting state for a semaphore with waiters queued
through normal wait() calls (await_ready() only ever decrements val when val > 0, so a waiter that
has to suspend never touches val at all).

Fixing the implementation to match the doc would change signal(0)'s observable behavior at val == 0;
nothing in this repo currently calls signal(0) at all, so that risk is specifically about consumers
outside this repo that aren't visible from here. Left unresolved on purpose until that's checked.

This test asserts the currently-documented behavior and is expected to fail until a side is chosen.
If the implementation gets fixed to match the doc, this file should start passing as-is. If the doc
gets narrowed to match the implementation instead, this test's assertion needs to flip along with it
(val == 0 should then assert nobody gets woken) - don't just delete it either way. */

int test39_counter = 0;

co::task_t test39_waiter(co::sem_p sem) {
    co_await sem->wait();
    test39_counter++;
    co_return 0;
}

co::task_t test39_signaller(co::sem_p sem) {
    /* by the time this runs, the 2 waiters scheduled before it have already run to their first
    suspend point (co_await sem->wait()), since the ready queue is FIFO - val is still exactly 0 */
    ASSERT_COFN(CHK_BOOL(sem->signal(0) == co::ERROR_OK));
    co_return 0;
}

int test39_signal_zero_boundary() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    pool->sched(test39_waiter(sem));
    pool->sched(test39_waiter(sem));
    pool->sched(test39_signaller(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test39_counter == 2)); /* both waiters woken by signal(0) per the docs */

    return 0;
}

int main() {
    int ret = test39_signal_zero_boundary();
    print_test_result("018-007-reproduced_signal_zero_boundary.cpp", ret >= 0);
    return ret;
}
