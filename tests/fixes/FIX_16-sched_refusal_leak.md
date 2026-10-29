# Proposal: a SCHED-refused sched kills the task and reports the error (fix #16)

Status: [APPLIED 2026-10-29 on the user's word, in a different shape: the SCHED slot's answers
are ignored, a schedule is neither refused nor held]

Apply verification, 2026-10-29: the test fails on the header before the fix (no task ran) and
passes on the header as applied, two runs of two on Windows and one on Linux; the modif-family
tests pass; the whole suite on Windows `[PASSED: 106]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++
15.2, epoll) `[PASSED: 105]/[FAILED: 0]`, every file but the Windows-only `018-029`. The check
table is the last item of "How it was checked"; the title above is the proposal's, the shape
applied is in "Where the fix lands".

## The bug

`pool_internal_t::sched()` ran the SCHED callbacks of a task and then did this:

```cpp
if (do_sched_modifs(state) != ERROR_OK) {
    return ;
}
```

A SCHED modif returning an error refused the schedule, and the `return` walked away having done
nothing else: the task was never queued, so it sat in no list of the pool, `pool_t::clear()`
sweeps only the ready queue, the io waiters and the semaphores, and no caller existed to free it.
Three things followed, all confirmed by `repro/16-sched_refusal_leak.md` (REPRODUCED, behavioral,
deterministic 2/2):

- the task's EXIT callbacks never fired - not at the sched, not during `run()`, not at pool
  destruction - so user cleanup hooked to EXIT was silently lost;
- the coroutine frame and the two modif control blocks its table kept alive stayed allocated
  after the pool itself was destroyed (the repro's counting operator-new ledger: 3 live blocks
  left, against 0 in the accepting control);
- `co_await co::sched(t, pack)` completed instantly with no error at all, because
  `sched_awaiter_t::await_suspend()` returns false unconditionally and its `await_resume()` was
  void - the refusal was invisible to the code that scheduled the task.

The refused CALL path does the cleanup right: `task<T>::await_suspend()` runs `do_exit_modifs()`
on a refused call and returns the caller, whose `await_resume()` then destroys the frame. The
sched path had no caller to lean on, and leaned on nothing - that asymmetry is the bug.

The same shape sits a second place: `_take_thread_tasks()` (the `COLIB_ENABLE_MULTITHREAD_SCHED`
take of what `thread_sched()` pushed) drops a refused task with the same silent
`if (...) == ERROR_OK` and the same leak. The bug repro covers only the first site; the fix
covers both.

One more thing makes this a contract decision and not just a missed `delete`: the documented
SCHED contract said "the sched doesn't report it: the modif that returned it owns the corutine
from there and must take care of it". No public API lets a modif take care of a refused task -
`state_t::self` is reachable but a raw `destroy()` skips the EXIT callbacks, which is exactly the
observed brokenness - so the old contract asked for something that cannot be done cleanly. This
proposal replaces that half of the contract; Open question 1 puts the replacement to you.

## Where the fix lands

As applied on 2026-10-29, in a shape the user chose over the proposal's: the SCHED slot became a
notification slot, like ENTER, LEAVE and EXIT. A schedule is neither refused nor parked, so the
leak has no path left and nothing needs reporting; `pool_t::sched` and `co::sched` keep their
`void`. Nothing parks or refuses at a schedule today: every SCHED callback in the header (the
killer's, the tracer's) and in the tests answers ERROR_OK, and the other copies of colib.h in the
workspace carry no SCHED callback in user code, so no caller changes.

| colib.h region                   | what changes                                             | adds/dels |
|----------------------------------|----------------------------------------------------------|-----------|
| `CO_MODIF_SCHED_CBK` doc comment | the answers are ignored; a schedule is neither refused   | +6/-7     |
|                                  | nor held, a task that must not run isn't scheduled       |           |
| `do_generic_modifs`              | `CO_MODIF_SCHED_CBK` joins the slots whose answers are   | +1/-1     |
|                                  | ignored, so one callback's error doesn't stop the others |           |
| `pool_internal_t::sched`         | the SCHED callbacks run, the task is queued; no early    | +2/-4     |
|                                  | return                                                   |           |
| `_take_thread_tasks`             | the same on the thread take                              | +2/-2     |

Total +11/-14. `docs/TODO.md`'s contract list said "a non-OK SCHED means not queued" and says
the new rule; `02_api.md` mirrors no enum doc, so it stays.

## The change

`diff -u` of the header right before this fix (2026-10-29, with fixes #11, #12, #14, #24, #32,
#33, #38, #39, #40, #48 and `wait_msg()` in) against the header as applied. The proposal's diff,
a refusal that destroys the task and returns its error (+33/-14), was not applied: the user ruled
that a schedule has nothing to refuse.

Reasoning per hunk:

- the doc comment: the contract is stated where the slot is declared, with the reason it holds.
  At a schedule nothing is suspended: the coroutine hasn't started and the scheduler doesn't
  wait, so a modif has nothing to hold (a park) and nobody to hand an error to (a refusal). The
  code that must keep a task from running has the task object and doesn't schedule it.
- `do_generic_modifs`: with the answers ignored, one callback's error must not stop the ones
  after it, as in the other notification slots; the list is where that rule lives.
- `sched()` and `_take_thread_tasks()`: the callbacks run for what they observe, then the task is
  queued. The leak had no other cause.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -741,13 +741,12 @@
     /*! This is called when a task is called (on the task), via 'co_await task' */
     CO_MODIF_CALL_CBK = 0,
 
-    /*! This is called on the coroutine that is scheduled, after the mods it inherits are added. On
-    any return other than ERROR_OK the coroutine is not queued and the sched doesn't report it: the
-    modif that returned it owns the coroutine from there and must take care of it. ERROR_SUSPENDED
-    is final: the callbacks after it still run, and an error from one of them doesn't take the
-    coroutine from the modif that suspended it. An error before any ERROR_SUSPENDED stops the
-    callbacks after it.
-    @date 2026-10-14 00:01 */
+    /*! This is called on the coroutine that is scheduled, after the mods it inherits are added.
+    What it returns is ignored: the coroutine is queued whatever the callbacks answer, and an error
+    from one of them doesn't stop the ones after it. A schedule is neither refused nor held here:
+    the coroutine hasn't started and nobody waits on it, so a task that must not run isn't
+    scheduled.
+    @date 2026-10-29 */
     CO_MODIF_SCHED_CBK,
 
     /*! This is called on a corutine right before it is destroyed. The return value of the callback
@@ -2842,7 +2841,7 @@
             cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
             cbk_id == CO_MODIF_ENTER_CBK || cbk_id == CO_MODIF_UNWAIT_IO_CBK ||
             cbk_id == CO_MODIF_UNWAIT_SEM_CBK || cbk_id == CO_MODIF_UNYIELD_CBK ||
-            cbk_id == CO_MODIF_RETURN_CBK;
+            cbk_id == CO_MODIF_RETURN_CBK || cbk_id == CO_MODIF_SCHED_CBK;
     /* the closing ones run in reverse, so they nest like destructors */
     constexpr bool reversed =
             cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
@@ -4219,10 +4218,8 @@
         add_modifs(pool, task, own_modifs);
         inherit_modifs(state, parent_table, CO_MODIF_INHERIT_ON_SCHED);
 
-        /* ... call the sched modifs */
-        if (do_sched_modifs(state) != ERROR_OK) {
-            return ;
-        }
+        /* ... call the sched modifs: notifications, their answers change nothing */
+        do_sched_modifs(state);
 
         /* third, we add the task to the pool */
         ready_tasks.push_back(state);
@@ -4438,8 +4435,8 @@
         /* by index: what a SCHED callback thread_sched()s is taken in the same pass */
         for (size_t i = 0; i < ready_thread_tasks.size(); i++) {
             state_t *s = ready_thread_tasks[i];
-            if (do_sched_modifs(external_init_task(s, pool)) == ERROR_OK)
-                ready_tasks.push_back(s);
+            do_sched_modifs(external_init_task(s, pool));   /* notifications, as in sched() */
+            ready_tasks.push_back(s);
         }
         ready_thread_tasks.clear();
         thread_pushed_new_tasks = false;
```

