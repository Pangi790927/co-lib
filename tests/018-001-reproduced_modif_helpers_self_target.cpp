#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test31 - Reproduced Bugs: no-arg add_modifs()/rm_modifs()/task_modifs() operate on the caller
================================================================================================= */

/* The no-argument overloads of add_modifs()/rm_modifs()/task_modifs() are themselves small helper
coroutines (`co_await co::add_modifs(mods)` calls into one). Their intent is to mutate the modif
table of whichever coroutine co_awaited them ("self"), not their own throwaway table. That requires
reading/writing `get_state()->caller_state->modif_table` - the caller's state - rather than the
helper's own `get_state()->modif_table`, which is destroyed with the helper the moment it returns.
An earlier version used the latter, silently making these calls no-ops. Fixed in colib.h by routing
through caller_state.

This test does NOT assert on the absolute call_count after add/rm - because CO_MODIF_INHERIT_ON_CALL
means the CALL_CBK also fires on the co_await to rm_modifs()/task_modifs() itself (those are calls
made by the caller too, same as any other), so the exact count depends on how many bookkeeping calls
happen to be inflight. Asserting on that absolute number is what caused earlier debugging sessions to
misdiagnose this as still-broken. What actually matters, and what's asserted here, is the delta: once
rm_modifs() has returned, no *further* call may trigger the callback, and task_modifs() must reflect
the real caller's table (not the helper's). */

int test31_call_count = 0;
size_t test31_after_rm_sz = 999;
int test31_count_after_first_inner = -1;
int test31_count_after_rm = -1;
int test31_count_after_second_inner = -1;

co::task_t test31_inner() {
    co_return 0;
}

co::task_t test31_outer() {
    auto pool = co_await co::get_pool();
    auto mod = co::create_modif<co::CO_MODIF_CALL_CBK>(co::CO_MODIF_INHERIT_ON_CALL,
        [](co::state_t*) -> co::error_e { test31_call_count++; return co::ERROR_OK; });

    /* add_modifs(mods) must land on *this* coroutine's table, not a helper's. Note: reading it
    back via task_modifs() right here is deliberately skipped - task_modifs() is itself a call
    made by this coroutine, so under CO_MODIF_INHERIT_ON_CALL it would also trigger the CALL_CBK
    being tested below and pollute the count. */
    co_await co::add_modifs(co::modif_pack_t{mod});

    co_await test31_inner();
    test31_count_after_first_inner = test31_call_count;

    /* rm_modifs(mods) must remove it from *this* coroutine's table */
    co_await co::rm_modifs(co::modif_pack_t{mod});
    test31_count_after_rm = test31_call_count;

    co_await test31_inner();
    test31_count_after_second_inner = test31_call_count;

    /* Checked here (not via task_modifs(), which would itself be an inherited call) - table
    should now be empty. get_state() is exempt from that same concern: it isn't a co_await call
    at all, just a plain function reading this coroutine's own state. */
    test31_after_rm_sz = co::get_modif_table_sz((co_await co::get_state())->modif_table);

    co_return 0;
}

int test31_modif_helpers_self_target() {
    auto pool = co::create_pool();
    pool->sched(test31_outer());
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test31_count_after_first_inner == 1));
    ASSERT_FN(CHK_BOOL(test31_count_after_second_inner == test31_count_after_rm));
    ASSERT_FN(CHK_BOOL(test31_after_rm_sz == 0));

    return 0;
}

int main() {
    int ret = test31_modif_helpers_self_target();
    print_test_result("018-001-reproduced_modif_helpers_self_target.cpp", ret >= 0);
    return ret;
}
