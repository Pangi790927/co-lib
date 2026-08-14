#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test1 - Semaphores
================================================================================================= */

/* testing ping-pong usage of the semaphore */

bool test1_done = false;

co::task_t test1_co_even(co::sem_p a, co::sem_p b) {
    volatile int64_t i = 0;
    while (true) {
        co_await a->wait();
        if (i % 100'000L == 0)
            DBG("the coro: %ld", i);
        i = i + 1;
        b->signal();
        if (i > 1'000'000) {
            test1_done = true;
            break;
        }
    }
    co_return 0;
}

co::task_t test1_co_odd(co::sem_p a, co::sem_p b) {
    while (true) {
        co_await b->wait();
        if (test1_done)
            break;
        a->signal();
    }
    co_return 0;
}

int test1_semaphore() {
    auto pool = co::create_pool();
    auto a = co::create_sem(pool, 1);
    auto b = co::create_sem(pool, 0);

    pool->sched(COLIB_REGNAME(test1_co_odd(a, b)));
    pool->sched(COLIB_REGNAME(test1_co_even(a, b)));

    DBG("Run");

    ASSERT_FN(pool->run());

    return 0;
}

int main() {
    int ret = test1_semaphore();
    print_test_result("1-1-semaphore_ping_pong.cpp", ret >= 0);
    return ret;
}
