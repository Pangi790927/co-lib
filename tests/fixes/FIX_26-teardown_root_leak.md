# Proposal: the pool frees a killed root whose woken caller never resumes (fix #26)

Status: [APPLIED 2026-10-29 on the user's word, in a different shape: the engine frees the frame a
dying caller still names, the pool and the killer unchanged]

Apply verification, 2026-10-29: the test fails on the header before the fix (the root's
destructor never fires at the first teardown) and passes on the header as applied, two runs of
two on Windows and one on Linux; the killer, pool, modif and futures family tests pass; the whole
suite on Windows `[PASSED: 107]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll)
`[PASSED: 106]/[FAILED: 0]`, every file but the Windows-only `018-029`. The check table is the
last item of "How it was checked"; the title above is the proposal's, the shape applied is in
"Where the fix lands".

## The bug

`killer_state_t::end_chain()` ends a killed chain by running `destroy_state(top, root)`, and
`destroy_state` deliberately does not destroy a root that has a caller: it only fires the root's
EXIT, keeping the frame alive, because its destruction belongs to that caller — the documented
contract is that the caller's `task<T>::await_resume` performs the `h.destroy()` when the kill's
error wakes it. `end_chain` also queues that woken caller into the pool's ready queue
(`queue_caller(root)`). Between the kill and the caller's resume, the killed root's frame is in no
queue at all and is owned by nobody: `destroy_state` walks `caller_state` links upward only, so
from the suspended caller's side the awaited callee is unreachable — its handle lives inside the
caller's coroutine frame, outside colib's state graph.

The wrong step sits in `pool_internal_t::clear()`. Its ready-queue drain does
`close_wait(state); destroy_state(state);` on every queued task *without resuming it*, so a caller
that the kill had woken is destroyed still suspended at its `co_await`. That destroys the frame
holding the only reference to the killed callee's frame, and nothing ever destroys the callee: no
callback runs (its EXIT already ran at the kill), its locals' destructors never run, `~state_t()`
never runs, and the heap block is never freed. The at-exit debug checker stays silent about it,
because the kill-time EXIT already erased the frame's `dbg_check_coro_states` entry — the leak is
invisible to every instrument colib has.

The trigger (confirmed, `repro/26-teardown_root_leak.md`, REPRODUCED, deterministic: two clean
runs plus an instrumented trace build, all identical, exit 1): a caller co_awaits a callee that
parks on a never-signaled semaphore, a `create_killer` pack is attached to the callee, one
`pool->run()` parks both, then `kill_fn()` exits the callee and wakes the caller into
`ready_tasks`, and the pool is destroyed (`pool.reset()`) before any second `run()`. The observed
consequence: both leak runs destroyed the queued caller at teardown (`caller_dead=1`) while the
killed callee's marker destructor never fired (`callee_dead=0,0`) and its address never appeared
in a single `ENDING STATE` trace line — the frame leaked. The control run in the same repro proves
the contract intact on the resume path: with a second `run()`, the caller resumes, `await_resume`
destroys the callee (`callee_dead=1`), and the kill's error (`-4`, ERROR_USER) is delivered. This
is not UB and not a crash; the process exits cleanly and bleeds one frame per occurrence.

## Where the fix lands

As applied on 2026-10-29, in a shape the user chose over the proposal's, under DESIGN.md 2 (the
library can't depend on the killer): no list in the pool, no teardown phase, no killer code. The
engine keeps its own promise, "a root that was called is only exited: its caller destroys it",
at the one place it broke it. A call is recorded in the caller's wait slot (`wait`, `wait_on`,
which a call left empty), as a wait on the callee, from the call's suspend to its resume; a frame
left to a caller is then named by that caller's call, and `destroy_state()` frees the frame a
dying caller still names. The two creators of such frames, a kill's kept root and the returned
root a RETURN modif parks (the driven root, which the proposal predates), need no code: both
leave the caller's call naming the frame.

| colib.h region               | what changes                                               | adds/dels |
|------------------------------|------------------------------------------------------------|-----------|
| `state_t`                    | `WAIT_CALL` in `wait_e`; `wait_on`'s comment names the     | +2/-2     |
|                              | callee; no new member                                      |           |
| `state_access_t`             | `set_wait(s, callee)` and `called(s)`                      | +7/-0     |
| `destroy_state()`            | frees the frame `curr` still names before freeing `curr`;  | +8/-2     |
|                              | unnames a frame it frees from its caller; the comment      |           |
|                              | states the contract's two ends                             |           |
| `task<T>::await_suspend()`   | the caller's call names the callee                         | +1/-0     |
| `task<T>::await_resume()`    | the call is over: the name goes before the callee is read  | +1/-0     |
|                              | and freed                                                  |           |

Total +20/-4, all engine code, none inside an `#if COLIB_OS_*` guard. Every other reader of the
wait slot (`close_wait`, `woken_with_effect`, the killer's `stop_io`) checks for its own kind
(io, sem, yield), so a call in the slot passes through them untouched, and the drain's
`close_wait` of a queued caller doesn't clear it. No declaration or public doc changes;
`02_api.md` mirrors no `state_t` internals.

## The change

`diff -u` of the header right before this fix (2026-10-29, with fixes #11, #12, #14, #16, #24,
#32, #33, #38, #39, #40, #48 and `wait_msg()` in) against the header as applied. The proposal's
diff (a `killed_roots` list in the pool, fed by `end_chain`, drained by `clear()`, +24/-2) was
not applied. Two other shapes were built and rejected on the way: the proposal's, for making the
pool carry state only the killer produces (DESIGN.md 2), and a killer-side one (a modification
on the caller whose ENTER marks the frame taken and whose EXIT frees it, +34/-2, verified
107/107), for making the killer do the engine's bookkeeping; the user found it worse to read. A
`state_t` pointer for the owed frame was considered and dropped: the wait slot holds the fact.

Reasoning per hunk:

- `state_t`, `state_access_t`: a call becomes a kind of wait, on the callee, next to io, sem and
  yield; the accessor pair follows the three that exist.
- `destroy_state()`: the one rule has two ends. A frame left to a caller is freed by the caller
  at its resume (`await_resume`, unchanged) or with the caller when the caller is destroyed
  first; the second end was missing. The owed frame's exit already ran (at its kill, or at its
  return in `final_awaiter_ends`), so it is only freed, one hop: a frame that ended owes nothing
  itself. A frame the walk frees is unnamed from its caller first, so the caller's turn finds
  nothing owed, and the kept root keeps its name, which is the record.
- `await_suspend()`, `await_resume()`: the call names the callee from the suspend to the resume;
  the resume unnames it before reading and freeing (or keeping, for a yield) the callee, so the
  engine never sees a taken frame as owed.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -1202,12 +1202,13 @@
     friend struct state_list_t;
     friend struct state_access_t;
 
-    enum wait_e : uint8_t { WAIT_NONE, WAIT_IO, WAIT_SEM, WAIT_YIELD };
+    enum wait_e : uint8_t { WAIT_NONE, WAIT_IO, WAIT_SEM, WAIT_YIELD, WAIT_CALL };
 
     state_t *prev = nullptr;
     state_t *next = nullptr;
     state_list_t *list = nullptr;           /* the queue it is linked in, if any */
-    void *wait_on = nullptr;                /* the io_desc_t or the sem_t of that wait, if any */
+    void *wait_on = nullptr;                /* the io_desc_t or the sem_t of that wait, if any;
+                                               the callee's state_t of a call */
     state_e state = STATE_LEFT;
     wait_e wait = WAIT_NONE;                /* the wait it is in, from WAIT to UNWAIT */
 
@@ -2597,6 +2598,10 @@
     static void set_wait(state_t *s, io_desc_t *io) { s->wait = state_t::WAIT_IO; s->wait_on = io; }
     static void set_wait(state_t *s, sem_t *sem) { s->wait = state_t::WAIT_SEM; s->wait_on = sem; }
     static void set_wait_yield(state_t *s) { s->wait = state_t::WAIT_YIELD; s->wait_on = nullptr; }
+    static void set_wait(state_t *s, state_t *callee) {
+        s->wait = state_t::WAIT_CALL;
+        s->wait_on = callee;
+    }
     static void clear_wait(state_t *s) { s->wait = state_t::WAIT_NONE; s->wait_on = nullptr; }
     static io_desc_t *wait_io(state_t *s) {
         return s->wait == state_t::WAIT_IO ? (io_desc_t *)s->wait_on : nullptr;
@@ -2605,6 +2610,9 @@
         return s->wait == state_t::WAIT_SEM ? (sem_t *)s->wait_on : nullptr;
     }
     static bool wait_yield(state_t *s) { return s->wait == state_t::WAIT_YIELD; }
+    static state_t *called(state_t *s) {
+        return s->wait == state_t::WAIT_CALL ? (state_t *)s->wait_on : nullptr;
+    }
 };
 
 /* Those are needed for destroy_state, internally and to call it */
