#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test66 - Reproduced bug: a kill after the holder freed its yielded generator terminated
================================================================================================= */

/* Was BUGS.md #12: with a killer attached to a generator, the holder freed the yielded generator
(destroy_state()) and a later kill still terminated, on a generator that no longer existed. Now
the terminate comes at the root's first co_yield itself, before the holder can free it or kill it
- that's the only terminate that passes; the holder getting to free it fails. */

static const char *test66_file = "018-022-reproduced_killer_freed_generator_terminates.cpp";
static bool test66_in_yield = false;

static co::task_t test66_generator() {
    test66_in_yield = true;
    co_yield 1;                 /* must terminate */
    test66_in_yield = false;
    co_return 2;
}

static co::task_t test66_holder(co::task_t gen, std::function<co::error_e(void)> kill_fn) {
    co_await gen;
    co::destroy_state(gen.get_state());  /* the bug's path: the holder frees it... */
    kill_fn();                           /* ...and the kill terminated anyway */
    co_return 0;
}

int test66_freed_generator() {
    std::set_terminate([] {
        print_test_result(test66_file, test66_in_yield);
        std::_Exit(test66_in_yield ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto gen = co::add_modifs(pool.get(), test66_generator(), mods);
    pool->sched(test66_holder(gen, kill_fn));
    ASSERT_FN(pool->run());
    DBG("no terminate: the co_yield must have terminated");
    return -1;
}

int main() {
    int ret = test66_freed_generator();
    print_test_result(test66_file, ret >= 0);
    return ret;
}
