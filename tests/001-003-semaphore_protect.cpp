#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test3 - Semaphores
================================================================================================= */

/* testing the semaphore protection */
int test3_counter = 0;
int test3_counter_end = 0;

co::task_t test3_co_protect(co::sem_p a) {
    co_await a->wait();
    for (int i = 0; i < 3; i++) {
        test3_counter++;
        co_await co::yield();
    }
    test3_counter_end = test3_counter;
    a->signal();
    co_return 0;
}

co::task_t test3_co_access(co::sem_p a) {
    co_await a->wait();
    for (int i = 0; i < 3; i++) {
        test3_counter = 0;
        co_await co::yield();
    }
    a->signal();
    co_return 0;
}

int test3_semaphore() {
    auto pool = co::create_pool();

    /* the first one is going to be test3_co_protect, meaning it takes the lock, increment the
    value 3 times and finally the test3_co_access will set it back to 0*/
    auto a = co::create_sem(pool, 1);
    pool->sched(test3_co_protect(a));
    pool->sched(test3_co_access(a));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test3_counter_end == 3));
    ASSERT_FN(CHK_BOOL(test3_counter == 0));

    /* this time both of them will have access to the resource, test3_co_access will be able to
    0 the variable each time */
    test3_counter_end = 0;
    a = co::create_sem(pool, 2);
    pool->sched(test3_co_protect(a));
    pool->sched(test3_co_access(a));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test3_counter_end == 0));
    ASSERT_FN(CHK_BOOL(test3_counter == 0));

    return 0;
}

int main() {
    int ret = test3_semaphore();
    print_test_result("001-003-semaphore_protect.cpp", ret >= 0);
    return ret;
}
