#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test55 - Flow Control: a scheduled root that co_yields while a kill resumes it
================================================================================================= */

/* The target's semaphore wait already gave it its token, so the kill resumes it up to its next
wait. It co_yields instead: a killer's root that yields is a generator, which a killer must not
be attached to - its co_yield calls std::terminate(), inside the kill. Up to that
point nothing but the target ran inside the kill: the witness queued before the kill hasn't run.
Only a terminate from inside that kill() call, with the witness not run, passes. */

static const char *test55_file = "002-006-flowctrl_killer_driven_yield.cpp";
static bool test55_witness_ran = false;
static bool test55_in_kill = false;

static co::task_t test55_target(co::sem_p sem) {
    co_await sem->wait();
    co_yield 5;                 /* a scheduled root, no caller */
    co_return 0;
}

static co::task_t test55_witness() {
    test55_witness_ran = true;
    co_return 0;
}

static co::task_t test55_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    auto pool = co_await co::get_pool();
    co_await co::yield();       /* the target waits on sem */
    sem->signal();              /* it takes the token and is queued */
    pool->sched(test55_witness());
    test55_in_kill = true;
    kill_fn();                  /* must terminate */
    test55_in_kill = false;
    co_return 0;
}

int test55_driven_yield() {
    std::set_terminate([] {
        bool ok = test55_in_kill && !test55_witness_ran;
        print_test_result(test55_file, ok);
        std::_Exit(ok ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(co::add_modifs(pool.get(), test55_target(sem), mods));
    pool->sched(test55_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    DBG("the kill returned: it must have terminated");
    return -1;
}

int main() {
    int ret = test55_driven_yield();
    print_test_result(test55_file, ret >= 0);
    return ret;
}
