#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test38 - Reproduced Bugs: scheduling (instead of co_await-ing) a no-arg modif helper crashes
================================================================================================= */

/* OPEN BUG - see BUGS.md #2. Not fixed yet: this test is expected to fail (crash) until it is.
Once fixed, this file stays as-is (assertions updated to match the actual fix if needed) so the
bug can't silently come back.

add_modifs(mods)/rm_modifs(mods)/task_modifs() (the no-arg, self-target overloads - see
18-1-reproduced_modif_helpers_self_target.cpp) read/write
`(co_await get_state())->caller_state->modif_table`. `caller_state` is only ever set inside
task<T>::await_suspend() (colib.h ~2655), i.e. only when the helper coroutine is directly
co_await-ed by another coroutine - which is the only documented way to call these (colib.h's own
doc comment: "@return **Coroutine** that resolvs to: the adding of the modifiers", i.e. must be
awaited). Nothing stops a caller from scheduling one like any other task_t instead, e.g.
`pool->sched(co::add_modifs(mods))`. That leaves `caller_state == nullptr`, and the dereference is
an immediate access violation - confirmed by this test, which currently crashes the whole process
(exit code 0xC0000005) rather than failing an assertion.

This is intentionally not a subprocess-isolated crash test: keeping it inline means `make all` stops
dead here until the bug is fixed, same as any other reproduced-bug test - that's the point. */

int test38_sched_no_arg_modif_helper() {
    auto pool = co::create_pool();
    auto mod = co::create_modif<co::CO_MODIF_CALL_CBK>(pool.get(), co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*) -> co::error_e { return co::ERROR_OK; });

    /* misuse: scheduling add_modifs() directly instead of co_await-ing it from a coroutine -
    caller_state is never set, so the helper's body dereferences a null pointer */
    pool->sched(co::add_modifs(co::modif_pack_t{mod}));
    ASSERT_FN(pool->run()); /* must not crash - however the eventual fix chooses to handle misuse */

    return 0;
}

int main() {
    int ret = test38_sched_no_arg_modif_helper();
    print_test_result("18-6-reproduced_sched_no_arg_modif_helper.cpp", ret >= 0);
    return ret;
}
