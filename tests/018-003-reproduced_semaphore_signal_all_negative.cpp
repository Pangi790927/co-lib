#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test33 - Reproduced Bugs: sem_t::signal_all() with a negative starting value
================================================================================================= */

/* signal_all() is documented to wake every currently-waiting coroutine. It used to compute
signal(to_awake) unconditionally, where to_awake = number of waiters. If val was already negative
(a valid starting state - semaphores can be created with a negative value for multi-waiter setups,
see 1-9-semaphore_signal_negative.cpp), val += to_awake could land at or below zero, so signal()'s
internal `while (val > 0 && waiting_on_sem.size())` loop woke fewer than to_awake waiters - some
were left hanging despite signal_all() having "run". Fixed in colib.h by adding back the negative
deficit: signal(to_awake - std::min(0, val)), which makes val land exactly on to_awake regardless of
its starting sign, so the wake loop always runs to_awake times. */

int test33_counter = 0;

co::task_t test33_waiter(co::sem_p sem) {
    co_await sem->wait();
    test33_counter++;
    co_return 0;
}

co::task_t test33_signaller(co::sem_p sem) {
    ASSERT_COFN(CHK_BOOL(sem->signal_all() == co::ERROR_OK));
    co_return 0;
}

int test33_signal_all_negative() {
    auto pool = co::create_pool();
    /* val starts negative; 3 real waiters queue up behind it */
    auto sem = co::create_sem(pool, -2);

    pool->sched(test33_waiter(sem));
    pool->sched(test33_waiter(sem));
    pool->sched(test33_waiter(sem));
    pool->sched(test33_signaller(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test33_counter == 3)); /* all 3 waiters woken, not just (3 - 2) = 1 */

    return 0;
}

int main() {
    int ret = test33_signal_all_negative();
    print_test_result("018-003-reproduced_semaphore_signal_all_negative.cpp", ret >= 0);
    return ret;
}
