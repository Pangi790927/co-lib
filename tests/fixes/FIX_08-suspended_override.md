# Proposal: an ERROR_SUSPENDED is final (fix #08)

Status: [APPLIED 2026-10-14 on the user's word]

Applied with the two code lines first proposed; the comments that state the rule were rewritten at
apply time, since the user asked for the documentation to be fixed with it (see "The change"). The
regression test landed as `tests/018-039-reproduced_suspended_override.cpp` (Test94), its owner
ending the park with `destroy_state()` so that the debug checks stay on. Verified at apply time
(step 7 of "How it was checked"): the landed test fails on the header before the fix and passes
after it; the whole suite passes on Windows (94/94) and on Linux (93/93, `018-029` being
Windows-only); ASan is clean on every `001-`, `002-`, `011-` and `018-` test.

Follow-up, 2026-10-14, on the user's word (open question 2): the dropped error is logged. The loop
now reads:

```cpp
            if (ret < 0 && !ignored_ret) {
                if (result != ERROR_SUSPENDED)
                    return ret;
                /* an earlier callback suspended it: the error can't take it back */
                COLIB_DEBUG("WARNING: dropped error[%s] of modif cbk[%d] on %s: an earlier cbk "
                        "suspended it", dbg_enum(ret).c_str(), (int)cbk_id,
                        dbg_name(state->self).c_str());
            }
```

It prints only in the mixed case (in `018-039`, the [A, B] case: `dropped error[ERROR_USER] of
modif cbk[7]`) and does nothing when `COLIB_ENABLE_LOGGING` is false; the suite passes with it on
Windows and on Linux.

## The bug

`do_generic_modifs` runs the callbacks of one modif slot in vector order and aggregates their
returns: a positive `ERROR_SUSPENDED` is remembered in `result`, but the loop returns immediately on
the first negative return. The comment above it stated the rule as "An error wins (the wait doesn't
happen), then ERROR_SUSPENDED, then ERROR_OK" — right for an error that arrives before any
suspension, wrong once a suspension has already happened: the loop's `return ret` on a later
negative throws away the `ERROR_SUSPENDED` accumulated from an earlier callback, so a later error
outranks an ownership transfer that already took place.

The trigger is two same-slot modifs where the earlier one parks and the later one errors. Concretely
(confirmed, `repro/08-suspended_override.md`, REPRODUCED, ASan + behavioral, deterministic 2/2): a
coroutine with two WAIT_SEM modifs [A, B] does `co_await sem->wait()`. A's callback returns
`ERROR_SUSPENDED`, which by contract means A parked the coroutine and owns its resume. B's callback
then returns `ERROR_USER`. `do_generic_modifs` hits its negative short-circuit on B and hands
`ERROR_USER` to `sem_awaiter_t::await_suspend`, which takes the refusal branch — erase the waiter,
unwind the wait, ENTER, `return to_suspend` — and the engine resumes the coroutine on the spot, in
the same `run()` pass, although A parked it. The body runs to completion and frees its frame (a root
destroys itself at its final suspend). A, honoring its ownership, then calls
`external_sched_resume` on the state it captured: heap-use-after-free reading `state->pool`
(ASan abort, `repro/fix/08/08-base-run.log` has the fresh run). The control phase of the repro
(A alone) proves the mechanism is otherwise sound: the park holds and A's single resume is clean —
only B's later negative return breaks it. The same shape exists on every non-ignored callback
family: WAIT_IO, YIELD (both awaiters), CALL (the callee is killed out from under the owner), and
SCHED (no observable change there — both values mean "not queued").

Two users of the same loop came after this was first written. The killer: while `drive()` runs a
target whose wait already did its work on to its next wait (DESIGN.md rule 3), the killer's WAIT_*
callback parks the target there with `ERROR_SUSPENDED`, and a later modif's error overrides that
park, so the kill runs the target past the wait it parked it at and ends it at a later one
(confirmed by a probe, see "How it was checked"). And RECLAIM (`exec_reclaim_modifs()`): there an
error and `ERROR_SUSPENDED` both mean "kept", so its outcome is the same with or without the fix.

## Where the fix lands

