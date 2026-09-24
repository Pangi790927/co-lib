#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test64 - Flow Control: a killed generator that yields while the kill resumes it terminates
================================================================================================= */

/* The killer is attached to a generator waiting on a semaphore whose token it already got, so the
kill resumes it up to its next wait. It co_yields instead: the killer's root is a generator, and
attaching a killer to one is not allowed, so its co_yield calls std::terminate() - here from
inside the kill that resumed it. Only a terminate from inside that kill() call passes. */

static const char *test64_file = "002-009-flowctrl_killer_generator_driven.cpp";
static bool test64_in_kill = false;

static co::task_t test64_generator(co::sem_p sem) {
    co_await sem->wait();       /* the token is given: the kill resumes us */
    co_yield 5;                 /* slips out of the kill */
    co_return 0;
}

static co::task_t test64_holder(co::task_t gen) {
    co_await gen;
    co_await gen;               /* not reached with the fix: finishes it, so nothing leaks */
    co_return 0;
}

static co::task_t test64_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the generator waits on sem */
    sem->signal();
    test64_in_kill = true;
    kill_fn();                  /* must terminate */
    test64_in_kill = false;
    co_return 0;
}

int test64_generator_driven() {
    std::set_terminate([] {
        print_test_result(test64_file, test64_in_kill);
        std::_Exit(test64_in_kill ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test64_holder(co::add_modifs(pool.get(), test64_generator(sem), mods)));
    pool->sched(test64_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    DBG("the kill returned: it must have terminated");
    return -1;
}

int main() {
    int ret = test64_generator_driven();
    print_test_result(test64_file, ret >= 0);
    return ret;
}
