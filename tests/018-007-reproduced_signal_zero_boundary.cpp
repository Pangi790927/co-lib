#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test39 - Reproduced Bugs: sem_t::signal(0) is a no-op in every state
================================================================================================= */

/* signal(0) does nothing, whatever the counter is. This isn't special-cased in sem_internal_t::
signal() - it falls out of the general rule (`val += inc`, then drain while `val > 0` and waiters
remain). A waiter only ever queues while the counter is <= 0, because await_ready() decrements and
returns true whenever it is > 0; and every signal() drains until the counter is <= 0 or the wait
list is empty. So a positive counter and a queued waiter never coexist, and after `val += 0` the
drain loop has nothing to do.

Three states are covered below, because they fail differently if the no-op property ever breaks:
  - val == 0 with waiters queued  - the ordinary resting state of a semaphore being waited on. This
    is the one an earlier `inc == 0` special case in signal() got wrong in the other direction (it
    checked `val < 0`); a regression here means a mass wake-up at the most common state there is,
    which for a create_sem(pool, 1) mutex means every queued coroutine believes it holds the lock.
  - val < 0 with waiters queued   - the countdown-latch idiom (create_sem(pool, -N)). signal(0) must
    not wake anyone and must not reset the deficit; clear(0) is the call that resets it.
  - val > 0 with no waiters       - signal(0) must leave the counter untouched, so a subsequent
    wait() still takes the non-suspending fast path exactly val times. */

int test39_woken = 0;

co::task_t test39_waiter(co::sem_p sem) {
    co_await sem->wait();
    test39_woken++;
    co_return 0;
}

/* by the time this runs, the waiters scheduled before it have already reached their first suspend
point (co_await sem->wait()), since the ready queue is FIFO - the counter is untouched by them */
co::task_t test39_signal_zero(co::sem_p sem) {
    ASSERT_COFN(CHK_BOOL(sem->signal(0) == co::ERROR_OK));
    co_return 0;
}

/* val == 0, two waiters queued: signal(0) wakes neither */
int test39_zero_counter() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    test39_woken = 0;
    pool->sched(test39_waiter(sem));
    pool->sched(test39_waiter(sem));
    pool->sched(test39_signal_zero(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test39_woken == 0));

    return 0;
}

/* val < 0, two waiters queued: signal(0) wakes neither and leaves the deficit alone. Proven by
then signalling exactly enough to cross 0 and wake one: from -2 that takes 3 signals. If signal(0)
had reset the counter to 0 (or woken everyone), the counts below would differ. */
co::task_t test39_signal_zero_then_climb(co::sem_p sem) {
    ASSERT_COFN(CHK_BOOL(sem->signal(0) == co::ERROR_OK));
    ASSERT_COFN(CHK_BOOL(test39_woken == 0));

    ASSERT_COFN(CHK_BOOL(sem->signal(1) == co::ERROR_OK)); /* -1 */
    ASSERT_COFN(CHK_BOOL(sem->signal(1) == co::ERROR_OK)); /*  0 */
    co_await co::yield();
    ASSERT_COFN(CHK_BOOL(test39_woken == 0));              /* still nobody: never went positive */

    ASSERT_COFN(CHK_BOOL(sem->signal(1) == co::ERROR_OK)); /* +1 -> wakes exactly one, back to 0 */
    co_await co::yield();
    ASSERT_COFN(CHK_BOOL(test39_woken == 1));

    co_return 0;
}

int test39_negative_counter() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, -2);

    test39_woken = 0;
    pool->sched(test39_waiter(sem));
    pool->sched(test39_waiter(sem));
    pool->sched(test39_signal_zero_then_climb(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test39_woken == 1)); /* the second waiter is still parked at pool teardown */

    return 0;
}

/* val > 0, no waiters: signal(0) leaves the counter at 2, so try_dec() succeeds exactly twice */
co::task_t test39_positive_counter_co(co::sem_p sem) {
    ASSERT_COFN(CHK_BOOL(sem->signal(0) == co::ERROR_OK));

    ASSERT_COFN(CHK_BOOL(sem->try_dec() == true));
    ASSERT_COFN(CHK_BOOL(sem->try_dec() == true));
    ASSERT_COFN(CHK_BOOL(sem->try_dec() == false));

    co_return 0;
}

int test39_positive_counter() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 2);

    pool->sched(test39_positive_counter_co(sem));

    ASSERT_FN(pool->run());

    return 0;
}

int test39_signal_zero_boundary() {
    ASSERT_FN(test39_zero_counter());
    ASSERT_FN(test39_negative_counter());
    ASSERT_FN(test39_positive_counter());

    return 0;
}

int main() {
    int ret = test39_signal_zero_boundary();
    print_test_result("018-007-reproduced_signal_zero_boundary.cpp", ret >= 0);
    return ret;
}
