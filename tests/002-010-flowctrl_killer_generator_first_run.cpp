#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test65 - Flow Control: a generator killed before its first co_yield dies like any coroutine
================================================================================================= */

/* The killer learns its root is a generator only from a co_yield. Killed before its first one, a
generator can't be told apart from any other coroutine, and it is killed like one: no terminate,
the code after its wait never runs, its caller gets T{} and frees it, and the kill function then
finds nothing left. */

static bool test65_after_wait = false;
static int test65_got = -1;
static co::error_e test65_kill_ret = co::ERROR_GENERIC;
static bool test65_ok = false;

static co::task_t test65_generator(co::sem_p sem) {
    co_await sem->wait();       /* killed here */
    test65_after_wait = true;
    co_yield 5;
    co_return 0;
}

static co::task_t test65_holder(co::task_t gen) {
    test65_got = co_await gen;
    co_return 0;
}

static co::task_t test65_controller(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the generator waits on sem */
    test65_kill_ret = kill_fn();
    test65_ok = true;
    co_return 0;
}

int test65_generator_first_run() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test65_holder(co::add_modifs(pool.get(), test65_generator(sem), mods)));
    pool->sched(test65_controller(kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test65_ok));
    ASSERT_FN(CHK_BOOL(test65_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(!test65_after_wait));
    ASSERT_FN(CHK_BOOL(test65_got == 0));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test65_generator_first_run();
    print_test_result("002-010-flowctrl_killer_generator_first_run.cpp", ret >= 0);
    return ret;
}
