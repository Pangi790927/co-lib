#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test48 - Flow Control: a killed chain that finishes before it can die
================================================================================================= */

/* The target's wait completed (a semaphore gave it its token), and after it the chain never waits
again: the killer resumes it inside the kill, the chain returns all the way up to its root and the
kill reports ERROR_FINISHED - it wasn't killed, it completed in time, and its results exist. */

static int test48_out = 0;
static bool test48_ok = false;

static co::task<int> test48_inner(co::sem_p sem) {
    co_await sem->wait();
    co_return 7;
}

static co::task_t test48_outer(co::sem_p sem) {
    test48_out = co_await test48_inner(sem);    /* a 2-frame chain: inner returns into outer */
    co_return 0;
}

static co::task_t test48_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* outer/inner run, inner waits on `sem` */
    sem->signal();
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_FINISHED));
    ASSERT_COFN(CHK_BOOL(test48_out == 7));     /* it ran to the end, inside the kill */
    ASSERT_COFN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    test48_ok = true;
    co_return 0;
}

int test48_killer_finished() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test48_outer(sem), mods));
    pool->sched(test48_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test48_ok));
    return 0;
}

int main() {
    int ret = test48_killer_finished();
    print_test_result("002-003-flowctrl_killer_finished.cpp", ret >= 0);
    return ret;
}
