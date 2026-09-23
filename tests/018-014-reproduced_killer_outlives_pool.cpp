#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test45 - Reproduced Bugs: a killer outliving its pool freed into the dead pool
================================================================================================= */

/* create_killer() kept its state in the pool's allocator - alloc<kill_state_t>(pool), freed
through dealloc_create(pool) - and its WAIT_SEM modif stored the semaphore's waiter handle,
itself pool-allocated by push_waiter(), keeping it after the wait ended. The killer and its modif
pack are the user's to hold, like a modif_p (see 018-010), and nothing about holding them keeps the
pool alive: destroying them after the pool freed both blocks through a pool that no longer
existed. Fixed in colib.h the way 018-010 was: the kill state is a plain std::make_shared, and the
waiter handle is let go of when the wait ends.

Two holders are covered: a killer whose task never waited on anything, and one whose task waited
on a semaphore and was woken, which is the path that kept the waiter handle. 2026-09-23 05:05 */

/* Waits on the semaphore once and ends. */
static co::task_t test45_waiter(co::sem_p sem) {
    co_await sem->wait();
    co_return 0;
}

/* Wakes the waiter. */
static co::task_t test45_signaler(co::sem_p sem) {
    sem->signal();
    co_return 0;
}

/* Does nothing that waits, and ends. */
static co::task_t test45_idle() {
    co_return 0;
}

int test45_killer_outlives_pool() {
    /* a task that never waited */
    auto pool = co::create_pool();
    auto killer = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test45_idle(), killer.first);
    ASSERT_FN(pool->run());
    pool = nullptr;                         /* the pool dies first */
    killer = {};                            /* then the killer: it must not touch the pool */

    /* a task that waited on a semaphore and was woken */
    auto pool2 = co::create_pool();
    auto sem = co::create_sem(pool2, 0);
    auto killer2 = co::create_killer(pool2.get(), co::ERROR_USER);
    pool2->sched(test45_waiter(sem), killer2.first);
    pool2->sched(test45_signaler(sem));
    ASSERT_FN(pool2->run());
    pool2 = nullptr;
    killer2 = {};
    sem = nullptr;

    return 0;
}

int main() {
    int ret = test45_killer_outlives_pool();
    print_test_result("018-014-reproduced_killer_outlives_pool.cpp", ret >= 0);
    return ret;
}