## The regression test

Landed as `tests/018-050-reproduced_sched_refusal_leak.cpp` (Test106) on 2026-10-29. The
proposal's test, a refused sched surfacing its error, was not landed: it pins the shape that was
not applied. This one pins the applied contract.

Four tasks, each with a pack of two SCHED callbacks and an EXIT callback, the first SCHED callback
answering ERROR_GENERIC (ERROR_SUSPENDED for one of them): one through `pool_t::sched`, two
through `co_await co::sched`, one through `thread_sched` (the file enables
`COLIB_ENABLE_MULTITHREAD_SCHED`, as `004-006` does). Every task must run, exit and be destroyed
(a local's destructor), and the second SCHED callback must run after the erroring one every time.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
order [rrrrs], ran 0, exits 0, destroyed 0
Failed CHK_BOOL(test106_ran == 4) err_str: The operation completed successfully.
[FAILED]: 018-050-reproduced_sched_refusal_leak.cpp
```

The one `s` is the callback after the ERROR_SUSPENDED one: a park let the later callbacks run
(fix #08's rule) while an error stopped them; none of the four tasks ran.

On the header as applied, two runs of two on Windows and one on Linux (WSL Ubuntu, g++ 15.2):

```
order [rsrsArsrsDBC], ran 4, exits 4, destroyed 4
[PASSED]: 018-050-reproduced_sched_refusal_leak.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <string>

/* Test106 - Reproduced Bugs: a SCHED callback's error dropped the task, leaking it
================================================================================================= */

/* pool_internal_t::sched() ran the SCHED callbacks of a task and, on any answer but ERROR_OK,
returned without queueing it: the task sat in no list of the pool, its EXIT callbacks never ran,
its frame leaked past the pool's death, and the code that scheduled it learned nothing (the sched
is void). The multithread take, _take_thread_tasks(), dropped a refused task the same way. The
doc asked the refusing modif to "take care of" the task, which no public API lets it do.

The fix, in the shape the user chose (fix #16, tests/fixes/FIX_16-sched_refusal_leak.md): the
SCHED slot is a notification slot, like ENTER, LEAVE and EXIT. What a SCHED callback returns is
ignored, the task is queued whatever the callbacks answer, and an error from one of them doesn't
stop the ones after it; there is no refusal and no park at a schedule, since nothing is suspended
there for a modif to hold. This file pins that: a task whose first SCHED callback answers
ERROR_GENERIC still gets its second SCHED callback, runs, exits and is destroyed, through
pool_t::sched(), through co_await co::sched() and through thread_sched(); and a SCHED callback
answering ERROR_SUSPENDED doesn't keep the task from running either. On the header before the fix
none of the four tasks ran. */

/* results hauled out of the coroutines: a failed ASSERT_COFN only logs (tests/CLAUDE.md) */
static std::string test106_order;           /* the callbacks, in the order they ran */
static int  test106_ran = 0;                /* tasks that ran their body */
static int  test106_dtor = 0;               /* frames destroyed (a local's destructor) */
static int  test106_exit = 0;               /* EXIT callbacks fired */

struct test106_marker_t { ~test106_marker_t() { test106_dtor++; } };

static co::task_t test106_task(char name) {
    test106_marker_t m;
    test106_ran++;
    test106_order += name;
    co_return 0;
}

/* a refusing SCHED callback, a second SCHED callback after it, and an EXIT callback */
static co::modif_pack_t test106_pack(co::error_e first_answer) {
    return co::modif_pack_t{
        co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [first_answer](co::state_t *) -> co::error_e {
                test106_order += 'r';   /* the one that answers an error (or a park) */
                return first_answer;
            }),
        co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test106_order += 's';   /* must still run after the error */
                return co::ERROR_OK;
            }),
        co::create_modif<co::CO_MODIF_EXIT_CBK>(co::CO_MODIF_INHERIT_NONE,
            [](co::state_t *) -> co::error_e {
                test106_exit++;
                return co::ERROR_OK;
            }),
    };
}

