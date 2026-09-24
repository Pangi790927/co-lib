#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test69 - Flow Control: a killed generator never gets a later run
================================================================================================= */

/* A kill kills coroutines, it doesn't wake a generator up: a generator with a killer never gets to
a second run where a kill could reach it, because its root's first co_yield already calls
std::terminate(). Here the plan was to kill it in its second run, while it waits; only a
terminate from inside that first co_yield passes - the second run starting, or the kill
happening, fails. */

static const char *test69_file = "002-011-flowctrl_killer_generator_later_run.cpp";
static bool test69_in_first_yield = false;

static co::task_t test69_generator(co::sem_p sem) {
    test69_in_first_yield = true;
    co_yield 7;                 /* must terminate */
    test69_in_first_yield = false;
    co_await sem->wait();       /* the second run would wait here, to be killed */
    co_yield 8;
    co_return 9;
}

static co::task_t test69_caller(co::task_t gen) {
    co_await gen;
    co_await gen;
    co_return 0;
}

static co::task_t test69_killer(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the generator would wait on sem, in its second run */
    kill_fn();
    co_return 0;
}

int test69_later_run() {
    std::set_terminate([] {
        print_test_result(test69_file, test69_in_first_yield);
        std::_Exit(test69_in_first_yield ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto gen = co::add_modifs(pool.get(), test69_generator(sem), mods);
    pool->sched(test69_caller(gen));
    pool->sched(test69_killer(kill_fn));
    ASSERT_FN(pool->run());
    DBG("no terminate: the first co_yield must have terminated");
    return -1;
}

int main() {
    int ret = test69_later_run();
    print_test_result(test69_file, ret >= 0);
    return ret;
}
