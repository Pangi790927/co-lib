# Proposal: a yield does not complete a create_future future (fix #14)

Status: [APPLIED 2026-10-29 on the user's word]

Apply verification, 2026-10-29: the test fails on the header before the fix (`future gave 7`) and
passes on the header as applied, two runs of two on Windows and one on Linux; `007-001`,
`008-001` and `018-004` pass on both; the whole suite on Windows `[PASSED: 102]/[FAILED: 0]`; on
Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 101]/[FAILED: 0]`, every file but the Windows-only
`018-029`. The check table is the last item of "How it was checked".

## The bug

`create_future()` installs one shared lambda (`exit_func`) on the wrapped task as BOTH a RETURN
modif and an EXIT modif. The RETURN slot does not only fire at a `co_return`: `cpp_yield_awaiter()`
sets `state->err = ERROR_YIELDED` and then calls `do_return_modifs()`, so a generator-style
`co_yield` fires it too — that is exactly what the RETURN modif's public contract documents ("on a
co_return (before the exit cbk) or on a co_yield: its err is ERROR_YIELDED if it yielded"), and
what the code comment above the lambda acknowledged ("the first of its return (a co_yield too)").
But `exit_func` never looked at `state->err` — unlike `killer_state_t::on_return()`, which checks
`s->err == ERROR_YIELDED` at the same callback. So the wrong step is simply the missing check: at
the wrapped task's `co_yield`, `yield_value()` has already emplaced the interim value into
`promise().ret`, `exit_func` snapshotted it, set `data->ready`, and signaled the future's sem. The
waiter woke holding the interim value, and the task's eventual real `co_return` hit the
`if (data->ready) return ERROR_OK;` guard and was discarded.

The trigger is a `create_future`-wrapped task that does a generator-style `co_yield` before its
real return (confirmed, `repro/14-create_future_yield.md`, REPRODUCED, deterministic, exit 1): the
awaited future resolved to the yielded 7 instead of the returned 42. One finding detail was
refined by the repro: the bare rescheduling `co_await co::yield();` does NOT fire RETURN modifs
(`yield_awaiter_t` runs only LEAVE and YIELD), so tasks using only the bare yield were never hit;
and at a `co_yield` the promise holds the interim value (not `T{}`), so the future resolves with
the yielded value — the `T{}` fallthrough stays reserved for dying without a value. Same defect
class either way: a non-final value completes the future and the real return is discarded.
`wait_all()` is built on `create_future()`, so it inherits the fix.

The fix mirrors the killer's precedent: `exit_func` returns early on `ERROR_YIELDED`, leaving
`data->ready` unset so the real return — or the EXIT slot at a death without a return — resolves
the future later.

## Where the fix lands

| colib.h region          | what changes                                       | adds/dels |
|-------------------------|----------------------------------------------------|-----------|
| `create_future()`'s     | its `exit_func` lambda gains an early return on   | +4 -1     |
| `exit_func` lambda and  | `state->err == ERROR_YIELDED`; the comment above  |           |
| its comment             | it drops "(a co_yield too)"                        |           |

One lambda in one function. The install sites (`CO_MODIF_RETURN_CBK` / `CO_MODIF_EXIT_CBK`), the
sem, the waiting coroutine, and `wait_all()` are untouched; the public doc of `create_future`
("returned once the return value of the task is available") already states the fixed contract, so
it needs no change.

## The change

```diff
--- base/colib.h
+++ patched/colib.h
@@ -6454,10 +6454,13 @@
     data->ready = false;
 
     /* ! this std::function will not be used using our allocator */