/* schedules through the awaitable, the second carrier of a sched */
static co::task_t test106_driver() {
    co_await co::sched(test106_task('B'), test106_pack(co::ERROR_GENERIC));
    co_await co::sched(test106_task('C'), test106_pack(co::ERROR_SUSPENDED));
    co_return 0;
}

int test106_sched_refusal_leak() {
    {
        auto pool = co::create_pool();
        pool->sched(test106_task('A'), test106_pack(co::ERROR_GENERIC));   /* pool_t::sched */
        pool->sched(test106_driver());                                      /* co::sched, twice */
        pool->thread_sched(co::add_modifs(pool.get(), test106_task('D'),
                test106_pack(co::ERROR_GENERIC)));                          /* the thread take */
        ASSERT_FN(pool->run());
    }
    DBG("order [%s], ran %d, exits %d, destroyed %d", test106_order.c_str(), test106_ran,
            test106_exit, test106_dtor);

    /* every task ran, exited and was destroyed, whatever its SCHED callbacks answered */
    ASSERT_FN(CHK_BOOL(test106_ran == 4));
    ASSERT_FN(CHK_BOOL(test106_exit == 4));
    ASSERT_FN(CHK_BOOL(test106_dtor == 4));

    /* the second SCHED callback ran after the erroring one, every time: four "rs" pairs */
    size_t pairs = 0;
    for (size_t at = test106_order.find("rs"); at != std::string::npos;
            at = test106_order.find("rs", at + 2))
        pairs++;
    ASSERT_FN(CHK_BOOL(pairs == 4));
    return 0;
}