| colib.h region                    | what changes                                     | adds/dels |
|-----------------------------------|--------------------------------------------------|-----------|
| `CO_MODIF_SCHED_CBK`'s doc        | an error stops the callbacks after it only       | +7 -4     |
|                                   | before any ERROR_SUSPENDED, and an error after   |           |
|                                   | one doesn't make its modif an owner              |           |
| `CO_MODIF_WAIT_IO_CBK`'s doc      | "a negative return aborts the wait" gains the    | +6 -3     |
|                                   | exception, and ERROR_SUSPENDED is described      |           |
| comment above `do_generic_modifs` | states which answer a pass gives, and why an     | +5 -2     |
|                                   | ERROR_SUSPENDED is final                         |           |
| `do_generic_modifs` callback loop | the negative short-circuit gains                 | +1 -1     |
|                                   | `&& result != ERROR_SUSPENDED`                   |           |

One function, and the two public docs that state the rule (WAIT_SEM's and YIELD's point at
WAIT_IO's). Every `do_*_modifs` variant dispatches through this template, so all six
error-sensitive families (SCHED, CALL, WAIT_IO, WAIT_SEM, YIELD, RECLAIM) are covered by the same
change; the ignored-return families (LEAVE/ENTER/EXIT/UNWAIT_*/UNYIELD/RETURN) never take the
short-circuit and are untouched.

## The change

Applied 2026-10-14. The two code lines are the ones proposed; the three comments were rewritten
at apply time from the facts they held, as the user asked for the documentation to be fixed with
the fix, and carry the date of that rewrite.

```diff
--- colib.h (before)
+++ colib.h (applied)
@@ -738,10 +738,13 @@
     /*! This is called when a task is called (on the task), via 'co_await task' */
     CO_MODIF_CALL_CBK = 0,
 
-    /*! This is called on the corutine that is scheduled. Other mods are inherited before this is
-    called. On any return other than ERROR_OK the corutine is not queued and the sched doesn't
-    report it: the modif that returned it owns the corutine from there and must take care of it.
-    With ERROR_SUSPENDED all the SCHED callbacks still run; an error stops the ones after it. */
+    /*! This is called on the coroutine that is scheduled, after the mods it inherits are added. On
+    any return other than ERROR_OK the coroutine is not queued and the sched doesn't report it: the
+    modif that returned it owns the coroutine from there and must take care of it. ERROR_SUSPENDED
+    is final: the callbacks after it still run, and an error from one of them doesn't take the
+    coroutine from the modif that suspended it. An error before any ERROR_SUSPENDED stops the
+    callbacks after it.
+    @date 2026-10-14 00:01 */
     CO_MODIF_SCHED_CBK,
 
     /*! This is called on a corutine right before it is destroyed. The return value of the callback
@@ -754,9 +757,12 @@
     /*! This is called on a resume. The return value of the callback is ignored */
     CO_MODIF_ENTER_CBK,
 
-    /*! This is called when a corutine is waiting for an IO (after the leave cbk). If the return
-    value is negative, then the wait is aborted. A negative value is returned by the wait at
-    once: code that retries the wait without handling that error spins. */
+    /*! This is called when a coroutine is waiting for an IO, after its LEAVE callbacks. A negative
+    return aborts the wait, which returns that value at once: code that retries the wait without
+    handling that error spins. ERROR_SUSPENDED parks the coroutine instead, and is final: the
+    modif that returned it owns the coroutine, the callbacks after it still run, and a negative
+    return from one of them no longer aborts the wait.
+    @date 2026-10-14 00:01 */
     CO_MODIF_WAIT_IO_CBK,
 
     /*! This is called when the io is done and the corutine that awaited it is resumed (before the
@@ -2796,8 +2802,11 @@
         state->modif_table = new_table;   
 }
 
-/* An error wins (the wait doesn't happen), then ERROR_SUSPENDED, then ERROR_OK. A callback can't
-throw: a throw out of one ends the program */
+/* Runs the callbacks of one modif slot in order and answers for all of them: the first error that
+comes before any ERROR_SUSPENDED (the event is refused), else ERROR_SUSPENDED, else ERROR_OK. An
+ERROR_SUSPENDED is final: the modif that returned it owns the coroutine, so a later error can't
+take it back, and the callbacks after it still run; the ones after an error don't. A callback
+can't throw: a throw out of one ends the program. 2026-10-14 00:01 */
 template <modif_e cbk_id, typename ...Args>
 inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
     /* the return value of those is ignored, so one of them can't stop the others */
@@ -2823,7 +2832,7 @@
             catch (...) {
                 std::terminate();   /* a callback must not throw */
             }
-            if (ret < 0 && !ignored_ret)
+            if (ret < 0 && !ignored_ret && result != ERROR_SUSPENDED)
                 return ret;
             if (ret == ERROR_SUSPENDED)
                 result = ERROR_SUSPENDED;
```

The first two hunks are the public docs. The SCHED callback's doc said an error stops the
callbacks after it; after the fix that holds only for an error before any `ERROR_SUSPENDED`, and an
error after one doesn't make its modif an owner. The WAIT_IO callback's doc said a negative return
aborts the wait, which now has the same exception; it also says what `ERROR_SUSPENDED` does there.
WAIT_SEM and YIELD are documented as "similar to wait_io", so that doc covers them.

The third hunk states the rule where the whole engine implements it. The old comment ranked the
outcomes ("an error wins, then ERROR_SUSPENDED, then ERROR_OK"), which the fix makes untrue; the
new one says which answer a pass gives, why an `ERROR_SUSPENDED` is final, and which callbacks
still run after each outcome.

The fourth hunk is the one functional change. `result != ERROR_SUSPENDED` makes a negative return
stop the pass only while no suspension has been handed over; once one has, the loop keeps running
the remaining callbacks and the aggregate stays `ERROR_SUSPENDED`, so the awaiters take their park
branch instead of their refusal branch. In every case without the mix (no suspension, or no later
error) the execution and the return value are exactly what they were. In the mixed case two things
change, both intended: the aggregate becomes the suspension, and the callbacks after the erroring
one also run — they were already promised that by the all-run-after-SUSPENDED rule.

## The regression test

Landed as `tests/018-039-reproduced_suspended_override.cpp` (Test94); it was first written as
018-029 / Test83, and the re-check of step 6 ran it as 018-040 / Test95. It is the repro reshaped
to the tests/ conventions, with one case added that pins the half of the rule that stays. At apply
time its owner was changed from resuming the parked coroutine with `external_sched_resume()` to
ending it with `destroy_state()`, as `011-006`'s owner does: that resume path skips ENTER (FIX_19,
still open), which had made the first version keep the debug checks off; the landed one keeps them
on.

