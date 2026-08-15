#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test40 - Reproduced Bugs: sem_t::unlocker_t signals even when the wait it came from was aborted
================================================================================================= */

/* Was an open bug (sem_awaiter_t::await_resume() carried its own TODO: "unlocker shouldn't signal
when suspend failed, I should fix it with a false sem or smthg") - now fixed: await_resume() tracks
an await_state_e (AWAITER_NOT_CALLED / AWAITER_SUSPEND_LAST / AWAITER_READY_LAST) instead of a single
bool, and returns a null-sem unlocker_t (unlock() is a safe no-op on it) specifically for the aborted
case. This file is the regression test for that fix; see also 018-009 for the companion fast-path
case that same fix had to get right too.

sem_awaiter_t::await_suspend() can abort a wait before it actually suspends: if a user-attached
CO_MODIF_WAIT_SEM_CBK modif returns a non-OK error, the waiter is removed again
(erase_waiter(*psem_it)) and the coroutine resumes immediately via `return to_suspend` - the
semaphore's internal counter is never touched (await_ready() already returned false, so no decrement
happened either). `triggered` stays false on this path.

But await_resume() unconditionally returns a live `unlocker_t(sem)` regardless of `triggered`. That's
correct for the *other* way `triggered` ends up false - the fast path, where await_ready() itself
returned true because the counter was already > 0 and got decremented right there; unlock()-ing that
one is a legitimate, balanced release. It's wrong for the aborted-wait path: nothing was ever
decremented, so calling .unlock() (e.g. via the RAII/lock_guard pattern the type exists for) on the
unlocker_t returned from an aborted wait calls sem->signal() with nothing to balance it against -
the counter goes up by one for an acquire that never happened. */

co::task_t test40_aborted_waiter(co::sem_p sem) {
    /* wait() gets aborted by the WAIT_SEM_CBK modif below before it actually suspends - the counter
    is never touched. Calling unlock() on the returned unlocker_t anyway (RAII style) still signals. */
    auto unlocker = co_await sem->wait();
    unlocker.unlock();
    co_return 0;
}

int test40_unlocker_spurious_signal() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    auto abort_wait = co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t*, co::sem_t*, co::sem_waiter_handle_p) -> co::error_e {
            return co::ERROR_GENERIC; /* reject every wait attempt on this coroutine */
        });

    auto victim = co::add_modifs(pool.get(), test40_aborted_waiter(sem), co::modif_pack_t{abort_wait});
    pool->sched(victim);

    ASSERT_FN(pool->run());

    /* nothing was ever legitimately acquired, so the counter must still be at 0 - try_dec() must
    fail. If the spurious signal() went through, the counter is at 1 and this wrongly succeeds. */
    ASSERT_FN(CHK_BOOL(sem->try_dec() == false));

    return 0;
}

int main() {
    int ret = test40_unlocker_spurious_signal();
    print_test_result("018-008-reproduced_unlocker_spurious_signal.cpp", ret >= 0);
    return ret;
}