-    /* the first of its return (a co_yield too) and its exit, if it dies without returning */
+    /* its co_return, and its exit if it dies without returning: a co_yield is neither, the task is
+    alive and its real end still comes */
     auto exit_func = [sem, data](state_t *state) -> error_e {
         if (data->ready)
             return ERROR_OK;
+        if (state->err == ERROR_YIELDED)
+            return ERROR_OK;   /* a co_yield: it fires the return cbk without ending the task */
         typename task<T>::handle_t h = task<T>::handle_t::from_address(state->self.address());
         if (state->exception) {
             data->exc = state->exception;
```

The comment half rewrites what the lambda resolves at: the old text documented the bug ("a co_yield
too"), the new one states the contract — the task's `co_return`, or its death without a return; a
`co_yield` is neither, and the real end still comes.

The guard itself is placed after the `data->ready` check (an already-resolved future needs nothing
regardless of `err`) and before any capture, mirroring `killer_state_t::on_return()`'s own
`ERROR_YIELDED` check at the same callback — minus the `std::terminate`, because unlike a killer,
a future over a yielding task is not a contract violation: it just is not finished.
`cpp_yield_awaiter_t` resets `state->err = ERROR_OK` in its `await_resume` when the task is
resumed, so the real `co_return` after a yield arrives with a clean `err` and resolves normally
(the adjusted repro verifies exactly this path). The EXIT slot needs no separate handling: the
paths that fire it
(`final_awaiter_ends` after a return — where `ready` is already true — and `destroy_state` on a
death without return: killer, sem/io clear, timeout) deliver a state that is parked in a wait list
or mid-chain, never one suspended at a generator yield with `ERROR_YIELDED`, which is in no queue
and holds no caller. Scenario 3 of the regression test pins that death coverage behaviorally.

## The regression test

Landed as `tests/018-046-reproduced_future_yield_resolution.cpp` (Test102) on 2026-10-29; it was
first written as 018-029 / Test84, the names the older logs below carry.

It is the repro reshaped to the tests/ conventions. Scenario 1: a task that only does the bare
`co::yield()` (no RETURN callback fires) resolves its future with its return, as it always did.
Scenario 2, the bug: a task that `co_yield`s 7 and returns 42, driven by awaiting it twice, the
only way a `co_yield` task resumes; the interim 7 must still reach its awaiter and the future must
resolve to the 42. Scenario 3: a wrapped task killed while parked on a semaphore still resolves
its future through the EXIT slot (to `T{}`, since fix #24, which would surface the kill, is
pending). No assertion of the repro was weakened.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
yield gave 7, future gave 7, real return reached: 1
Failed CHK_BOOL(test102_b_future == 42) err_str: The operation completed successfully.
kill 0, destroyed 1, ran past 0, future 0
[FAILED]: 018-046-reproduced_future_yield_resolution.cpp
```

On the header as applied, two runs of two on Windows and one on Linux (WSL Ubuntu, g++ 15.2):

```
yield gave 7, future gave 42, real return reached: 1
kill 0, destroyed 1, ran past 0, future 0
[PASSED]: 018-046-reproduced_future_yield_resolution.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test102 - Reproduced Bugs: create_future resolved a future at a co_yield
================================================================================================= */

/* Was an open bug in create_future()'s exit_func: the same lambda was installed as both a RETURN
and an EXIT modif on the wrapped task, and the RETURN slot also fires on a generator-style
co_yield (with state->err == ERROR_YIELDED, set by cpp_yield_awaiter before do_return_modifs) -
but exit_func never checked that err, unlike killer_state_t::on_return(). At the task's first
co_yield the promise already holds the yielded interim value (yield_value emplaces it), so
exit_func resolved the future with that non-final value, set data->ready and signaled: the waiter
woke with the interim value and the task's real co_return was discarded (data->ready was already
true when it arrived). The bare rescheduling co::yield() never fires RETURN modifs (yield_awaiter_t
runs only LEAVE and YIELD), so only generator-style co_yield tasks were hit. Now fixed: exit_func
returns early on ERROR_YIELDED - a yield is not an end - so the future resolves at the real
co_return, or at the task's death without a return (the EXIT slot, which never carries
ERROR_YIELDED), never at a yield. Fix #14 (tests/fixes/FIX_14-create_future_yield.md).

The co_yield'ing task in scenario 2 is driven by awaiting it again (the only way a co_yield task
resumes - a caller-less, sched'd one that co_yields is orphaned at its yield, its future would
honestly stay pending forever, and no test can wait that out). Scenario 3 pins the EXIT slot's
death coverage the fix had to keep: a future whose wrapped task is killed mid-wait still resolves.
The silent T{} it resolves to (the kill error swallowed) is a separate open bug, repro/24. */

/* ---- observation state: written by the coroutines, read by main after run() drained ---- */

static int  test102_a_future = -1;    /* 1: value the future resolved to (bare yield path)    */
static int  test102_b_yield  = -999;  /* 2: interim value the first await handed out          */
static int  test102_b_future = -1;    /* 2: value the future resolved to (generator path)     */
static bool test102_b_returned = false; /* 2: the wrapped task reached its real co_return      */

static int  test102_c_kill   = -999;  /* 3: kill_fn() return                                  */
static bool test102_c_dtor   = false; /* 3: the victim's frame destroyed by the kill           */
static bool test102_c_ran    = false; /* 3: the victim resumed past its wait: must not happen  */
static int  test102_c_future = -1;    /* 3: value the future resolved to (death path)         */

/* ---- Scenario 1: a bare co::yield() must not resolve the future (and never did) ---- */

static co::task<int> test102_a_task() {
    co_await co::yield();    /* bare rescheduling yield: LEAVE/YIELD modifs only, no RETURN */
    co_return 42;
}

static co::task_t test102_a_driver() {
    auto pool = co_await co::get_pool();
    auto t = test102_a_task();
    auto f = co::create_future(pool, t);
    co_await co::sched(t);
    test102_a_future = co_await f;
    co_return 0;
}

static int test102_bare_yield() {
    auto pool = co::create_pool();
    pool->sched(test102_a_driver());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test102_a_future == 42));   /* the real co_return, unchanged */
    return 0;
}

/* ---- Scenario 2: a generator co_yield must not resolve the future (the bug) ---- */

static co::task<int> test102_b_task() {
    co_yield 7;               /* fires the RETURN slot with ERROR_YIELDED */
    test102_b_returned = true;
    co_return 42;
}

static co::task_t test102_b_driver() {
    auto pool = co_await co::get_pool();
    auto t = test102_b_task();
    auto f = co::create_future(pool, t);
    test102_b_yield = co_await t;    /* drives it to its co_yield: hands the interim 7 out */
    co_await t;                     /* drives it to its real co_return 42 */
    test102_b_future = co_await f;   /* a yield is not an end: must be the 42 */
    co_return 0;
}

static int test102_generator_yield() {
    auto pool = co::create_pool();
    pool->sched(test102_b_driver());
    ASSERT_FN(pool->run());
    DBG("yield gave %d, future gave %d, real return reached: %d",
            test102_b_yield, test102_b_future, (int)test102_b_returned);
    ASSERT_FN(CHK_BOOL(test102_b_yield == 7));     /* the interim value still reaches its awaiter */
    ASSERT_FN(CHK_BOOL(test102_b_returned));       /* the wrapped task ran to its real return */
    ASSERT_FN(CHK_BOOL(test102_b_future == 42));   /* the future took the real return, not the 7 */
    return 0;
}

/* ---- Scenario 3: death without a return still resolves the future (coverage kept) ---- */

struct test102_c_marker_t { ~test102_c_marker_t() { test102_c_dtor = true; } };

static co::task<int> test102_c_victim(co::sem_p never) {
    test102_c_marker_t m;
    co_await never->wait();    /* never signaled: only the kill ends it */
    test102_c_ran = true;       /* killed means destroyed, not resumed */
    co_return 42;
}

static co::task_t test102_c_driver(co::sem_p never, const co::modif_pack_t& killer_mods,
        std::function<co::error_e(void)> kill_fn) {
    auto pool = co_await co::get_pool();
    auto t = co::add_modifs(pool, test102_c_victim(never), killer_mods);
    auto f = co::create_future(pool, t);
    co_await co::sched(t);
    co_await co::yield();          /* let the victim run and park on the sem */
    test102_c_kill = kill_fn();
    test102_c_future = co_await f;  /* the EXIT slot fires on its death */
    co_return 0;
}

static int test102_death_still_resolves() {
    auto pool = co::create_pool();
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    pool->sched(test102_c_driver(never, killer_mods, kill_fn));
    ASSERT_FN(pool->run());
    DBG("kill %d, destroyed %d, ran past %d, future %d",
            test102_c_kill, (int)test102_c_dtor, (int)test102_c_ran, test102_c_future);
    ASSERT_FN(CHK_BOOL(test102_c_kill == co::ERROR_OK));   /* the kill verifiably happened */
    ASSERT_FN(CHK_BOOL(test102_c_dtor));
    ASSERT_FN(CHK_BOOL(!test102_c_ran));
    ASSERT_FN(CHK_BOOL(test102_c_future != -1));           /* death resolved the future */
    return 0;
}

int main() {
    int ret = test102_bare_yield();
    ret |= test102_generator_yield();
    ret |= test102_death_still_resolves();
    print_test_result("018-046-reproduced_future_yield_resolution.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds ran in `repro/fix/14/` (patched copy) and `repro/fix/14/base/` (pristine copy of the
current colib.h), with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I.`, per the fix spec.
The diff of base vs patched copy is exactly the hunk above (`repro/fix/14/14-fix.diff`, +4/-1
lines).

1. The original repro fails on the unpatched copy — fresh run, matching the recorded verdict
   (exit 1; scenario B: "future value : 7 (WRONG)", task orphaned at its yield, real return
   discarded) — `repro/fix/14/14-base-run.log`.

2. The repro, adjusted to assert the fixed behavior (scenario B's generator driven by awaiting —
   the only way a co_yield task resumes; scenario C added to pin the EXIT death coverage):
   - base copy: FAILED, exit 1 — "future value : 7 (WRONG) ... real return discarded : YES
     (ready was already true at the yield)" (`repro/fix/14/base/14-fixed-base-run.log`) — the
     adjusted driver reproduces the bug just as the original shape did.
   - patched copy: PASSED, exit 0, four runs — "future value : 42 (correct) ... real return
     discarded : no", scenario A "42 (correct)" both sides, scenario C resolved at the kill
     (`repro/fix/14/14-fixed-run.log`, `-run2.log`, `-run3.log`, `14-fixed-final-run.log`).
   - The original repro's own sched'd shape on the patched copy, run once for the record: its
     wrapped task co_yields caller-less, is orphaned at the yield as before, and its future now
     honestly never resolves (value stays unset, `pool->run()` still returns RUN_OK with the
     waiter parked) — exit 1 by its own (bug-shaped) assertion
     (`repro/fix/14/14-orig-on-patched-run.log`). This shape is Open question 1, not a regression:
     pre-fix it "resolved" to the interim value while the same task was just as orphaned.

3. The regression test above, run against both copies (plain build):
   - base copy: FAILED — `Failed CHK_BOOL(test84_b_future == 42)` with
     `yield gave 7, future gave 7, real return reached: 1` (`repro/fix/14/base/` run, exit 127)
   - patched copy: PASSED, exit 0 — `yield gave 7, future gave 42, real return reached: 1` and
     `kill 0, destroyed 1, ran past 0, future 0` (`repro/fix/14/018-029-patched-run.log`)

4. No-regression, the three closest existing tests against the patched copy (include changed to
   `"colib.h"`, nothing else): all PASSED, exit 0 —
   - `007-001-futures.cpp` (create_future's documented usage, sched'd, no yield)
   - `008-001-wait_all.cpp` (wait_all, built on create_future)
   - `018-004-reproduced_future_exception_propagation.cpp` (create_future's exit_func exception
     and default-construction branches — the other arms of the same lambda)

5. NOT-CHECKED: no Linux/kqueue build on this box (no make, per the spec). The changed region is
   platform-independent C++ with no `#if COLIB_OS_*` around it; the RETURN/EXIT firing paths it
   depends on are the shared engine (`cpp_yield_awaiter`, `final_awaiter_ends`, `destroy_state`),
   so reading is the validation there.

6. At apply time, 2026-10-29, on the header of that day (fixes #11, #12, #38, #39, #40, #48 and
   `wait_msg()` in since the checks above), the diff rebased unchanged:

   | check                       | result                                                       |
   |-----------------------------|--------------------------------------------------------------|
   | documented usage reaches it | yes: a `create_future`-wrapped task that generator-co_yields |
   |                             | before its return; `wait_all()` inherits it                  |
   | DESIGN.md                   | its rules concern the killer, which keeps its own            |
   |                             | `ERROR_YIELDED` terminate in its RETURN callback; no kill    |
   |                             | path changes, scenario 3 pins the EXIT path                  |
   | epoll / kqueue              | the lambda is platform-independent; the test, `007-001`,    |
   |                             | `008-001` and `018-004` pass on Linux (WSL, g++ 15.2);       |
   |                             | kqueue NOT-CHECKED (WIP)                                     |
   | contract / docs             | the doc ("wait for the return value", "once it has finished  |
   |                             | executing") now holds; the comment above the lambda said     |
   |                             | "(a co_yield too)", which the user ruled a description of    |
   |                             | the bug, not intent; no declaration changed, `02_api.md`     |
   |                             | stays                                                        |
   | callers                     | `wait_all()` and the tests; a scheduled task that yields and |
   |                             | is never awaited again now leaves its future pending (open   |
   |                             | question 1, the user's A)                                    |
   | pending fixes               | #24 edits the same lambda: its storage goes below this       |
   |                             | guard; #26 is adjacent through `destroy_state`               |
   | runs                        | the test fails before (`future gave 7`) and passes after (2  |
   |                             | of 2 on Windows, 1 on Linux); the nearby futures tests pass  |
   |                             | on both; the whole suites: see the status line               |

## Interactions

The create_future-family row (`repro/fix/00-FIXSTATUS.md`: 14 24, with 26 adjacent via
destroy_state):

- #24 (the killed member's future swallows the error): the two edits land in the SAME lambda, and
  they compose rather than conflict — explicitly: whatever #24 adds to surface the kill (storing
  the death's error and handing it to the waiter) must sit BELOW this fix's `ERROR_YIELDED` guard,
  so it too never fires at a yield; my guard returns before any capture, so #24's storage logic
  sees only real ends (a return, or a death whose EXIT slot never carries ERROR_YIELDED — the
  killed victim of repro/24 arrives at `destroy_state` with `err == ERROR_OK`, verified
  behaviorally by scenario C resolving post-fix). Neither fix subsumes the other: mine says WHEN
  the lambda may act, #24 says WHAT the death path reports. If #24 lands first, this fix's guard
  slots in above its storage unchanged; if this one lands first, #24's edit goes below the guard.
- #26 (teardown root leak, adjacent via destroy_state): post-fix, a future whose wrapped task
  co_yields caller-less and is never re-driven stays pending forever — its waiter parks on the
  future's sem, and only pool teardown (sem clear -> destroy_state, #26's region) would end it.
  No line overlap; worth re-running #26's repro after both land to see the abandoned waiter.
- The killer-generator tests (`002-008`..`002-012`, `018-019`/`018-020`/`018-021`): the killer's
  own `on_return` terminates on YIELDED independently of this change; nothing in the killer or
  the generator engine was touched (my design mirror, not my edit site).

## Open questions for the user

1. The contract decision, flagged per the briefing: a yield never completes a create_future
   future, under any shape. The observable change beyond the fixed bug: a sched'd (caller-less)
   wrapped task that generator-co_yields and is never awaited again now leaves its future pending
   forever — the waiter parks, `run()` reports RUN_OK (verified, `14-orig-on-patched-run.log`);
   pre-fix that future "resolved" to the interim value while the task was equally orphaned. I
   chose the honest pending state over both wrong resolutions, because the public doc already
   promises "the return value of the task", and because abandoned co_yield tasks are an existing
   hazard of their own (the 018-019/018-020 generator-leak tests). If you would rather have
   create_future auto-drive a yielding wrapped task (a YIELD-slot modif that requeues it, so a
   sched'd generator runs to its return), that is a semantic addition to create_future, not this
   fix — say the word and it becomes its own proposal.
2. Composition order with #24, stated here so the two land coherently (also in Interactions):
   #24's error-storage must sit below this fix's `ERROR_YIELDED` guard — same lambda, and the
   guard is what keeps the kill-surfacing from ever firing at a yield.

Decided at apply time, 2026-10-29, on the user's word: (1) A, the doc is the contract: a future
resolves at the task's `co_return`, or at its death without a return, never at a `co_yield`; the
comment above the lambda, which said "(a co_yield too)", described the bug, not an intent, and a
scheduled yielding task nobody awaits again leaves its future pending, as every other library's
future over a sequence does; no auto-driving of a yielding task. (2) When #24 lands, its storage
goes below this guard.
