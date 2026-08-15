#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test37 - Modifs: task_modifs()/add_modifs()/rm_modifs() explicit-target overloads
================================================================================================= */

/* Covers the explicit-target overloads - task_modifs(t), add_modifs(pool, t, mods),
rm_modifs(t, mods) - which operate directly on a task<T> handle the caller already holds, as
opposed to the no-argument self-target overloads covered by
18-1-reproduced_modif_helpers_self_target.cpp. Exercises: reading back a task's attached modifs,
adding a modif mid-flight (before it's ever run), and removing one again before the task runs -
none of the three should ever run since the callback is removed before the task is scheduled. */

int test37_enter_cnt = 0;

co::task_t test37_child() {
    co_return 0;
}

int test37_modifs_standalone_explicit() {
    auto pool = co::create_pool();
    auto mod = co::create_modif<co::CO_MODIF_ENTER_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e { test37_enter_cnt++; return co::ERROR_OK; });

    auto t = test37_child();
    ASSERT_FN(CHK_BOOL(co::task_modifs(t).size() == 0)); /* nothing attached yet */

    t = co::add_modifs(pool.get(), t, co::modif_pack_t{mod});
    ASSERT_FN(CHK_BOOL(co::task_modifs(t).size() == 1)); /* readable back before the task ever runs */

    t = co::rm_modifs(t, co::modif_pack_t{mod});
    ASSERT_FN(CHK_BOOL(co::task_modifs(t).size() == 0)); /* removed again before it ever runs */

    pool->sched(t);
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test37_enter_cnt == 0)); /* removed mid-flight: ENTER_CBK must not fire */

    return 0;
}

int main() {
    int ret = test37_modifs_standalone_explicit();
    print_test_result("011-005-modifs_standalone_explicit.cpp", ret >= 0);
    return ret;
}