Before the fix it fails cleanly: the first case's `test94_after_wait == 0` fails (`order: AB
after_wait: 1` - both callbacks ran, A parked, yet the engine resumed the body past the parked
wait), and it returns before the parked pointer is touched. After the fix it passes unchanged.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test94 - Reproduced Bugs: a later WAIT_SEM error revoked an earlier ERROR_SUSPENDED park
================================================================================================= */

/* Was an open bug (repro/08): do_generic_modifs returned on the first negative callback return,
so a WAIT_SEM callback that had already returned ERROR_SUSPENDED (it parked the coroutine and owns
it) was overridden by a later WAIT_SEM callback returning an error. sem_awaiter_t::await_suspend
then took its refusal path and resumed the coroutine at once, although a modif had parked it and
still owned it: the owner's own resume then reached a freed frame (repro/08-suspended_override.md,
a heap-use-after-free in external_sched_resume). Now an ERROR_SUSPENDED is final: a later error
can't take the coroutine back from the modif that parked it, the callbacks after an
ERROR_SUSPENDED still run and the ones after an error don't. An error that comes before any
ERROR_SUSPENDED still refuses the wait, as before.

The owner ends the park with destroy_state(), as 011-006's owner does, so the debug checks stay on
and check the leave/enter pairing of both flows too.

The parker and the refuser are attached with two separate add_modifs() calls on purpose: one call
would order them inside a std::set<modif_p> by shared_ptr address, not pack order, and the vector
order decides which return the aggregate sees first. */

static std::string test94_order;
static co::state_t *test94_parked = nullptr;
static int test94_after_wait = 0;
static int test94_destructed = 0;

struct test94_marker_t {
    ~test94_marker_t() { test94_destructed++; }
};

static co::task_t test94_victim(co::sem_p sem) {
    test94_marker_t marker;
    co_await sem->wait();
    test94_after_wait++;        /* runs once per resume that gets past the wait */
    co_return 0;
}

static co::modif_p test94_parker() {
    return co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t *s, co::sem_t *) -> co::error_e {
            test94_order += "A";
            test94_parked = s;  /* the modif owns s from its return on */
            return co::ERROR_SUSPENDED;
        });
}

static co::modif_p test94_refuser() {
    return co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t *, co::sem_t *) -> co::error_e {
            test94_order += "B";
            return co::ERROR_USER;
        });
}

static void test94_reset() {
    test94_order.clear();
    test94_parked = nullptr;
    test94_after_wait = 0;
    test94_destructed = 0;
}

/* [A, B]: A parks, B's error must not revoke the park. The owner then ends the coroutine, and
nothing ever runs it past the wait. */
static int test94_park_survives_later_error() {
    test94_reset();

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    co::modif_pack_t pack_a;
    pack_a.push_back(test94_parker());
    co::modif_pack_t pack_b;
    pack_b.push_back(test94_refuser());

    /* two add_modifs calls, so the WAIT_SEM vector order is [A, B] */
    auto parked = co::add_modifs(pool.get(), test94_victim(sem), pack_a);
    pool->sched(co::add_modifs(pool.get(), parked, pack_b));

    ASSERT_FN(pool->run());
    DBG("order: %s after_wait: %d", test94_order.c_str(), test94_after_wait);
    ASSERT_FN(CHK_BOOL(test94_order == "AB"));       /* B ran, after the park */
    ASSERT_FN(CHK_BOOL(test94_after_wait == 0));     /* the park held, no engine resume */
    ASSERT_FN(CHK_PTR(test94_parked));
    ASSERT_FN(CHK_BOOL(test94_parked->get_state() == co::STATE_LEFT));   /* in no queue */

    co::destroy_state(test94_parked);                /* the owner ends it, as 011-006's does */
    ASSERT_FN(CHK_BOOL(test94_destructed == 1));     /* its frame is gone, once */
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test94_after_wait == 0));     /* nothing ran it past the wait */
    return 0;
}

/* [B, A]: an error before any suspension still refuses the wait. A never runs, nobody parks, and
the coroutine continues past the refused wait right away - the half of the rule that stays. */
static int test94_error_before_park_refuses() {
    test94_reset();

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    co::modif_pack_t pack_b;
    pack_b.push_back(test94_refuser());
    co::modif_pack_t pack_a;
    pack_a.push_back(test94_parker());

    auto refused = co::add_modifs(pool.get(), test94_victim(sem), pack_b);
    pool->sched(co::add_modifs(pool.get(), refused, pack_a));

    ASSERT_FN(pool->run());
    DBG("order: %s after_wait: %d", test94_order.c_str(), test94_after_wait);
    ASSERT_FN(CHK_BOOL(test94_order == "B"));        /* the error stopped A, nothing parked */
    ASSERT_FN(CHK_BOOL(test94_parked == nullptr));
    ASSERT_FN(CHK_BOOL(test94_after_wait == 1));     /* refused: it ran past the wait at once */
    ASSERT_FN(CHK_BOOL(test94_destructed == 1));     /* and on to its end */
    return 0;
}

int main() {
    int ret = test94_park_survives_later_error();
    ret |= test94_error_before_park_refuses();
    print_test_result("018-039-reproduced_suspended_override.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds ran in `repro/fix/08/` (patched copy) and `repro/fix/08/base/` (pristine copy of the
current colib.h), with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. [-fsanitize=address]`,
per the fix spec. The diff of base vs patched copy was then the two `do_generic_modifs` hunks
(`repro/fix/08/08-fix.diff`, +4/-2 lines); steps 1-4 are that first check, step 6 the re-check.

