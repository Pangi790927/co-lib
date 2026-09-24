#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test63 - Flow Control: a killer attached to a generator terminates at its first co_yield
================================================================================================= */

/* A kill kills coroutines, it doesn't wake a generator up, so a killer can't be attached to one: the
co_yield of the killer's root calls std::terminate(), even if no kill ever comes. Only a terminate
from inside that co_yield passes; the program going on, or any other terminate, fails. */

static const char *test63_file = "002-008-flowctrl_killer_generator_yield.cpp";
static bool test63_in_yield = false;

static co::task_t test63_generator() {
    test63_in_yield = true;
    co_yield 1;                 /* must terminate */
    test63_in_yield = false;
    co_return 2;
}

static co::task_t test63_holder(co::task_t gen) {
    co_await gen;
    co_await gen;               /* not reached with the rule: finishes it, so nothing leaks */
    co_return 0;
}

int test63_generator_yield() {
    std::set_terminate([] {
        print_test_result(test63_file, test63_in_yield);
        std::_Exit(test63_in_yield ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test63_holder(co::add_modifs(pool.get(), test63_generator(), mods)));
    ASSERT_FN(pool->run());
    DBG("the co_yield went on: it must have terminated");
    return -1;
}

int main() {
    int ret = test63_generator_yield();
    print_test_result(test63_file, ret >= 0);
    return ret;
}
