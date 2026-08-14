#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test25 - Semaphores: destruction with waiting coroutines
================================================================================================= */

/* colib.h docs on ~sem_t(): "If the semaphore dies while waiters wait, they will all be forcefully
destroyed (their entire call stack)". Confirmed in the implementation: ~sem_t() calls
get_internal()->clear(0), the same destructive path as sem_t::clear() (see 20-1-sem_clear.cpp). */

int test25_destruct_cnt = 0;
int test25_resumed_cnt = 0;

struct test25_marker_t {
    ~test25_marker_t() { test25_destruct_cnt++; }
};

co::task_t test25_waiter(co::sem_t *sem) {
    /* raw pointer, not sem_p: a sem_p parameter would keep its own shared_ptr reference alive in
    this coroutine's suspended frame, so the sem's refcount would never hit zero below */
    test25_marker_t marker;
    co_await sem->wait();
    test25_resumed_cnt++; /* must NOT run */
    co_return 0;
}

int test25_sem_destruction() {
    auto pool = co::create_pool();
    {
        auto sem = co::create_sem(pool, 0);

        pool->sched(test25_waiter(sem.get()));
        pool->sched(test25_waiter(sem.get()));

        co::run_e ret = pool->run();
        ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
        ASSERT_FN(CHK_BOOL(test25_destruct_cnt == 0)); /* both still parked, waiting on the sem */
    } /* `sem` (the only sem_p) goes out of scope here -> ~sem_t() -> clear(0) -> force-destroys waiters */

    ASSERT_FN(CHK_BOOL(test25_destruct_cnt == 2));
    ASSERT_FN(CHK_BOOL(test25_resumed_cnt == 0));

    return 0;
}

int main() {
    int ret = test25_sem_destruction();
    print_test_result("001-008-semaphore_destruction.cpp", ret >= 0);
    return ret;
}