1. Repro fails on the unpatched copy — fresh run, matching the recorded verdict. Control phase ok
   (park holds, exactly one owner resume); bug phase:
   ```
   [A] WAIT_SEM: parking state ...3D0, returning ERROR_SUSPENDED
   [B] WAIT_SEM: returning ERROR_USER
   [...colib.h:4831 await_suspend() :> User stopped wait on semaphore: state[...3D0] sem[...0980]]
   FAIL-REPRO (behavioral): A parked the coroutine, yet the engine resumed it and the
       body ran past the parked wait (after_wait=1)
   ==...==ERROR: AddressSanitizer: heap-use-after-free ... in colib::external_sched_resume
   ==...==ABORTING
   EXIT: 1
   ```
   (`repro/fix/08/08-base-run.log`; the engine's refusal trace line, "User stopped wait on
   semaphore", is the exact wrong step.)

2. Repro passes on the patched copy, twice (exit 0, ASan clean). B still runs after A ("[A] ... then
   [B] ..."), the awaiter takes the park branch (no refusal line, UNSEM unwinds the wait), the first
   `run()` leaves `after_wait=0`, and the owner's single resume runs the body exactly once:
   ```
   [bug] run()=0 started=1 after_wait=0 parked=00001225384A03D0
   [bug] second run()=0 after_wait=1
   PASS: B's error could not revoke A's park; the owner's single resume ran it once
   ```
   (`repro/fix/08/08-fixed-run.log`, `08-fixed-run2.log`)

3. The regression test above, run against both copies (plain build):
   - base copy: FAILED — `Failed CHK_BOOL(test83_after_wait == 0)` with
     `order: AB after_wait: 1` (`repro/fix/08/base/018-029-base-run.log`)
   - patched copy: PASSED, exit 0, twice, and once under ASan —
     `order: AB after_wait: 0` then `[PASSED]: 018-029-reproduced_suspended_override.cpp`
     (`repro/fix/08/018-029-patched-run.log`, `-run2.log`, `018-029-asan-run.log`)

4. No-regression, three closest existing tests against the patched copy (include changed to
   `"colib.h"`, nothing else): all PASSED, exit 0 —
   - `011-006-modifs_force_suspend.cpp` (ERROR_SUSPENDED parks, LEAVE/WAIT/UNWAIT order, refused
     yield — the park contract itself)
   - `011-008-modifs_yield_pair.cpp` (YIELD/UNYIELD pair, refused yield, kill-closed)
   - `001-004-semaphore_multiple_waiters.cpp` (plain multi-waiter semaphores)

5. Linux was NOT-CHECKED at first (no make on this box then); it is checked since, see 6. kqueue
   stays NOT-CHECKED: the changed region is a platform-independent template, and every awaiter
   reaches it through the same `do_*_modifs` wrappers.

6. Re-checked 2026-10-13 on the header of that day (FIX_02, 43, 44, 45 and 47 applied since), with
   the rebased diff above. That header also had FIX_03's five lines and its test, removed the same
   day; those lines acted only when a kill's `stop_io()` failed, which no other test reaches, so
   the counts below include one test that is gone:
   - the regression test, plain build: the current header FAILS (`order: AB after_wait: 1`), the
     patched one PASSES (`order: AB after_wait: 0`, then `order: B after_wait: 1`);
   - the whole suite on Windows, plain build: `[PASSED: 95]/[FAILED: 0]` with the fix (the 94
     tests plus this one);
   - the whole suite on Linux (WSL Ubuntu, g++ 15.2, epoll; `018-029` left out, its watchdog is
     Windows-only): the current header `[PASSED: 93]/[FAILED: 1]`, the one failure being this
     test, and the patched one `[PASSED: 94]/[FAILED: 0]`;
   - ASan (MSVC), patched header: every `001-`, `002-`, `011-` and `018-` test (71 files, this one
     included) exits 0 with no ASan report;
   - a scratch probe beyond the regression test, ASan build, both headers clean: a [parker,
     refuser] vector on YIELD, on WAIT_IO (a `sleep_ms`) and on CALL (inherited by the callee) —
     on the current header the park is overridden in all three, patched it holds and the owner's
     single resume runs the coroutine once; and a killer that drives its target into a second
     semaphore wait a later modif refuses — on the current header the kill runs the target past
     that wait (`step2=1`), patched it ends it there (`step2=0`), the kill answering `ERROR_OK`
     both times;
   - `patch --dry-run` of every other waiting FIX diff answers the same with and without this
     fix (the ones that no longer apply were already stale before it);
   - the WAIT_IO doc hunk was added after these runs; it changes a comment only, and the
     regression test was rebuilt with it and passed.

7. At apply time (2026-10-14), on the repo's own header and tests:
   - the landed test on the header before the fix: FAILED, the first case by its checks
     (`order: AB after_wait: 1`), the second passing; after it: PASSED, 3 runs of 3 (plain);
   - the whole suite on Windows, plain build: `[PASSED: 94]/[FAILED: 0]`;
   - the whole suite on Linux (WSL Ubuntu, g++ 15.2, epoll; `018-029` left out, its watchdog
     is Windows-only): `[PASSED: 93]/[FAILED: 0]`;
   - ASan (MSVC), on a copy of the same header and tests: every `001-`, `002-`, `011-` and
     `018-` test (70 files) exits 0 with no ASan report, and the landed test 3 more runs of 3.

## Interactions

The modifs-engine row (`repro/fix/00-FIXSTATUS.md`: 08 30 33 16 18 19), plus 07 as briefed and
the killer that landed since:

- #30 (modif type OOB at attach): different functions (`add_modifs_to_table` / `inherit_modifs`
  push_back sites). Independent — neither fix touches the other's lines; both can land in any
  order.
- #33 (one add_modifs call orders same-slot modifs by shared_ptr address): no line-level overlap,
  but this fix makes vector order semantically decisive — which callback returns first now decides
  ownership versus refusal. My fix does not need 33's (the repro, the regression test, and
  tests/011-007 all use two separate `add_modifs` calls, which append in call order today), and
  33's fix would compose cleanly: with pack order honored, "earlier in the pack" becomes a
  reliable notion for this rule. Worth landing 33 (or avoiding single-call mixed packs) for
  predictable ownership.
