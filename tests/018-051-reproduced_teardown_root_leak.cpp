#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test107 - Reproduced Bugs: pool teardown leaked a frame a kill had left to its queued caller
================================================================================================= */

/* A kill of a called root ends it the documented way: the root is exited but its frame is kept,
since its caller frees it at the resume of its await (reading the kill's error there), and the
caller is queued. A driven root that returns by itself leaves its frame the same way: its RETURN
callback keeps the final awaiter from resuming the caller, and the caller is queued. If the pool is
destroyed before that caller ever resumes, clear()'s ready drain destroys the queued caller without
resuming it; the kept frame was in no queue and reachable only from the caller's frame, so it was
never freed: its locals' destructors never ran, ~state_t() never ran.

Fixed in the engine (fix #26, tests/fixes/FIX_26-teardown_root_leak.md, in the shape the user
chose): a call is recorded in the caller's wait slot, as a wait on the callee, from the call's
suspend to its resume, so a frame left to a caller is named by that caller's call; destroy_state()
frees the frame a dying caller still names, with its exit already run. The killer and the pool
don't change. The control phases pin the resume path: a run() after the kill resumes the caller,
which frees the frame and reads the kill's error (or the driven root's return value). The teardown
phases pin the fix: the caller dies in the ready drain never resumed, the kept frame dies exactly
once with it, and the root's EXIT and UNWAIT_SEM callbacks ran exactly once each, at the kill,
never again at the teardown. Reproduced 2026-09-28 (repro/26-teardown_root_leak.md). */

/* observation state, written by the coroutines and their callbacks, asserted by main */
static int test107_callee_dtor    = 0;   /* the root's frame destroyed: exactly once per phase  */
static int test107_caller_dtor    = 0;   /* the caller's frame destroyed: exactly once per phase */
static int test107_callee_exit    = 0;   /* the root's EXIT modif: exactly once                  */
static int test107_callee_unwait  = 0;   /* the root's UNWAIT_SEM modif: exactly once            */
static int test107_callee_past    = 0;   /* the root ran past its wait (the driven root does)    */
static int test107_caller_resumed = 0;   /* caller code after its co_await ran                   */
static int test107_caller_err     = -999;/* the error the caller's co_await handed back          */
static int test107_caller_ret     = -999;/* the value the caller's co_await handed back          */

struct test107_callee_marker_t { ~test107_callee_marker_t() { test107_callee_dtor++; } };
struct test107_caller_marker_t { ~test107_caller_marker_t() { test107_caller_dtor++; } };

/* the root: parks on a semaphore; killed there it never runs past it, driven past it (the
semaphore signaled first) it returns by itself */
static co::task_t test107_callee(co::sem_p sem) {
    test107_callee_marker_t m;
    co_await sem->wait();
    test107_callee_past++;
    co_return 42;
}

/* the caller: co_awaits the root, so the kill (or the driven return) queues it while it hangs
suspended at that co_await */
static co::task_t test107_caller(co::pool_t *pool, co::sem_p sem, co::modif_pack_t mods) {
    test107_caller_marker_t m;
    auto callee_task = co::add_modifs(pool, test107_callee(sem), mods);
    int ret = co_await callee_task;
    test107_caller_resumed++;
    test107_caller_err = (int)callee_task.get_err();
    test107_caller_ret = ret;
    co_return 0;
}

/* one phase: its own pool, run to the parked state, then the kill: at the park (`drive` false)
or after a signal woke the root, so the kill drives it to its return (`drive` true); then either
run again (the caller resumes and frees the kept frame) or destroy the pool with the caller
still queued */
static int test107_phase(bool drive, bool run_after_kill) {
    test107_callee_dtor = test107_caller_dtor = 0;
    test107_callee_exit = test107_callee_unwait = test107_callee_past = 0;
    test107_caller_resumed = 0;
    test107_caller_err = test107_caller_ret = -999;

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* extra eyes through the public modif API: UNWAIT_SEM shows the root leaving the sem queue,
    EXIT shows the exit that is not the frame's death */
    mods.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *, co::sem_t *) -> co::error_e {
                test107_callee_unwait++;
                return co::ERROR_OK;
            }));
    mods.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test107_callee_exit++;
                return co::ERROR_OK;
            }));

    pool->sched(test107_caller(pool.get(), sem, mods));
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));   /* both parked, pool idle */
    if (drive) {
        sem->signal();                             /* woken, not resumed: its wait did its work */
        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));   /* driven to its return */
        ASSERT_FN(CHK_BOOL(test107_callee_past == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_OK));         /* ended at its park */
        ASSERT_FN(CHK_BOOL(test107_callee_past == 0));
    }

    if (run_after_kill)
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));

    pool.reset();   /* ~pool_internal_t -> clear(): the leak window, the caller still queued */
    return 0;
}

static int test107_teardown_root_leak() {
    for (int drive = 0; drive < 2; drive++) {
        int want_err = drive ? (int)co::ERROR_OK : (int)co::ERROR_USER;
        int want_ret = drive ? 42 : 0;

        /* control: the caller resumes and frees the kept frame at its await */
        ASSERT_FN(test107_phase(drive, true));
        ASSERT_FN(CHK_BOOL(test107_caller_resumed == 1));
        ASSERT_FN(CHK_BOOL(test107_caller_err == want_err && test107_caller_ret == want_ret));
        ASSERT_FN(CHK_BOOL(test107_callee_dtor == 1 && test107_caller_dtor == 1));
        ASSERT_FN(CHK_BOOL(test107_callee_exit == 1 && test107_callee_unwait == 1));
        DBG("%s, control: caller resumed, err %d ret %d, both frames destroyed once",
                drive ? "driven" : "killed", test107_caller_err, test107_caller_ret);

        /* teardown: the caller dies in the ready drain never resumed; the kept frame must die
        with it, not leak, and the root's exit must not run a second time */
        for (int i = 0; i < 2; i++) {
            ASSERT_FN(test107_phase(drive, false));
            ASSERT_FN(CHK_BOOL(test107_caller_resumed == 0));
            ASSERT_FN(CHK_BOOL(test107_callee_dtor == 1 && test107_caller_dtor == 1));
            ASSERT_FN(CHK_BOOL(test107_callee_exit == 1 && test107_callee_unwait == 1));
            DBG("%s, teardown %d: caller destroyed queued, the kept frame destroyed once, its "
                    "EXIT and UNWAIT_SEM once", drive ? "driven" : "killed", i + 1);
        }
    }
    return 0;
}

int main() {
    int ret = test107_teardown_root_leak();
    print_test_result("018-051-reproduced_teardown_root_leak.cpp", ret >= 0);
    return ret;
}
