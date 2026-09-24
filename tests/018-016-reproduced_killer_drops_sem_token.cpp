#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test47 - Reproduced Bugs: a killer destroys a coroutine a semaphore already gave its token to
================================================================================================= */

/* sem_t::signal() takes the count down and queues the waiter. A kill that runs before that waiter
is resumed finds it in the ready queue and destroys it: the token was consumed and nobody got it.
The same flaw as 018-015, on semaphores and on every platform. Fixed, the killer resumes the waiter
up to its next wait (the code after the wait gets the token) and destroys it there. 2026-09-23 */

static int test47_got_token = 0;
static int test47_after_next_wait = 0;
static int test47_destructed = 0;
static bool test47_ok = false;

struct test47_marker_t {
    ~test47_marker_t() { test47_destructed++; }
};

static co::task_t test47_victim(co::sem_p sem, co::sem_p never) {
    test47_marker_t marker;
    co_await sem->wait();
    test47_got_token++;             /* must run: the wait completed, it holds the token */
    co_await never->wait();         /* dies here, without waiting */
    test47_after_next_wait++;       /* must not run */
    co_return 0;
}

static co::task_t test47_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();           /* the victim runs and waits on `sem` */
    sem->signal();                  /* the victim takes the token and is queued, not resumed yet */
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
    ASSERT_COFN(CHK_BOOL(test47_got_token == 1));   /* before the fix: 0, the token was lost */
    ASSERT_COFN(CHK_BOOL(test47_after_next_wait == 0));
    ASSERT_COFN(CHK_BOOL(test47_destructed == 1));
    ASSERT_COFN(CHK_BOOL(sem->try_dec() == false)); /* the token wasn't handed out twice either */
    test47_ok = true;
    co_return 0;
}

int test47_killer_drops_sem_token() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test47_victim(sem, never), mods));
    pool->sched(test47_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test47_ok));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test47_killer_drops_sem_token();
    print_test_result("018-016-reproduced_killer_drops_sem_token.cpp", ret >= 0);
    return ret;
}
