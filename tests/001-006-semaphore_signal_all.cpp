#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test23 - Semaphores: sem_t::signal_all()
================================================================================================= */

int test23_counter = 0;

co::task_t test23_waiter(co::sem_p sem) {
    co_await sem->wait();
    test23_counter++;
    co_return 0;
}

co::task_t test23_signal_all_caller(co::sem_p sem) {
    /* by the time this coroutine runs, the 3 waiters scheduled before it have already run to
    their first suspend point (co_await sem->wait()), since the ready queue is FIFO */
    ASSERT_COFN(CHK_BOOL(sem->signal_all() == co::ERROR_OK));
    co_return 0;
}

int test23_signal_all() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    pool->sched(test23_waiter(sem));
    pool->sched(test23_waiter(sem));
    pool->sched(test23_waiter(sem));
    pool->sched(test23_signal_all_caller(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test23_counter == 3)); /* all 3 woken by a single signal_all() call */

    return 0;
}

int main() {
    int ret = test23_signal_all();
    print_test_result("001-006-semaphore_signal_all.cpp", ret >= 0);
    return ret;
}
