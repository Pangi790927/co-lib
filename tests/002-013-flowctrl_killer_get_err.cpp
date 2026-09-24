#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test73 - Flow Control: a killed callee's error, through task<T>::get_err()
================================================================================================= */

/* The kill sets its error (create_killer's e) inside the killed coroutine. Its caller's await then
returns the default value, and the task that was awaited keeps the error: get_err() returns it. A
callee that returned normally leaves ERROR_OK there. */

static int test73_killed_ret = -1;
static co::error_e test73_killed_err = co::ERROR_OK;
static int test73_normal_ret = -1;
static co::error_e test73_normal_err = co::ERROR_GENERIC;
static bool test73_ok = false;

static co::task_t test73_victim(co::sem_p sem) {
    co_await sem->wait();       /* killed here */
    co_return 5;
}

static co::task_t test73_normal() {
    co_return 7;
}

static co::task_t test73_caller(co::sem_p sem, co::modif_pack_t mods) {
    auto pool = co_await co::get_pool();
    auto victim = co::add_modifs(pool, test73_victim(sem), mods);
    test73_killed_ret = co_await victim;
    test73_killed_err = victim.get_err();

    auto normal = test73_normal();
    test73_normal_ret = co_await normal;
    test73_normal_err = normal.get_err();
    test73_ok = true;
    co_return 0;
}

static co::task_t test73_killer(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the victim waits on sem */
    kill_fn();
    co_return 0;
}

int test73_get_err() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test73_caller(sem, mods));
    pool->sched(test73_killer(kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test73_ok));
    ASSERT_FN(CHK_BOOL(test73_killed_ret == 0));
    ASSERT_FN(CHK_BOOL(test73_killed_err == co::ERROR_USER));
    ASSERT_FN(CHK_BOOL(test73_normal_ret == 7));
    ASSERT_FN(CHK_BOOL(test73_normal_err == co::ERROR_OK));
    return 0;
}

int main() {
    int ret = test73_get_err();
    print_test_result("002-013-flowctrl_killer_get_err.cpp", ret >= 0);
    return ret;
}
