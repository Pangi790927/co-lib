#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <exception>

/* Test62 - Reproduced bug: a generator killed after a co_yield handed its caller the old value
================================================================================================= */

/* Was BUGS.md #11: a killer attached to a generator, killed in the run after its co_yield while it
waited - the caller's await returned the value of the previous co_yield (7) as if it had yielded
again. Now a killer's root can't get past its first co_yield: it calls std::terminate(), so the
killed run and the stale value can't happen. Only a terminate from inside that co_yield passes;
the caller getting any second value fails. */

static const char *test62_file = "018-021-reproduced_killed_generator_stale_value.cpp";
static bool test62_in_first_yield = false;

static co::task_t test62_generator(co::sem_p sem) {
    test62_in_first_yield = true;
    co_yield 7;                 /* must terminate */
    test62_in_first_yield = false;
    co_await sem->wait();       /* the bug: killed here, in its second run */
    co_yield 8;
    co_return 9;
}

static co::task_t test62_caller(co::task_t gen) {
    co_await gen;
    int second = co_await gen;  /* the bug: 7 again */
    DBG("second: %d", second);
    co_return 0;
}

static co::task_t test62_killer(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();
    kill_fn();
    co_return 0;
}

int test62_killed_generator() {
    std::set_terminate([] {
        print_test_result(test62_file, test62_in_first_yield);
        std::_Exit(test62_in_first_yield ? 0 : 1);
    });

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto gen = co::add_modifs(pool.get(), test62_generator(sem), mods);
    pool->sched(test62_caller(gen));
    pool->sched(test62_killer(kill_fn));
    ASSERT_FN(pool->run());
    DBG("no terminate: the first co_yield must have terminated");
    return -1;
}

int main() {
    int ret = test62_killed_generator();
    print_test_result(test62_file, ret >= 0);
    return ret;
}
