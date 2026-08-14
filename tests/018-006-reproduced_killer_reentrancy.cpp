#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test38 - Reproduced Bugs: calling a killer's kill_fn() reentrantly (from within a kill it caused)
================================================================================================= */

/* OPEN BUG - see BUGS.md. colib.h ~5855, create_killer()'s own TODO: "calling killer from killer (as
a result of killing a coro) is not ok". Not fixed yet: this test is expected to fail (crash) until it
is. Once fixed, this file stays as-is (assertions/expectations updated to match the actual fix if
needed) so the bug can't silently come back.

create_killer()'s kill_fn() (colib.h's sig_kill lambda) unwinds kstate->call_stack top-to-bottom,
calling do_exit_modifs(state) then state->self.destroy() for each frame. state->self.destroy() runs
synchronously and tears down that coroutine frame, which runs the destructors of any local objects
still alive in it - so if one of those destructors calls kill_fn() again (the same killer, as a side
effect of the very kill that's tearing its frame down), the reentrant call runs on the SAME
kstate->call_stack while the outer call is still mid-unwind:

  - By the time the reentrant call runs, the outer call's own bookkeeping EXIT_CBK has already popped
    the frame currently being destroyed off call_stack. The reentrant call sees whatever's left and
    unwinds the rest of the stack itself - including the caller frame(s) the outer call was about to
    get to on its own - right down to empty.
  - Once the reentrant call returns, the outer call finishes destroying its own current frame, then
    loops back to check `call_stack.size() > 1` - false, since the reentrant call already emptied it -
    and falls through to `kstate->call_stack.top()` on an empty stack: undefined behavior.

This reproduces the exact "stale state_t pointer / double-destroy" failure signature this project's own notes
describe for an unrelated, still-open heap-corruption investigation (create_timeo's exec_coro/
timer_coro mutually killing each other via create_killer was the leading suspect there) - this test
isolates the reentrancy mechanism on its own, without create_timeo in the picture.

block_forever/kill_fn are threaded through as parameters (not globals) so nothing here can outlive
the pool_p it was created from - a global sem_p/std::function surviving past its local pool's
destruction caused a second, unrelated dangling-pointer crash during static teardown the first time
this test was written, which had nothing to do with the bug being reproduced here. */

struct test38_reentrant_kill_t {
    std::function<co::error_e(void)> kill_fn;

    ~test38_reentrant_kill_t() {
        /* Fires while test38_inner's coroutine frame is being torn down by the kill_fn() call in
        test38_controller() below - i.e. reentrantly, from inside that same call's own unwind. */
        kill_fn();
    }
};

co::task_t test38_inner(co::sem_p block_forever, std::function<co::error_e(void)> kill_fn) {
    test38_reentrant_kill_t guard{kill_fn};
    co_await block_forever->wait(); /* never signaled: only the killer ends this */
    co_return 0;
}

co::task_t test38_outer(co::sem_p block_forever, std::function<co::error_e(void)> kill_fn) {
    co_await test38_inner(block_forever, kill_fn); /* 2-level call chain, so the killer's call_stack
                                                        has >1 entry to unwind */
    co_return 0;
}

co::task_t test38_controller(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield(); /* let test38_outer/test38_inner run first and park on the semaphore */
    auto ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
    co_return 0;
}

int test38_killer_reentrancy() {
    auto pool = co::create_pool();
    auto block_forever = co::create_sem(pool, 0);

    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto victim = co::add_modifs(pool.get(), test38_outer(block_forever, kill_fn), mods);
    pool->sched(victim);
    pool->sched(test38_controller(kill_fn));

    ASSERT_FN(pool->run()); /* must not crash */

    return 0;
}

int main() {
    int ret = test38_killer_reentrancy();
    print_test_result("018-006-reproduced_killer_reentrancy.cpp", ret >= 0);
    return ret;
}
