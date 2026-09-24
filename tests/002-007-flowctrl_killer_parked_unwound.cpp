#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test56 - Flow Control: a chain parked by the kill that resumed it, with a second killer on it
================================================================================================= */

/* The target carries two killers. The second one's kill resumes it (its semaphore wait already had
its token); while resumed, the target tries to kill itself through the first killer, which throws
kill_self_t (caught) and kills nothing. At its next wait only the second killer parks it (the first
one isn't resuming it), and the second kill destroys the chain once its resume returned: one
destruction, the code after the wait never runs, and the first killer finds nothing left to kill. */

static std::function<co::error_e(void)> test56_kill_first;
static int test56_destructed = 0;
static int test56_after_wait = 0;
static bool test56_refused = false;
static bool test56_ok = false;

struct test56_marker_t {
    ~test56_marker_t() { test56_destructed++; }
};

static co::task_t test56_target(co::sem_p sem, co::sem_p never) {
    test56_marker_t marker;
    co_await sem->wait();               /* the token is given: the second kill resumes us */
    try {
        test56_kill_first();            /* we are executing: refused */
    }
    catch (co::kill_self_t &) {
        test56_refused = true;
    }
    co_await never->wait();             /* the second killer parks us here */
    test56_after_wait++;
    co_return 0;
}

static co::task_t test56_controller(co::sem_p sem, std::function<co::error_e(void)> kill_second) {
    co_await co::yield();               /* the target waits on sem */
    sem->signal();
    co::error_e ret = kill_second();    /* resumes it, it parks, the chain is destroyed */
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
    ASSERT_COFN(CHK_BOOL(test56_refused));
    ASSERT_COFN(CHK_BOOL(test56_destructed == 1));
    ASSERT_COFN(CHK_BOOL(test56_kill_first() == co::ERROR_FINISHED));
    test56_ok = true;
    co_return 0;
}

int test56_parked_unwound() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [first_mods, kill_first] = co::create_killer(pool.get(), co::ERROR_USER);
    auto [second_mods, kill_second] = co::create_killer(pool.get(), co::ERROR_USER);
    test56_kill_first = kill_first;

    auto target = test56_target(sem, never);
    co::add_modifs(pool.get(), target, first_mods);
    co::add_modifs(pool.get(), target, second_mods);
    pool->sched(target);
    pool->sched(test56_controller(sem, kill_second));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test56_ok));
    ASSERT_FN(CHK_BOOL(test56_after_wait == 0));
    ASSERT_FN(CHK_BOOL(test56_destructed == 1));
    return 0;
}

int main() {
    int ret = test56_parked_unwound();
    print_test_result("002-007-flowctrl_killer_parked_unwound.cpp", ret >= 0);
    return ret;
}
