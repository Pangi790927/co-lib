#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test2 - Semaphores
================================================================================================= */

/* testing the multi-wait initialization of a semaphore */

int test2_counter = 0;
int test2_counter_after = 0;
int test2_counter_before = 0;

co::task_t test2_co_signaler1(co::sem_p a) {
    for (int i = 0; i < 50; i++) {
        a->signal();
        test2_counter++;
    }
    co_return 0;
}

co::task_t test2_co_signaler2(co::sem_p a) {
    for (int i = 0; i < 50; i++) {
        a->signal();
        test2_counter++;
    }
    co_return 0;
}

co::task_t test2_co_waiter(co::sem_p a) {
    test2_counter_before = test2_counter; /* we know that co_awaiter is executed first because
                                          it is the first that was scheduled */
    co_await a->wait();
    test2_counter_after = test2_counter;  /* observe that sem_t::signal does not suspend the
                                          corutine */
    co_return 0;
}

int test2_semaphore() {
    auto pool = co::create_pool();
    auto a = co::create_sem(pool, -99); /* needs to be signaled after 0 */

    pool->sched(test2_co_waiter(a));
    pool->sched(test2_co_signaler1(a));
    pool->sched(test2_co_signaler2(a));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test2_counter_after == 100));
    ASSERT_FN(CHK_BOOL(test2_counter_before == 0));

    return 0;
}

int main() {
    int ret = test2_semaphore();
    print_test_result("001-002-semaphore_multi_wait.cpp", ret >= 0);
    return ret;
}