- #16 (sched refusal leak): today the sched callers treat any non-OK aggregate the same (not
  queued), so this fix changes nothing observable at SCHED. #16 changes that: it destroys a
  refused task, so without this fix a SCHED vector [parker, refuser] would have #16 destroy a task
  the parker owns (FIX_16's own composition probe shows it). Land this fix first, or together
  with #16.
- #18 (killer rebind) was superseded by the applied FIX_45. #19 (external enter skipped): no
  shared code with `do_generic_modifs`. The first version of the regression test resumed the
  parked coroutine through `external_sched_resume`, which skips ENTER, and so kept the debug
  checks off; the landed test ends the park with `destroy_state()` and keeps them on, so it no
  longer waits on #19.
- #07 (a woken waiter holds its semaphore) was declined by the user on 2026-10-13; nothing of it
  is left to compose with.
- The killer (FIX_44/47, applied): its WAIT_IO, WAIT_SEM and YIELD callbacks return
  `ERROR_SUSPENDED` while `drive()` runs its target. With this fix that park is final, so the kill
  ends the target at the wait it parked it at, whatever a later modif answers, as DESIGN.md
  rule 3 says; a modif placed before the killer's still refuses the wait first, and the target
  then runs on to its next wait. The killer tests pass with the fix (see "How it was checked").

## Open questions for the user

1. The contract this fix states, flagged per the briefing: once a callback returns
   ERROR_SUSPENDED, the remaining callbacks of that slot still run, and the suspension is final —
   a later error cannot revoke it. I chose this because the public SCHED comment already promises
   exactly this shape for SCHED ("With ERROR_SUSPENDED all the SCHED callbacks still run; an error
   stops the ones after it"), and because short-circuiting on ERROR_SUSPENDED would contradict
   that documented behavior and hide the park from later callbacks that are promised to see the
   event. The fix generalizes the SCHED wording to every family; if you want the opposite rule for
   the wait/yield families (stop the pass at the park), the loop change is one guard away, but it
   would break the documented SCHED contract unless SCHED is special-cased.
2. In the mixed case the later error is dropped entirely: `do_generic_modifs` returns one
   `error_e`, so B's ERROR_USER (in the repro's terms) is unobservable — the coroutine parks and
   the error goes nowhere. A debug-log line for the swallowed error would fit the existing
   COLIB_DEBUG plumbing if you want it visible; I left it silent to keep the change minimal.
   Answered 2026-10-14: the user asked for the log line; it landed as the follow-up under Status.
3. The rule stays order-asymmetric on purpose: an error that comes before any suspension still
   wins and stops the callbacks after it (so a would-be parker later in the vector never runs,
   nothing parks, the wait is refused). That is the pre-fix behavior of the error short-circuit,
   kept. Note its dependency on #33: in one `add_modifs` call, "which callback is earlier" is heap
   address order today, not pack order.
