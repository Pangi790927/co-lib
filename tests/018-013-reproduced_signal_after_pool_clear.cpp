#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test44 - Reproduced Bugs: using a semaphore after its pool cleared it dereferenced null
================================================================================================= */

/* pool_t::clear() - which ~pool_t also runs - destroys every coroutine waiting on the pool's
semaphores and then invalidates each semaphore, setting its `internal` to null, so that a sem_t
outliving its pool "won't do anything anymore". sem_t objects are allocated with the global
allocator for exactly that reason: they may be held past the pool. ~sem_t honoured the
invalidation, but signal(), signal_all(), try_dec() and clear() did not - each dereferenced the
null `internal`, although signal()'s own comment promised an error for a pool that had gone.

Found by homeauto's core: an object holding a semaphore was destroyed after its pool had been
cleared, and signalled the waiters it believed it still had. Fixed in colib.h by having the four
members answer ERROR_GENERIC (try_dec(): false) on an invalidated semaphore. Both ways a semaphore
outlives its pool are covered: the pool cleared but alive, and the pool destroyed. 2026-09-23 04:52 */

/* Something to wait on the semaphore, so clear() has a waiter to destroy too. */
static co::task_t test44_waiter(co::sem_p sem) {
    co_await sem->wait();
    co_return 0;
}

/* Answers 0 if every member of an invalidated semaphore does nothing and says so. */
static int test44_members_answer_errors(co::sem_p sem) {
    ASSERT_FN(CHK_BOOL(sem->signal() == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(sem->signal_all() == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(sem->try_dec() == false));
    ASSERT_FN(CHK_BOOL(sem->clear(0) == co::ERROR_GENERIC));
    return 0;
}

int test44_signal_after_pool_clear() {
    /* the pool cleared, still alive */
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    pool->sched(test44_waiter(sem));
    ASSERT_FN(pool->run());
    ASSERT_FN(pool->clear());
    ASSERT_FN(test44_members_answer_errors(sem));

    /* the pool destroyed, which clears it on the way */
    auto pool2 = co::create_pool();
    auto sem2 = co::create_sem(pool2, 1);
    pool2 = nullptr;
    ASSERT_FN(test44_members_answer_errors(sem2));

    return 0;
}

int main() {
    int ret = test44_signal_after_pool_clear();
    print_test_result("018-013-reproduced_signal_after_pool_clear.cpp", ret >= 0);
    return ret;
}