@@ -2627,7 +2635,8 @@
 }
 
 /* destroys `curr` and its callers; with a `root` it stops there, and a root that was called is
-only exited: its caller destroys it */
+only exited: its frame is freed by its caller at the resume, or with its caller here when that
+resume never comes, since the caller's call still names the frame it was left. 2026-10-29 */
 inline void destroy_state(state_t *curr, state_t *root = nullptr) {
     COLIB_DEBUG_TRACE_SCOPE("to destroy: %p", curr);
     while (curr) {
@@ -2637,8 +2646,13 @@
         if (curr == root) {
             if (!next)
                 curr->self.destroy();
-            return;
+            return;     /* kept for its caller, whose call still names it */
         }
+        if (state_t *owed = state_access_t::called(curr))
+            owed->self.destroy();   /* left to it (exited, or returned) and never taken: its
+                                       exit already ran, the frame goes with its caller */
+        if (next && state_access_t::called(next) == curr)
+            state_access_t::clear_wait(next);   /* destroyed here, nothing left to the caller */
         curr->self.destroy();
         curr = next;
     }
@@ -3098,6 +3112,7 @@
     state_t *state = &h.promise().state;
     state->caller_state = &caller.promise().state;
     state->pool = caller.promise().state.pool;
+    state_access_t::set_wait(state->caller_state, state);   /* the caller waits on its callee */
 
     inherit_modifs(state, caller.promise().state.modif_table, CO_MODIF_INHERIT_ON_CALL);
 
@@ -3123,6 +3138,7 @@
 
     ever_called = true;
     state_t *caller_state = h.promise().state.caller_state;
+    state_access_t::clear_wait(caller_state);       /* the call is over */
     if (h.promise().state.err == ERROR_YIELDED)
         h.promise().state.caller_state = nullptr;   /* yielded: no parent until it's awaited again */
     do_entry_modifs(caller_state);
```

## The regression test

Landed as `tests/018-051-reproduced_teardown_root_leak.cpp` (Test107) on 2026-10-29; it was first
written as 018-036 / Test85, the names the older logs below carry, and gained the driven path at
landing.

Two paths, each with a control phase and two teardown phases. The root parks on a semaphore under
a killer; its caller `co_await`s it. Killed at the park (`kill_fn()` answers ERROR_OK) or driven
to its return after the semaphore was signaled (`kill_fn()` answers ERROR_FINISHED, the root ran
past its wait), the caller is queued with the root's frame left to it. Control: a `run()` resumes
the caller, which frees the frame and reads the kill's error (or the driven root's 42). Teardown:
the pool is destroyed with the caller still queued; the caller dies in the ready drain never
resumed, the kept frame dies exactly once with it (a local's destructor), and the root's EXIT and
UNWAIT_SEM callbacks ran exactly once each.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile,
it fails at the first teardown, the root's destructor never firing:

```
killed, control: caller resumed, err -4 ret 0, both frames destroyed once
Failed CHK_BOOL(test107_callee_dtor == 1 && test107_caller_dtor == 1) err_str: The operation completed successfully.
[FAILED]: 018-051-reproduced_teardown_root_leak.cpp
```

On the header as applied, two runs of two on Windows and one on Linux (WSL Ubuntu, g++ 15.2):

```
killed, control: caller resumed, err -4 ret 0, both frames destroyed once
killed, teardown 1: caller destroyed queued, the kept frame destroyed once, its EXIT and UNWAIT_SEM once
killed, teardown 2: caller destroyed queued, the kept frame destroyed once, its EXIT and UNWAIT_SEM once
driven, control: caller resumed, err 0 ret 42, both frames destroyed once
driven, teardown 1: caller destroyed queued, the kept frame destroyed once, its EXIT and UNWAIT_SEM once
driven, teardown 2: caller destroyed queued, the kept frame destroyed once, its EXIT and UNWAIT_SEM once
[PASSED]: 018-051-reproduced_teardown_root_leak.cpp
```

The file as landed:

```cpp
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
```

## How it was checked

All builds ran in `repro/fix/26/` with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I.`
(ASan where noted), per the fix spec. `base/` holds the pristine copy of the current colib.h
(your uncommitted accept fix as-is), the workspace root the patched one; `repro/fix/26/26-fix.diff`
is the exact diff above (+24/-2). No build emitted a warning.

1. The repro fails on the unpatched copy — fresh run, matching the recorded verdict
   (`repro/26-teardown_root_leak.md`: REPRODUCED, two clean runs plus an instrumented trace, all
   identical), exit 1 (`base/26-base-run.log`):
   ```
   [leak #1] after teardown: caller_dead=1 callee_dead=0
       ... leak #2 identical (fresh pool, callee_dead=0) ...
   FAIL-REPRO: both leak runs destroyed the queued caller at teardown (caller_dead=1) while the
   killed callee's frame was never destroyed (callee_dead=0,0) - the frame leaked
   ```
2. The fix applied to the patched copy; the repro passes unchanged, exit 0, plain and under the
   trace+debug-checks build (`26-fixed-run.log`, `26-fixed-trace.log`):
   ```
   [leak #1] after teardown: caller_dead=1 callee_dead=1
   [leak #2] after teardown: caller_dead=1 callee_dead=1
   PASS: no leak observed (control destroyed the callee; leak runs did too)
   ```
   The trace shows the new phase doing exactly what it was drawn to do —
   `clear() :> Cleaning killed roots`, the callee's marker and `ENDING STATE` fire there, then
   `clear() :> Cleaning wait queue` destroys the never-resumed caller — and the control phase
   shows the resume path untouched: the callee dies inside `await_resume` ("caller resumed" logs
   after its `ENDING STATE`), and the later teardown's `Cleaning killed roots` finds the list
   already empty, because `~state_t()` unlinked the root when the caller destroyed it.
3. The regression test above, run against both copies (plain build):
   - base copy: FAILED, exit 1 — the control phase passes, the first teardown phase fails on
     `Failed CHK_BOOL(test85_callee_dtor == 1 && test85_caller_dtor == 1)`
     (`base/018-036-reproduced_teardown_root_leak-base-run.log`).
   - patched copy: PASSED, exit 0 (`018-036-fixed-run.log`):
     ```
     control: caller resumed, kill error -4 delivered, both frames destroyed once
     teardown run 1: caller destroyed queued (never resumed), killed root destroyed exactly once,
                     its EXIT and UNWAIT_SEM exactly once
     teardown run 2: (same)
     [PASSED]: 018-036-reproduced_teardown_root_leak.cpp
     ```
4. No-regression, the five closest existing tests against both copies (include changed to
   `"colib.h"`, nothing else), all PASSED exit 0 on base and on the patched copy:
   - `004-001-pool_clear.cpp` (clear()'s own contract)
   - `012-002-introspection_state.cpp` (state lifecycle names — the new list is invisible to it)
   - `002-002-flowctrl_create_killer.cpp`, `018-005-reproduced_killer_after_completion.cpp`,
     `018-014-reproduced_killer_outlives_pool.cpp` (the killer contract around `end_chain`: a
     returned-before-the-kill target, a kill after completion, a killer outliving its pool)
5. Composition with the live proposals that touch the same regions, one merged copy per family
   and one carrying all four:
   - `merged-25/` (this fix + FIX_25's two-phase `io_pool_t::clear()`): the repro 26 PASS, the
     regression test 018-036 PASS, FIX_25's own regression test PASS plain and under ASan, and
     `004-001-pool_clear` PASS.
   - `merged-1424/` (this fix + FIX_14 + FIX_24): 018-030 (FIX_24's test) PASS, 018-036 PASS,
     and FIX_14's test PASS in a composed-copy variant — see Interactions for the one line of
     that test its 14-only wording could not survive, and for the captured crash of its
     unadjusted form.
   - `merged-all/` (this fix + FIX_14 + FIX_24 + FIX_25, `fix25.diff` applied cleanly at a
     22-line offset): all four regression tests PASS, exit 0.
6. NOT-CHECKED: no Linux/kqueue/epoll build on this box (Windows + cl only, no make, per the
   spec). Every changed region — `pool_internal_t`, `killer_state_t::end_chain` — is
   platform-independent C++ behind no `#if COLIB_OS_*`; the io backends' own `clear()` bodies are
   not touched, so reading is the validation there.

At apply time, 2026-10-29, on the header of that day, in the applied shape (the call in the wait
slot):

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: a kill of a called root, or a drive that ends at the    |
|                             | root's return, with the pool destroyed before the caller     |
|                             | runs again; deterministic                                    |
| DESIGN.md                   | 1 and 3 hold, the killer is untouched; 2 is why this shape:  |
|                             | the engine keeps its own promise, the library gains nothing  |
|                             | killer-specific                                              |
| epoll / kqueue              | engine code, platform-independent; the test builds and       |
|                             | passes on Linux (WSL, g++ 15.2); kqueue NOT-CHECKED (WIP)    |
| contract / docs             | "its caller destroys it" now holds when the caller dies      |
|                             | first too, stated in `destroy_state`'s comment; no public    |
|                             | declaration or doc changes                                   |
| callers                     | every reader of the wait slot checks its own kind; the three |
|                             | frees of `await_resume` unname the callee first; the chain   |
|                             | walk unnames what it frees; a yielded generator is unnamed   |
|                             | at the caller's resume and never owed                        |
| pending fixes               | #25 (the two-phase io-pool `clear()`) and #27 (the cross-    |
|                             | pool sem) sit elsewhere; the tracker's "25 before 26" was a  |
|                             | preference, both orders were verified by the proposal        |
| runs                        | the test fails before (the root's destructor never fires)    |
|                             | and passes after (2 of 2 on Windows, 1 on Linux); the        |
|                             | killer, pool, modif and futures family tests pass; the       |
|                             | whole suites: see the status line                            |

## Interactions

The tracker rows: `create_future family: 14 24 (26 adjacent via destroy_state)`, the
`kill()/end_chain` row (`02 03 06`), and the clear() row shared with `25`.

- **#24** (a killed future surfaces its kill; it rewrites `end_chain`'s error write to
  unconditional): the two hunks compose textually and semantically, verified on `merged-1424`
  and `merged-all`. The composed `end_chain` reads: capture `kept_root`, write `root->err`
  unconditionally, `queue_caller`, `close_wait`, `destroy_state`, then push. For a killed
  *called* root nothing of #24's change is observable through my hunk (the caller still reads
  the same error at its await — my test's control phase delivers `-4` through it); for a killed
  *scheduled* (future-wrapped) root `kept_root` is null, the root is destroyed fully at its
  kill exactly as before, and nothing enters the list — #24's kill-surfacing is untouched. The
  one composed edge worth naming: a future-wrapped task that is *called* (co_awaited directly
  rather than sched'd) and killed becomes a kept root; its EXIT stores #24's `task_killed_t` at
  the kill, and if the pool tears down before the caller resumes, this fix frees the frame while
  the waiter dies in the sem or ready drain — each piece in its own phase.
- **#14** (a yield does not complete a future; same `create_future` lambda as #24, no shared
  lines with this fix): no direct overlap. Two verified findings from the merged runs:
  - *#14's pending-forever waiter at teardown.* Post-#14, a sched'd wrapped task that
    generator-co_yields and is never driven again leaves its future pending, the waiter parked
    on the future's sem. Probed directly on the `merged-1424` copy (`26-14edge.cpp`): the
    waiter is still pending when `run()` returns (`waiter_dtor=0`), and the teardown ends it —
    through the *sem-clear phase* of the same `clear()` this fix extends (`sem_t::clear()`'s
    close_wait+destroy_state, pre-existing machinery), not through the killed_roots drain. The
    abandoned-at-yield wrapped task itself stays leaked (`victim_dtor=0`): no kill ever took it,
    it is not a killed root, and this fix does not claim it — that is the pre-existing
    abandoned-generator hazard of `018-019`/`018-020`, unchanged.
  - *#14's test under #24.* FIX_14's regression test, scenario 3, awaits the killed victim's
    future expecting the silent `T{}` resolution (it was written against a 14-only tree, and its
    own header says the swallowed kill is repro/24). On the composed copy that `co_await` throws
    #24's `task_killed_t`, uncaught, out through `pool->run()`: the process terminates with no
    output (captured, `018-029-merged1424-unadjusted-run.log`: exit 127, zero bytes — the same
    termination shape FIX_24's proposal recorded for its own unadjusted repro). The composed-copy
    variant that catches `task_killed_t` and asserts `e == ERROR_USER` passes. Whichever of #14
    and #24 lands second owes that scenario the thrown-contract wording.
- **#25** (the two-phase clear): #25 restructures `io_pool_t::clear()` and the fd-table clear
  into take-then-end phases; `pool_internal_t::clear()` still calls `io_pool.clear()` first,
  then the semaphores, and this fix's new phase slots between the semaphores and the ready
  drain. The composition that matters: an io waiter that #25's second phase destroys may itself
  be the *caller* owing a killed root — before this fix that callee leaked exactly as repro 26
  shows, after it the killed_roots phase frees it once the io and sem sweeps are done. Verified
  both ways: the pairwise copy (`merged-25`, both regression tests plus the repro plus
  `004-001`, plain and ASan) and the four-fix copy (`merged-all`, all four regression tests).
- **#02** (kill()'s switch cases; #03, its STATE_WAITING_IO early return, was dropped on
  2026-10-13): it changes which kills reach `end_chain`, not what `end_chain` does — whatever
  reaches it flows through the same composed body.

## Open questions for the user

1. **The shape of the remembering, the flagged design decision.** The drain cannot reach the
   callee from the queued caller at teardown — the handle is a local of the caller's suspended
   frame, outside colib's state graph (that is the bug's mechanism, verified in the repro's
   trace) — so the fix must record the kept root somewhere at kill time. This proposal's answer
   is a pool-level `killed_roots` list, because `state_list_t` is the instrument the ready queue
   and the sem queues already are, `clear()` is the sweep that always runs before the list dies,
   and `~state_t()`'s existing unlink makes the resume path self-cleaning. The alternatives —
   a per-state "kept" flag walked by scanning the pool's allocators, or resuming the queued
   callers at teardown instead of destroying them — are heavier or change the teardown contract;
   say the word if you want one of them explored instead.
2. **The drain's position.** The killed roots are freed before the ready queue (the callee dies
   before the caller that owed it, mirroring the resume path's order). Nothing observable
   depends on it — `destroy_state` on the queued caller never touches the callee — so it is a
   convention; if you prefer the ready drain first, it is a two-line move.
3. **The lifetime a kept root now visibly has.** Between its kill and its caller's resume (or
   the pool's clear), a killed called root sits in the pool's list. Its lifetime is unchanged —
   the caller owed the frame before this fix too — but it is now *reachable* pool state; a pool
   kept alive indefinitely after such a kill holds the frame the same as before, visibly. I read
   that as an improvement (an inspector can name it), not a contract change, but it is yours to
   rule on.
4. **Naming and numbers.** `killed_roots` / `push_killed_root` follow the `ready_tasks` /
   `push_ready` pair; the test lands as `018-036` nominally (Test85) — both reassigned at
   landing if the live proposals' numbers shift.

Decided at apply time, 2026-10-29, on the user's word: (1) not the pool-level list: the user
recalled DESIGN.md 2, the library doesn't change to fit the killer; a killer-side shape was built
and rejected as worse to read; the applied shape records the call in the caller's existing wait
slot and lets `destroy_state` free what a dying caller still names, no new `state_t` member
("yet another pointer in coroutine state..."); (2) the drain's order is unchanged, the owed frame
goes right before the caller that owed it, wherever that caller is destroyed; (3) nothing new is
visible in the pool; a caller's call is now visible as a wait on its callee; (4) no new names
beyond `WAIT_CALL`, `set_wait(s, callee)` and `called(s)`.
