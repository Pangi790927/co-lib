#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test33 - Reproduced Bugs: double ENTER on the caller when a CALL modif vetoes the call
================================================================================================= */

/* task<T>::await_suspend() (colib.h ~2662-2684), on a failed CO_MODIF_CALL_CBK, does:
       do_entry_modifs(&caller.promise().state);
       return caller;
   ... which resumes the caller directly. But since the caller's own co_await machinery then runs
   task<T>::await_resume() (colib.h ~2686-2722) unconditionally, and that ALSO does:
       do_entry_modifs(h.promise().state.caller_state);
   the caller ends up ENTER-ed twice in a row with no LEAVE in between - only one LEAVE (from
   do_leave_modifs at the top of await_suspend) precedes both. With COLIB_ENABLE_DEBUG_CHECKS on,
   dbg_check_modif_enter's "entered twice" assertion (colib.h ~6423) should trip and abort() on the
   second call.

   018-002 covers the *return-value* side of a failed CALL modif (std::bad_variant_access) but
   doesn't enable COLIB_ENABLE_DEBUG_CHECKS, so it doesn't exercise this. */

int test33_call_cnt = 0;

co::task<int> test33_callee() {
    co_return 123;
}

co::task_t test33_caller() {
    auto pool = co_await co::get_pool();
    auto fail_call = co::create_modif<co::CO_MODIF_CALL_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e { test33_call_cnt++; return co::ERROR_GENERIC; });

    auto t = test33_callee();
    co::add_modifs(pool, t, co::modif_pack_t{fail_call});

    /* expected to survive without tripping "entered twice" (or any other) debug-check abort() */
    co_await t;
    co_return 0;
}

int test33_call_modif_failure_double_enter() {
    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test33_caller()));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test33_call_cnt == 1));
    return 0;
}

int main() {
    int ret = test33_call_modif_failure_double_enter();
    print_test_result("018-011-reproduced_call_modif_failure_double_enter.cpp", ret >= 0);
    return ret;
}