int main() {
    int ret = test106_sched_refusal_leak();
    print_test_result("018-050-reproduced_sched_refusal_leak.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds ran with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. [-fsanitize=address]`
from `repro/fix/16/` (patched copy) and `repro/fix/16/base/` (pristine copy of the current
colib.h). The diff of the two copies is exactly the change above (`repro/fix/16/16-fix.diff`,
+33/-14).

1. The repro fails on the unpatched copy - fresh run, matching the recorded verdict
   (`base/16-base-run.log`, exit 1):
   ```
   B: pool->run() -> 0, sched_cbk=1 target_ran=0 exit_cbk=0 await_passed=1, live blocks left after pool destroyed: 3
     EXIT modif callback (user cleanup) fired: NO - never fired
     leaked live blocks still allocated: 3
   FAIL-REPRO: a SCHED-refused sched silently drops the task: ...
   ```
2. The repro passes on the patched copy, twice, and once under ASan (`16-fixed-run.log`,
   `16-fixed-run2.log`, `16-asan-run.log`; exit 0 each):
   ```
   B: pool->run() -> 0, sched_cbk=1 target_ran=0 exit_cbk=1 await_passed=1, live blocks left after pool destroyed: 0
     EXIT modif callback (user cleanup) fired: yes
     leaked live blocks still allocated: 0
   REFUTED: the refused task was cleaned up after all (exit_cbk=1 residual=0)
   ```
3. The regression test above, run against both copies (plain build):
   - base copy: FAILED - `Failed CHK_BOOL(g_exit_cnt == 1)` in `sched_refusal_refused()`
     (`base/018-032-base-run.log`); the `-DCOLIB_ENABLE_MULTITHREAD_SCHED=true` build fails the
     same assertion in `sched_refusal_thread()` too (`base/018-032-mt-base-run.log`)
   - patched copy: PASSED, exit 0, twice, once under ASan, and once with
     `-DCOLIB_ENABLE_MULTITHREAD_SCHED=true` (which compiles and runs the thread scenario:
     `018-032-patched-run.log`, `-run2.log`, `018-032-asan-run.log`, `018-032-mt-patched-run.log`)
4. No-regression, the two closest existing SCHED tests plus two more, against the patched copy
   (includes changed to `"colib.h"`, nothing else) - all PASSED, exit 0:
   - `011-001-modifs.cpp` (CALL and SCHED basic firing - the briefed candidate)
   - `011-004-modifs_inherit_on_sched.cpp` (ON_SCHED inheritance through `co_await co::sched` -
     the briefed candidate)
   - `011-003-modifs_lifecycle.cpp` (lifecycle orderings incl. sched)
   - `018-029-reproduced_winevent_handle_leak.cpp` (the applied fix #06's test; killer + sched)
5. Composition with fix #08: a header carrying both fixes (`repro/fix/16/colib-0816.h`) passes
   the regression test, passes the repro, and passes a dedicated probe
   (`repro/fix/16/16-08-compose.cpp`) that schedules a task whose SCHED vector is [parker,
   refuser]: `sched -> 2 (ERROR_SUSPENDED), exit_cnt=0`, the owner's single resume runs it, and
   nothing leaks. Against fix #16 alone the same probe shows the later error revoking the park
   (`sched -> -4, exit_cnt=1` - the parked task destroyed at the sched), which is why fix #08
   should land first or together (see Interactions).
6. NOT-CHECKED: no Linux/kqueue build on this box. Every changed region is common code outside
   any `#if COLIB_OS_*` guard, and the multithread region was compiled and run with
   `COLIB_ENABLE_MULTITHREAD_SCHED=true` here, so reading is the only validation left outstanding
   for the other backends.
7. README.md and docs/02_api.md mirror the old declarations (`void sched(...)` at
   docs/02_api.md:342, the co::sched return line at docs/02_api.md:692/700); they take the same
   return-type wording when the fix is applied.

At apply time, 2026-10-29, on the header of that day, in the applied shape (the SCHED answers
ignored):

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: any SCHED callback answering an error, on both carriers |
|                             | and on the thread take                                       |
| DESIGN.md                   | the killer's `on_sched` answers ERROR_OK and keeps doing so; |
|                             | no kill path changes; a refused task no longer exists for a  |
|                             | kill to miss                                                 |
| epoll / kqueue              | platform-independent; the test builds and passes on Linux    |
|                             | (WSL, g++ 15.2); kqueue NOT-CHECKED (WIP)                    |
| contract / docs             | the SCHED slot's doc comment rewritten; `02_api.md` mirrors  |
|                             | no enum doc; `docs/TODO.md`'s contract list updated          |
| callers                     | every SCHED callback in the header (the killer's, the        |
|                             | tracer's) and the tests answers ERROR_OK; the workspace's    |
|                             | other colib.h copies carry no SCHED callback in user code    |
| pending fixes               | #30 (type bounds) and #19 (external resume) sit elsewhere in |
|                             | the modifs engine; #08 (applied) is no longer load-bearing   |
|                             | at this slot                                                 |
| runs                        | the test fails before (no task ran) and passes after (2 of 2 |
|                             | on Windows, 1 on Linux); `011-001`, `011-004`, `004-006`,    |
|                             | `018-032`, `018-038`, `018-039`, `002-002` pass; the whole   |
|                             | suites: see the status line                                  |

## Interactions

The modifs-engine row (`repro/fix/00-FIXSTATUS.md`: 08 30 33 16 18 19), with 08 checked deep:

- #08 (an ERROR_SUSPENDED is final, in `do_generic_modifs`): no shared lines, and they compose -
  both fixes together validated (ladder step 5). But this fix makes 08's SCHED aggregate
  load-bearing: without 08, a SCHED vector [parker, refuser] aggregates the refuser's error, and
  this fix then destroys a task the parker owns - 08's bug shape, back at the sched (the probe in
  step 5 shows both outcomes). FIX_08's own interactions note ("no observable change in the SCHED
  case") was written against the old void sched and no longer holds once this lands. 08 was
  applied on 2026-10-14 (`tests/fixes/FIX_08-suspended_override.md`), so the order this asked
  for is met.
- #18 (killer rebind): different functions entirely (killer_t internals vs pool_internal_t::sched
  and sched_awaiter_t). The killer's own SCHED callback always returns ERROR_OK, so it never
  enters the refusal branch. Independent; any order.
- #19 (external resume skips ENTER): no shared code. Adjacent in one spot: this fix's regression
  test uses `external_sched_resume` for the parked scenario and keeps
  `COLIB_ENABLE_DEBUG_CHECKS` off for that reason - when 19 lands, this test could turn the checks
  on. FIX_08's test met the same problem and landed ending its park with `destroy_state()`
  instead, checks on; this test could do the same. Independent; any order.
- #30 (modif type OOB at attach): different functions (the attach path vs the sched path).
  Independent.
- #33 (one add_modifs call orders same-slot modifs by shared_ptr address): no line overlap, but
  the composition probe above depends on vector order and uses three separate add_modifs calls
  for exactly 33's reason. Landing 33 would make pack order reliable there; it does not affect
  this fix's own behavior.

## Open questions for the user

1. The SCHED refusal contract changes, and this fix picks the new one. The old comment said the
   refusing modif owns the refused coroutine and must take care of it; no public API lets a modif
   do that (a raw `state->self.destroy()` skips the EXIT callbacks - the exact observed loss), so
   the old promise could not be kept cleanly. The new contract: a refused task dies at the sched
   (its EXIT callbacks run, its frame is freed) and the error is returned to the scheduler. If
   any of your modifs already handle a refused sched themselves per the old wording, they would
   now double-handle a dead frame - say the word and the refusal branch goes back to silent-drop
   (the leak stays) or to some third shape you prefer.
2. The error surface is an API change, flagged per the briefing: `pool_t::sched` returns
   `error_e` (was void) and `co_await co::sched(t, v)` resolves to `error_e` (was void). Existing
   callers compile unchanged (a discarded value); new callers can read the outcome. The
   alternatives considered: keep both void and fix only the leak (most conservative, but the
   refusal stays invisible - half the confirmed bug); or carry the error on the task's
   `get_err()` (weak - the sched call moves the task away, so the caller usually holds nothing to
   ask). If you prefer a different surface, the `pool_internal_t::sched` core stands unchanged -
   only the two carriers move.
3. A refusal on the `thread_sched` path now cleans up (EXIT runs, frame freed) but surfaces
   nowhere: the take runs inside `run()` on the pool's thread, long after the pushing thread
   returned from `thread_sched()`. A debug-log line on the swallowed error is one `COLIB_DEBUG`
   away if you want it visible; it is silent now, like `thread_sched` itself.
4. `ERROR_SUSPENDED` now flows out of `sched` as a positive return (the task was parked, the
   modif owns it). That is informative rather than behavioral - no caller treats it as failure -
   but it is part of the return contract in question 2, so it rides the same ruling.

Decided at apply time, 2026-10-29, on the user's word ("if nothing breaks, do 3, I don't really
know why we would want to stop a sched"): (1) the refusal contract goes with the refusal: a
schedule is neither refused nor held, the SCHED answers are ignored; (2) no API change, `sched`
stays void on both carriers; (3) the thread take queues too, nothing to surface; (4) no
ERROR_SUSPENDED flows out of a sched. The proposal's shape, die at the sched and return the error,
was not applied.
