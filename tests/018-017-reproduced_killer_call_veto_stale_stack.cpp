#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test53 - Reproduced Bugs: a vetoed call leaves a dangling frame on a killer's call stack
================================================================================================= */

/* A task carries a killer and a second modif whose CALL callback vetoes the call. The killer's
CALL callback runs first and pushes the callee on its call stack; the veto then makes
task<T>::await_suspend() resume the caller, and task<T>::await_resume() destroys the callee
without running its EXIT callbacks. The killer never pops it: its call stack keeps the dead
frame, and a later kill reads it (top()->pool). Crashes (access violation / SIGSEGV) today, on
the current killer and on the redesigned one alike. After the fix the kill must find nothing to
kill. Numbered 018-017 because 018-015 and 018-016 were already taken by the killer redesign's own
reproduced bugs (docs/old_drafts/). 2026-09-24 */

static bool test53_child_ran = false;
static bool test53_killed = false;
static co::error_e test53_kill_ret = co::ERROR_OK;

static co::task<int> test53_child() {
    test53_child_ran = true;
    co_return 5;
}

static co::task_t test53_parent(co::modif_pack_t kill_mods) {
    auto pool = co_await co::get_pool();
    auto child = test53_child();
    co::add_modifs(pool, child, kill_mods);     /* the killer first: its CALL cbk runs first */
    co::add_modifs(pool, child, co::modif_pack_t{co::create_modif<co::CO_MODIF_CALL_CBK>(
            co::CO_MODIF_INHERIT_NONE, [](co::state_t *) -> co::error_e { return co::ERROR_USER; })});
    co_await child;                             /* vetoed: the child never runs, and is destroyed */
    co_return 0;
}

static co::task_t test53_killer_side(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();                       /* the parent's vetoed call is over */
    test53_kill_ret = kill_fn();                /* nothing is left to kill */
    test53_killed = true;
    co_return 0;
}

int test53_call_veto_stale_stack() {
    auto pool = co::create_pool();
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test53_parent(mods));
    pool->sched(test53_killer_side(kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(!test53_child_ran));
    ASSERT_FN(CHK_BOOL(test53_killed));
    ASSERT_FN(CHK_BOOL(test53_kill_ret != co::ERROR_OK));  /* "nothing to kill", not a kill */
    return 0;
}

int main() {
    int ret = test53_call_veto_stale_stack();
    print_test_result("018-017-reproduced_killer_call_veto_stale_stack.cpp", ret >= 0);
    return ret;
}
