#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test68 - Flow Control: a generator with a killer is never left idle for a kill to find
================================================================================================= */

/* A yielded generator is alive and waiting on nothing: a kill can't end it without freeing a frame
its holder still owns. So there must never be such a thing for a kill to find: the root's first
co_yield already calls std::terminate(). Here the holder meant to kill it right after it yielded;
only a terminate from inside that co_yield passes - the kill being reached fails. */

static const char *test68_file = "002-012-flowctrl_killer_generator_silent.cpp";
static bool test68_in_yield = false;

static co::task_t test68_generator() {
    test68_in_yield = true;
    co_yield 1;                 /* must terminate */
    test68_in_yield = false;
    co_return 2;
}

static co::task_t test68_holder(co::task_t gen, std::function<co::error_e(void)> kill_fn) {
    co_await gen;               /* it yields: idle, alive */
    kill_fn();                  /* never reached with the rule */
    co_await gen;
    co_return 0;
}

int test68_generator_silent() {
    std::set_terminate([] {
        print_test_result(test68_file, test68_in_yield);
        std::_Exit(test68_in_yield ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto gen = co::add_modifs(pool.get(), test68_generator(), mods);
    pool->sched(test68_holder(gen, kill_fn));
    ASSERT_FN(pool->run());
    DBG("no terminate: the co_yield must have terminated");
    return -1;
}

int main() {
    int ret = test68_generator_silent();
    print_test_result(test68_file, ret >= 0);
    return ret;
}
