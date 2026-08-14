#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test7 - Pool: Clear
================================================================================================= */

int test7_destruct_cnt = 0;
std::string test7_prev = "";
struct test7_destruct_t {
    std::string curr;
    std::string prev;
    test7_destruct_t(std::string curr, std::string prev) : prev(prev), curr(curr) {}
    ~test7_destruct_t() {
        /* we make sure their order is clear and known */
        if (prev == test7_prev)
            test7_destruct_cnt++;
        test7_prev = curr;
    }
};

co::task_t test7_on_semaphore(co::sem_p sem, co::sem_p done){
    /* the semaphore wait dies first after timer because it is the first wait on the semaphore sem,
    so the first in queue */
    test7_destruct_t d("semaphore", "timer");
    done->signal();
    co_await sem->wait();
    co_return 0;
}

co::task_t test7_on_timer(co::sem_p done){
    /* The timer dies first because it is io, no previous */
    test7_destruct_t d("timer", "");
    done->signal();
    co_await co::sleep_ms(1000);
    co_return 0;
}

co::task_t test7_callee(co::sem_p sem, co::sem_p done) {
    /* calle is the second waiter on the semaphore sem, so he dies after semaphore */
    test7_destruct_t d("callee", "semaphore");
    done->signal();
    co_await sem->wait();
    co_return 0;
}

co::task_t test7_on_call(co::sem_p sem, co::sem_p done) {
    /* call is the caller of callee, so he dies right after callee */
    test7_destruct_t d("call", "callee");
    done->signal();
    co_await test7_callee(sem, done);
    co_return 0;
}

co::task_t test7_stopper(co::sem_p done) {
    /* stopper initiated the force_stop so he is in the ready_task queue, he will die last */
    test7_destruct_t d("stopper", "call");
    co_await done->wait();
    co_await co::force_stop();
    co_return 0;
}

int test7_clearing() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto done = co::create_sem(pool, -3);

    pool->sched(test7_on_semaphore(sem, done));
    pool->sched(test7_on_timer(done));
    pool->sched(test7_on_call(sem, done));
    pool->sched(test7_stopper(done));

    auto ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_STOPPED));
    ASSERT_FN(CHK_BOOL(test7_destruct_cnt == 0));
    ASSERT_FN(pool->clear());
    ASSERT_FN(CHK_BOOL(test7_destruct_cnt == 5));
    return 0;
}

int main() {
    int ret = test7_clearing();
    print_test_result("4-1-pool_clear.cpp", ret >= 0);
    return ret;
}
