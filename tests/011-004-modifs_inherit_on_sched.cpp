#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test36 - Modifs: CO_MODIF_INHERIT_ON_SCHED
================================================================================================= */

/* 11-1-modifs.cpp and 11-3-modifs_lifecycle.cpp both use CO_MODIF_INHERIT_ON_CALL exclusively.
This exercises the other inheritance path: a SCHED_CBK modif attached to a coroutine with
CO_MODIF_INHERIT_ON_SCHED must propagate to any coroutine that coroutine schedules via co::sched(),
the same way ON_CALL propagates across co_await. */

int test36_sched_cnt = 0;

co::task_t test36_inner() {
    co_return 0;
}

co::task_t test36_outer() {
    auto pool = co_await co::get_pool();
    auto mod = co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_ON_SCHED,
        [](co::state_t*) -> co::error_e { test36_sched_cnt++; return co::ERROR_OK; });

    co_await co::add_modifs(co::modif_pack_t{mod});
    co_await co::sched(test36_inner());

    co_return 0;
}

int test36_modifs_inherit_on_sched() {
    auto pool = co::create_pool();
    pool->sched(test36_outer());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test36_sched_cnt == 1));
    return 0;
}

int main() {
    int ret = test36_modifs_inherit_on_sched();
    print_test_result("011-004-modifs_inherit_on_sched.cpp", ret >= 0);
    return ret;
}
