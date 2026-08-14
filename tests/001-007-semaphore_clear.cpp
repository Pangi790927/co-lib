#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test24 - Semaphores: sem_t::clear()
================================================================================================= */

/* colib.h docs: clear() "Resets the semaphore, wakes up all waiters and destroys them before
resuming and re-initializes the semaphore to the given value." i.e. waiters are forcefully
destroyed (their whole call stack unwinds via destructors), not resumed normally. */

int test24_destruct_cnt = 0;
int test24_resumed_cnt = 0;

struct test24_marker_t {
    ~test24_marker_t() { test24_destruct_cnt++; }
};

co::task_t test24_waiter(co::sem_p sem) {
    test24_marker_t marker;
    co_await sem->wait();
    test24_resumed_cnt++; /* must NOT run: clear() destroys the waiter instead of resuming it */
    co_return 0;
}

co::task_t test24_clearer(co::sem_p sem) {
    ASSERT_COFN(CHK_BOOL(sem->clear(5) == co::ERROR_OK));
    co_return 0;
}

int test24_sem_clear() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    pool->sched(test24_waiter(sem));
    pool->sched(test24_waiter(sem));
    pool->sched(test24_clearer(sem));

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test24_destruct_cnt == 2)); /* both waiters force-destroyed */
    ASSERT_FN(CHK_BOOL(test24_resumed_cnt == 0));  /* neither resumed normally */

    /* clear(5) re-initialized the counter to 5 */
    int ok_count = 0;
    for (int i = 0; i < 6; i++)
        if (sem->try_dec())
            ok_count++;
    ASSERT_FN(CHK_BOOL(ok_count == 5));

    return 0;
}

int main() {
    int ret = test24_sem_clear();
    print_test_result("001-007-semaphore_clear.cpp", ret >= 0);
    return ret;
}
