# Proposal: a killed future's task surfaces its kill (fix #24)

Status: [APPLIED 2026-10-29 on the user's word]

Apply verification, 2026-10-29, in the shape the user chose ((b), a pair; see "Where the fix
lands"): on the header before the fix a plain-value program shows the kill vanishing (`await gave
0, exception 0, get_err 0`); the test passes on the header as applied, two runs of two; the whole
suite on Windows `[PASSED: 103]/[FAILED: 0]`; on Linux (WSL Ubuntu, g++ 15.2, epoll)
`[PASSED: 102]/[FAILED: 0]`, every file but the Windows-only `018-029`. The check table is the
last item of "How it was checked".

## The bug

`create_future` installs one shared callback as both a RETURN and an EXIT modif on the wrapped task.
When the task co_returns, the RETURN firing stores the value; when it dies without returning, the
EXIT firing runs instead, from `destroy_state` -> `do_exit_modifs`. That EXIT path checks, in order:
an exception of the task's own, then a produced value (`promise().ret.index() == 1`), then — for a
task that has neither — it takes the `std::is_default_constructible_v<T>` branch and stores `T{}`,
marks the future's data ready and signals its semaphore. A task destroyed by a kill has neither an
exception nor a value (it never co_returned), so a killed task's future resolved to `T{}` with every
error channel clean: the awaited value was the default, the waiter task's `get_err()` was ERROR_OK
(the waiter coroutine co_returns a value, and that member is only set for a callee that ended
without one), and no exception was stored or rethrown.

The kill's error had no path to the waiter. `killer_state_t::end_chain` writes the kill's error into
the killed root's state only when the root has a caller (`if (root->caller_state) root->err = e;`) —
that is the documented killer contract for a *called* task, whose caller's `await_resume` copies
`state.err` into the awaiting `task`'s `err` member. A future-wrapped task is *scheduled*, per
create_future's own documented usage, so it has no caller: nobody wrote the error, nobody read it.
The waiter is a separate coroutine parked on the future's semaphore, not the victim's caller, so the
contract's escape hatch was structurally unavailable. Even the victim task object's own `get_err()`
stayed ERROR_OK.

The trigger (confirmed, `repro/24-future_kill_swallowed.md`, REPRODUCED, deterministic, 5/5 runs):
build a `task<int>` that parks on a never-signaled semaphore, attach a `create_killer(pool,
ERROR_USER)` pack to it, wrap it with `create_future`, `co_await sched(t)`, one `co_await yield()`,
then `kill_fn()`. The kill verifiably happened (`kill_fn()` returned ERROR_OK, the second call
ERROR_FINISHED, the victim's frame destroyed exactly once, its body past the wait never ran) — and
`co_await fut` handed back `0` with `get_err() == ERROR_OK` and no exception. `wait_all`, built
entirely on `create_future`, showed the same face: of `wait_all(a=7, b=9)` with `b` killed mid-wait,
the tuple came back `{7, 0}` and the co_await completed normally — the kill vanished from the
result. `create_timeo` faces the identical event (its timer kills the inner task before it returns)
and surfaces it deliberately as `{T{}, ERROR_TIMEO}`, because its inner task is *called*, so
`end_chain`'s caller path delivers the error; the future path dropped the information on the floor.

## Where the fix lands

As applied on 2026-10-29, in the shape the user chose over the proposal's (open question 1, (b)):
no exception type. A future resolves to a pair, the value and an error, as `create_timeo` does,
and `wait_all` to a tuple of those pairs, one per member (open question 2: per member, so the
healthy members' values are kept).

| colib.h region                 | what changes                                                |
|--------------------------------|-------------------------------------------------------------|
| `create_future` declaration    | `task<std::pair<T, error_e>>`; the doc states the three     |
| and doc comment                | outcomes, that a task's own exception is still rethrown,    |
|                                | and that T must be default constructible                    |
| `wait_all` declaration and doc | `task<std::tuple<std::pair<ret_v, error_e>...>>`; the claim |
| comment                        | "killing one kills all (sig_killer installed in all)" and   |
|                                | the TODO above it go: no such killer was ever installed     |
| `create_future`'s body         | a `static_assert` on T, an `error_e` in its data, the death |
|                                | branch stores T{} and the error (the kill's, or             |
|                                | ERROR_GENERIC when the state holds none), the waiter        |
|                                | returns the pair; the "did not return a value" exception    |
|                                | and the `is_default_constructible` branch go with it        |
| `wait_all`'s body              | builds and returns the tuple of pairs                       |
| `killer_state_t::end_chain`    | `root->err = e` unconditionally: a scheduled root's state   |
|                                | carries the kill's error too, for its EXIT modifs           |

+34/-28 in all, the doc comments included. The rule the pair follows: its error is ERROR_OK only
when the task returned; that also closes the proposal's open question 5, since a death that is no
kill reads ERROR_GENERIC whatever the state held. Every await of a future or of `wait_all` now
reads a pair: the four tests that do were updated with the fix (`007-001`, `008-001`, `018-004`,
`018-046`); `examples/` has no such await.

## The change

`diff -u` of the header right after fix #14 (2026-10-29, the state this fix was built on) against
the header as applied.

Reasoning per hunk:

- `create_future`'s doc and declaration: the contract is stated once, where the function is
  declared: value and ERROR_OK for a return, T{} and the kill's error for a kill, T{} and
  ERROR_GENERIC for any other death; a task's exception still rethrown; T default constructible,
  as `create_timeo` already asks of its T.
- `wait_all`'s doc and declaration: the tuple of pairs, and the end of the "killing one kills all"
  claim with the user's TODO that questioned it: the function installs no killer, a kill of one
  member leaves the others running, and each now says what became of it.
- `create_future`'s body: the `static_assert` says why T needs a default constructor; the data
  carries the error next to the value; the branch that took T{} for every death now records why
  it died, the kill's error when `end_chain` left one in the state, ERROR_GENERIC otherwise, so a
  death never reads as success; the waiter returns the pair. A yield never reaches that branch
  (fix #14's guard above), and a task's own exception still wins (its branch comes first).
- `wait_all`'s body: the same loop over futures, with the tuple's element type changed.
- `end_chain`: the enabler. The old guard wrote the kill's error only for a called root, whose
  caller reads it at its await; a scheduled root's state carried nothing while `destroy_state`
  fired its EXIT modifs, which is the future's case. Written unconditionally, a called root sees
  the same value at the same point, and a scheduled root's EXIT modifs see it too.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -1701,37 +1701,37 @@
 
 /*! @fn
  * Takes a task and adds the requred modifications to it such that the returned object will be
- * returned once the return value of the task is available.
- * 
+ * returned once the task has ended: with its return value and ERROR_OK when it returned, with T{}
+ * and the kill's error when a kill destroyed it before it returned, with T{} and ERROR_GENERIC
+ * when it died any other way. A co_yield of the task ends nothing. An exception the task threw is
+ * rethrown by the await. T must be default constructible.
+ *
  * @param pool The pool on which the task `t` will be scheduled.
  * @param t The task of the coroutine that will be scheduled.
- * @return The task of a coroutine that can be awaited to wait for the return value
+ * @return The task of a coroutine that can be awaited to wait for the task's end, resolving to
+ * the pair of its return value and the error above
  * Example:
  * ```cpp
  * 1: auto t = co_task();
  * 2: auto fut = colib::create_future(t)
  * 3: co_await colib::sched(t);
  * 4: // ...
- * 5: co_await fut; // returns the value of co_task once it has finished executing 
+ * 5: auto [val, err] = co_await fut; // the value of co_task once it has finished executing
  * ```
 */
 template <typename T>
-inline task<T> create_future(pool_t *pool, task<T> t); /* not awaitable */
+inline task<std::pair<T, error_e>> create_future(pool_t *pool, task<T> t); /* not awaitable */
 
-/* TODO: status:investigatory. The comment below says killing one task kills all, through a
-sig_killer installed in each, but the definition of wait_all may install no killer: it makes a
-future for each task, schedules the tasks and awaits the futures in order. To check: whether a kill
-of one task, or of wait_all's caller, reaches the others, and whether the comment or the code holds
-the intent. */
 /*! @fn
- * Wait for all the tasks to finish, the return value can be found in the respective task, killing
- * one kills all (sig_killer installed in all). The inheritance is the same as with 'call'.
- * 
+ * Waits for all the tasks to end, each through a create_future: a kill of one leaves the others
+ * running, and each reports for itself. The inheritance is the same as with 'call'.
+ *
  * @param tasks The tasks to be awaited
  * @return **Coroutine** that resolves to: The waiting of all the tasks, that **further**
- * results in their return values. */
+ * results in a tuple of their outcomes, in order, each the pair create_future resolves to: the
+ * return value and ERROR_OK, or T{} and the kill's error, or T{} and ERROR_GENERIC. */
 template <typename ...ret_v>
-inline task<std::tuple<ret_v>...> wait_all(task<ret_v>... tasks);
+inline task<std::tuple<std::pair<ret_v, error_e>...>> wait_all(task<ret_v>... tasks);
 
 /*!
  * Causes the running pool::run to stop, the function will stop at this point, can be resumed with
@@ -5054,17 +5054,18 @@
 ------------------------------------------------------------------------------------------------  */
 
 template <typename ...ret_v>
-inline task<std::tuple<ret_v...>> wait_all(task<ret_v>... tasks) {
+inline task<std::tuple<std::pair<ret_v, error_e>...>> wait_all(task<ret_v>... tasks) {
+    using outcomes_t = std::tuple<std::pair<ret_v, error_e>...>;
     auto pool = co_await get_pool();
 
-    std::tuple<task<ret_v>...> futures = {create_future(pool, tasks)...};
+    std::tuple<task<std::pair<ret_v, error_e>>...> futures = {create_future(pool, tasks)...};
 
     /* TODO: Maybe we want a COLIB_REGNAME here? This needs to be determined, either way, it
     would require the sched to be transformed into a task? */   
     (co_await sched(tasks), ...);
 
-    co_return co_await std::apply([](auto&&... futures) -> task<std::tuple<ret_v...>> {
-        co_return std::tuple<ret_v...>{co_await COLIB_REGNAME(futures)...};
+    co_return co_await std::apply([](auto&&... futures) -> task<outcomes_t> {
+        co_return outcomes_t{co_await COLIB_REGNAME(futures)...};
     }, futures);
 }
 
@@ -6439,11 +6440,14 @@
 }
 
 template <typename T>
-inline task<T> create_future(pool_t *pool, task<T> t) {
+inline task<std::pair<T, error_e>> create_future(pool_t *pool, task<T> t) {
+    static_assert(std::is_default_constructible_v<T>, "colib: a future's T must be default "
+            "constructible, it stands in for the value of a task that ended without one");
     // using data_t = std::pair<T, bool>;
     struct data_t {
         std::optional<T> ret;
         std::exception_ptr exc;
+        error_e err = ERROR_OK;     /* ERROR_OK only when the task returned */
         bool ready = false;
     };
 
@@ -6468,11 +6472,12 @@
         }
         else if (h.promise().ret.index() == 1)
             data->ret = std::get<T>(std::move(h.promise().ret));
-        else if constexpr (std::is_default_constructible_v<T>)
+        else {
+            /* died without a value: a kill left its error in the state; anything else (a refused
+            sched, the pool's teardown) left none and reads as ERROR_GENERIC */
             data->ret = T{};
-        else
-            data->exc = std::make_exception_ptr(
-                    std::runtime_error("colib: future task did not return a value"));
+            data->err = state->err != ERROR_OK ? state->err : ERROR_GENERIC;
+        }
         data->ready = true;
         sem->signal();
         return ERROR_OK;
@@ -6482,12 +6487,12 @@
             create_modif<CO_MODIF_RETURN_CBK>(CO_MODIF_INHERIT_NONE, exit_func),
             create_modif<CO_MODIF_EXIT_CBK>(CO_MODIF_INHERIT_NONE, exit_func)});
 
-    return [](sem_p sem, std::shared_ptr<data_t> data) -> task<T> {
+    return [](sem_p sem, std::shared_ptr<data_t> data) -> task<std::pair<T, error_e>> {
         if (!data->ready)
             co_await sem->wait();
         if (data->exc)
             std::rethrow_exception(data->exc);
-        co_return std::move(*data->ret);
+        co_return std::pair<T, error_e>{std::move(*data->ret), data->err};
     }(sem, data);
 }
 
@@ -6700,8 +6705,9 @@
 
     void end_chain() {
         killing = true;
-        if (root->caller_state)
-            root->err = e;
+        /* the killed root's state carries the error: its caller reads it at its await, a sched'd
+        root has no caller and its EXIT modifs read it (a future's does) */
+        root->err = e;
         queue_caller(root->pool->get_internal(), root->caller_state);
         close_wait(top);
         destroy_state(top, root);
```

## The regression test

Landed as `tests/018-047-reproduced_future_kill_swallowed.cpp` (Test103) on 2026-10-29; it was
first written as 018-030 / Test84 around the proposal's exception, and reshaped to the pair.

Three scenarios. The documented future usage with the wrapped task killed while parked on a
semaphore: the await must give `{T{}, ERROR_USER}`, throw nothing, and leave the waiter's
`get_err()` at ERROR_OK. `wait_all` with one of two members killed mid-wait by a controller: the
tuple must arrive, the healthy member as `{7, ERROR_OK}` and the killed one as `{0, ERROR_USER}`.
A control with nothing killed: `{42, ERROR_OK}` and a tuple of `{7, ERROR_OK}`, `{9, ERROR_OK}`.
Every kill-happened guard of the original repro is kept: the first kill answers ERROR_OK, the
second ERROR_FINISHED, the frame is destroyed exactly once, the body past the wait never runs.

The file reads the pair, so it compiles only with the fix in. On the header before it (right
after fix #14, a copy kept aside at apply time), a program that reads the old plain value shows the
kill vanishing (`before24.cpp`, in the apply workspace):

```
kill 0 then 3, destroyed 1, ran past 0; await gave 0, exception 0, get_err 0
[FAILED]: before24.cpp (the kill must surface)
```

On the header as applied, two runs of two:

```
s1: value 0 err -4 exc 0 waiter err 0; s2: tuple 1 a {7, 0} b {0, -4} exc 0; s3: {42, 0} tuple ok 1
[PASSED]: 018-047-reproduced_future_kill_swallowed.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test103 - Reproduced Bugs: a killed create_future task reported T{} and ERROR_OK, as a success
================================================================================================= */

/* Was an open bug in create_future's exit_func: when a killer destroyed the wrapped task before it
returned, the EXIT callback found no exception and no value (promise().ret still monostate) and took
the default-construct branch: it stored T{}, marked the data ready and signaled. The waiter then
returned that T{} with every error channel clean: the awaited value was the default, the waiter
task's get_err() was ERROR_OK, no exception was thrown. wait_all, built on create_future, showed the
same face: a member killed mid-wait came back as T{} inside the result tuple and the whole co_await
completed as if nothing had died. The kill's error could not reach the waiter by any path: the
killer's end_chain wrote the error into the root's state only when a caller existed, and a scheduled
task has none. Now fixed (fix #24, in the shape the user chose): end_chain always writes the kill's
error into the killed root's state, and a future resolves to a pair, the value and an error: the
return value and ERROR_OK, or T{} and the kill's error, or T{} and ERROR_GENERIC for a death that
was no kill. wait_all resolves to a tuple of those pairs, one per member: a killed member reports
its kill and the others their values.

This file reads the pair, so it compiles only with the fix in place; against the unfixed colib.h a
program that reads the plain value shows the kill vanishing (the fix's record has that run). */

static int  test103_s1_kill_ret  = -999;  /* kill_fn() first call: ERROR_OK, it killed            */
static int  test103_s1_kill2_ret = -999;  /* second call: ERROR_FINISHED, nothing left            */
static int  test103_s1_dtor      = 0;     /* victim frame destroyed: exactly once                 */
static bool test103_s1_ran_past  = false; /* resumed past its wait: must stay false               */
static int  test103_s1_value     = -1;    /* the pair's value: T{}, the task never returned       */
static int  test103_s1_err       = -999;  /* the pair's error: the kill's                         */
static bool test103_s1_exc       = false; /* an exception at the await: must not happen           */
static int  test103_s1_fut_err   = -999;  /* waiter get_err(): stays ERROR_OK, the pair carries it */

static int  test103_s2_kill_ret  = -999;
static int  test103_s2_dtor      = 0;
static bool test103_s2_ran_past  = false;
static bool test103_s2_tuple     = false; /* wait_all delivered its tuple: it must                */
static int  test103_s2_a_value   = -1;    /* the healthy member's pair                            */
static int  test103_s2_a_err     = -999;
static int  test103_s2_b_value   = -1;    /* the killed member's pair                             */
static int  test103_s2_b_err     = -999;
static bool test103_s2_exc       = false;

static int  test103_s3_value     = -999;  /* control: a healthy future still returns its value    */
static int  test103_s3_err       = -999;
static bool test103_s3_tuple_ok  = false; /* control: a healthy wait_all still returns its tuple  */

struct test103_s1_marker_t { ~test103_s1_marker_t() { test103_s1_dtor++; } };
struct test103_s2_marker_t { ~test103_s2_marker_t() { test103_s2_dtor++; } };

/* Scenario 1: the documented create_future usage, the wrapped task killed while parked on a
semaphore that never signals. Every channel of the waiter is recorded. */
static co::task<int> test103_s1_victim(co::sem_p never) {
    test103_s1_marker_t m;
    co_await never->wait();
    test103_s1_ran_past = true;     /* killed means destroyed, never resumed */
    co_return 42;
}

static co::task_t test103_s1_driver(co::sem_p never) {
    auto pool = co_await co::get_pool();
    auto [killer_mods, kill_fn] = co::create_killer(pool, co::ERROR_USER);

    auto t = co::add_modifs(pool, test103_s1_victim(never), killer_mods);
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    co_await co::yield();          /* let the victim run first and park on the sem */

    test103_s1_kill_ret = kill_fn();
    test103_s1_kill2_ret = kill_fn();

    try {
        auto [value, err] = co_await fut;
        test103_s1_value = value;
        test103_s1_err = err;
    } catch (const std::exception&) {
        test103_s1_exc = true;
    }

    test103_s1_fut_err = fut.get_err();
    co_return 0;
}

/* Scenario 2: wait_all with one member killed mid-wait by a controller (the tests/002-002 shape).
The tuple must arrive, the killed member reporting its kill and the healthy one its value. */
static co::task<int> test103_s2_member_a() {
    co_return 7;
}

static co::task<int> test103_s2_member_b(co::sem_p never) {
    test103_s2_marker_t m;
    co_await never->wait();
    test103_s2_ran_past = true;     /* must not run */
    co_return 9;
}

static co::task_t test103_s2_controller(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();          /* let wait_all sched both members and let b park */
    test103_s2_kill_ret = kill_fn();
    co_return 0;
}

static co::task_t test103_s2_driver(
        co::pool_t *pool, co::sem_p never, const co::modif_pack_t& killer_mods) {
    auto b = co::add_modifs(pool, test103_s2_member_b(never), killer_mods);
    try {
        auto tup = co_await co::wait_all(test103_s2_member_a(), b);
        test103_s2_tuple = true;
        test103_s2_a_value = std::get<0>(tup).first;
        test103_s2_a_err = std::get<0>(tup).second;
        test103_s2_b_value = std::get<1>(tup).first;
        test103_s2_b_err = std::get<1>(tup).second;
    } catch (const std::exception&) {
        test103_s2_exc = true;
    }
    co_return 0;
}

/* Scenario 3, control: nothing killed. A future still hands back the real co_return value with
ERROR_OK and a healthy wait_all still delivers its tuple: the kill path added no false positives. */
static co::task<int> test103_s3_worker() {
    co_return 42;
}

static co::task_t test103_s3_driver() {
    auto pool = co_await co::get_pool();
    auto t = test103_s3_worker();
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    auto [value, err] = co_await fut;
    test103_s3_value = value;
    test103_s3_err = err;

    auto tup = co_await co::wait_all(
            []() -> co::task<int> { co_return 7; }(),
            []() -> co::task<int> { co_return 9; }());
    test103_s3_tuple_ok = std::get<0>(tup) == std::pair<int, co::error_e>{7, co::ERROR_OK}
            && std::get<1>(tup) == std::pair<int, co::error_e>{9, co::ERROR_OK};
    co_return 0;
}

int test103_future_kill_surface() {
    {
        auto pool = co::create_pool();
        auto never = co::create_sem(pool, 0);
        pool->sched(test103_s1_driver(never));
        ASSERT_FN(pool->run());
    }
    {
        auto pool = co::create_pool();
        auto never = co::create_sem(pool, 0);
        auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(test103_s2_driver(pool.get(), never, killer_mods));
        pool->sched(test103_s2_controller(kill_fn));
        ASSERT_FN(pool->run());
    }
    {
        auto pool = co::create_pool();
        pool->sched(test103_s3_driver());
        ASSERT_FN(pool->run());
    }

    DBG("s1: value %d err %d exc %d waiter err %d; s2: tuple %d a {%d, %d} b {%d, %d} exc %d; "
            "s3: {%d, %d} tuple ok %d", test103_s1_value, test103_s1_err, (int)test103_s1_exc,
            test103_s1_fut_err, (int)test103_s2_tuple, test103_s2_a_value, test103_s2_a_err,
            test103_s2_b_value, test103_s2_b_err, (int)test103_s2_exc, test103_s3_value,
            test103_s3_err, (int)test103_s3_tuple_ok);

    /* the kills verifiably happened, the old repro's own guards */
    ASSERT_FN(CHK_BOOL(test103_s1_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s1_kill2_ret == co::ERROR_FINISHED));
    ASSERT_FN(CHK_BOOL(test103_s1_dtor == 1 && !test103_s1_ran_past));
    ASSERT_FN(CHK_BOOL(test103_s2_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s2_dtor == 1 && !test103_s2_ran_past));

    /* scenario 1: the kill is in the pair, nowhere else */
    ASSERT_FN(CHK_BOOL(test103_s1_value == 0));
    ASSERT_FN(CHK_BOOL(test103_s1_err == co::ERROR_USER));
    ASSERT_FN(CHK_BOOL(!test103_s1_exc));
    ASSERT_FN(CHK_BOOL(test103_s1_fut_err == co::ERROR_OK));

    /* scenario 2: the tuple arrives, each member reporting for itself */
    ASSERT_FN(CHK_BOOL(test103_s2_tuple && !test103_s2_exc));
    ASSERT_FN(CHK_BOOL(test103_s2_a_value == 7 && test103_s2_a_err == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s2_b_value == 0 && test103_s2_b_err == co::ERROR_USER));

    /* scenario 3: the untouched contract is intact */
    ASSERT_FN(CHK_BOOL(test103_s3_value == 42 && test103_s3_err == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test103_s3_tuple_ok));

    return 0;
}

int main() {
    int ret = test103_future_kill_surface();
    print_test_result("018-047-reproduced_future_kill_swallowed.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds ran in `repro/fix/24/` with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I.
[-fsanitize=address]`, per the fix spec. `base/` holds the pristine copy of the current colib.h
(your uncommitted accept fix as-is), `patched/` the edited one; `repro/fix/24/24-fix.diff` is the
exact diff above (+20/-5). No build emitted a warning.

1. Repro fails on the unpatched copy — fresh run, matching the recorded verdict byte for byte on
   every verdict line, exit 1:
   ```
   co_await fut value        : 0 (T{} DEFAULT - victim never returned)
   waiter fut.get_err()      : 0 (ERROR_OK - no error surfaced)
   exception rethrown        : no
   ...
   wait_all tuple            : {a=7, b=0}
   FAIL-REPRO: the killed victim's future resolved to T{} (0) with get_err()==ERROR_OK and no
   exception
   ```
2. Fix applied to the patched copy; the repro's expectations flipped to the fixed contract
   (`repro/fix/24/24-fixed-expect.cpp`, every kill-happened guard kept), PASS, exit 0, twice, and
   once under ASan (clean):
   ```
   task_killed_t thrown      : YES (e: -4)
   other exception           : no
   waiter fut.get_err()      : 0 (stays ERROR_OK: the error rides the exception)
   ...
   wait_all tuple delivered  : no (contract: not delivered when a member is killed)
   PASS: both kills surfaced as task_killed_t{ERROR_USER}; wait_all delivered no silent tuple
   ```
   The original repro, run unmodified against the fix, is also telling: scenario 1 completes with
   the kill surfaced, and scenario 2 now *terminates* (0xC0000409) because its driver — written to
   expect a silent tuple — never catches, so the kill exception escapes the scheduled root,
   `pool->run()` rethrows it (the posted-exception path), and main has no handler. The error
   travels further than the repro ever imagined; the flipped-expectation copy is the honest run.
3. The regression test above: patched copy PASSED exit 0 (plain build); the `std::exception`-only
   variant against the base copy FAILED cleanly on the kill-surfaced assertion (output pasted in
   the test section — the kill-happened guards before it all passed).
4. No-regression, three create_future-family tests plus five killer-family tests (the fix touches
   `end_chain`), include changed to `"colib.h"`, nothing else, all PASSED exit 0 against the
   patched copy:
   - `007-001-futures.cpp` (the documented future usage, value path)
   - `008-001-wait_all.cpp` (healthy wait_all tuple)
   - `018-004-reproduced_future_exception_propagation.cpp` (a task's own exception through a
     future — the channel this fix reuses)
   - `002-002-flowctrl_create_killer.cpp`, `018-005`, `018-014`, `018-016`, `018-017` (the killer
     contract around `end_chain`; a called root's error propagation is byte-identical to before)
5. NOT-CHECKED: no Linux/kqueue build on this box (no make, per the spec). Both functional hunks
   sit in platform-independent code (`killer_state_t`, `create_future`); no `#if COLIB_OS_*`
   region is touched, so reading is the validation there.

6. At apply time, 2026-10-29, on the header right after fix #14, in the applied shape (the pair):

   | check                       | result                                                       |
   |-----------------------------|--------------------------------------------------------------|
   | documented usage reaches it | yes: a `create_future`-wrapped (scheduled) task killed before |
   |                             | it returned, and `wait_all` with a killed member             |
   | DESIGN.md                   | the killer's own code is untouched but for `end_chain`'s     |
   |                             | write, which changes nothing for a called root; rules 1-3    |
   |                             | hold (the test's kills destroy exactly once, never resume)   |
   | epoll / kqueue              | platform-independent code; the test and `007-001`,           |
   |                             | `008-001`, `018-004`, `018-046` pass on Linux (WSL, g++      |
   |                             | 15.2); kqueue NOT-CHECKED (WIP)                              |
   | contract / docs             | `create_future` and `wait_all` change their return type and  |
   |                             | doc; both mirrored in `02_api.md`; `understanding.md`'s      |
   |                             | lines updated; the "killing one kills all" claim and its     |
   |                             | TODO removed on the user's ruling                            |
   | callers                     | `wait_all` (updated with it); the tests `007-001`,           |
   |                             | `008-001`, `018-004`, `018-046` read the pair now; no        |
   |                             | example uses either                                          |
   | pending fixes               | #26 is adjacent through `destroy_state` and must keep a      |
   |                             | future's EXIT reading a meaningful `state->err`; #16's       |
   |                             | refused sched would read as ERROR_GENERIC, its own contract  |
   |                             | to state if it wants more                                    |
   | runs                        | the before program on the post-#14 header shows the kill     |
   |                             | vanishing; the test passes after (2 of 2); the whole suites: |
   |                             | see the status line                                          |

## Interactions

The tracker rows: `create_future family: 14 24 (26 adjacent via destroy_state)` and, for the
`end_chain` hunk, `killer kill()/end_chain: 02 03 06`.

- #14 (a create_future future resolves at a generator-style `co_yield`, its real co_return
  discarded — same `exit_func` lambda; `FIX_14-create_future_yield.md` is already at the root): the
  two fixes land in the SAME lambda and compose by position, as FIX_14's own Interactions row
  states. #14 adds an early `if (state->err == ERROR_YIELDED) return ERROR_OK;` right after the
  ready-guard — WHEN the lambda may act; this fix's kill branch sits below the value check — WHAT
  the death reports. A yield never reaches my branch (it returns at #14's guard first), and a kill
  never trips #14's (a kill's error is negative; ERROR_YIELDED is positive), so neither subsumes
  the other and either may land first. Two mechanical notes: both hunks rewrite the same comment
  block above the lambda, so whoever lands second merges the wording (mine already describes the
  yield as a value case, theirs drops "(a co_yield too)" entirely); and #14 verified against a base
  where the killed victim still arrives at `destroy_state` with `err == ERROR_OK` — this fix's
  `end_chain` hunk is what turns that into the kill's error, which is precisely the value my branch
  reads. The teardown-after-a-yield edge also lands softer with both: post-#14 the EXIT at
  teardown of a yielded task returns at the guard, so no stale value and no kill error is stored —
  and the waiter is destroyed by the same teardown either way.
- #26 (a killed root's frame leaks at pool teardown with its woken caller queued — adjacent via
  `destroy_state`): no shared lines; this fix writes `root->err` inside `end_chain` before
  `destroy_state` runs, and reads it from modifs that fire inside `destroy_state`. The two fixes
  compose in any order; #26's eventual change to the teardown path should keep in mind that a
  future-wrapped root's EXIT now expects `state->err` to be meaningful.
- #02, #06 (same `kill()`/`end_chain` row; its #03 was dropped on 2026-10-13, since only a handle
  closed before its stop reaches that bug): #02 adds cases to `kill()`'s switch for an executing
  target (the LEAVE / WAIT_SEM windows), while #06's is in `io_data_t`'s destructor; neither
  touches `end_chain`'s body, so the change here has no textual overlap with either. Whatever
  #02's new cases destroy, they destroy through the same `end_chain`, so a future over such a
  target sees the kill consistently.
- #16 (sched refusal leaks the frame): if #16's fix destroys a refused task through
  `destroy_state`, the future's EXIT will fire with `state->err == ERROR_OK` (a refusal leaves no
  error in the state) and take the `T{}` branch, exactly as today. If sched refusal should surface
  to futures as an error too, that is #16's contract to state, not this fix's.
- #08 (an ERROR_SUSPENDED is final — the modifs engine row): no shared code; EXIT callbacks are an
  ignored-return family, so #08's short-circuit never applies to them.

## Open questions for the user

1. **The channel, the flagged contract decision.** A kill of a future-wrapped task must surface,
   and `create_future` returns `task<T>`, not `task<std::pair<T, error_e>>` — the options were:
   (a) throw (store an exception; the waiter's rethrow plumbing already exists, the same channel a
   task's own exception uses), (b) change the return type to a pair like `create_timeo` (breaking
   every caller and `wait_all`'s tuple), (c) carry it on the waiter's `get_err()` (impossible
   without changing `task<T>::await_resume` for every await: its `err` member is only written for a
   callee that ended without a value, and the waiter does return a value). This proposal is (a),
   chosen because the plumbing exists and the contract stays `task<T>`; it is the option the
   briefing flagged as likely. With (a), the waiter's `get_err()` stays ERROR_OK by design — the
   error rides the thrown `task_killed_t::e` — and the test pins that deliberately. If you prefer
   (b)'s shape for futures after all, `create_timeo` is the template and this proposal is void.
2. **wait_all's price under (a).** A member killed before it returns now makes the whole
   `co_await wait_all(...)` throw that member's `task_killed_t`; the tuple is *not* delivered, so
   the values of the healthy members (the 7 in the repro) are lost with it. That is the honest
   reading of "wait for all" on one channel, but a `wait_all_settle`-style variant that reports
   per-member outcomes would need its own return type — say the word and it is a separate proposal.
3. **The exception type.** New public type `task_killed_t` next to `kill_self_t`, carrying the
   kill's `error_e`, message "colib: the task of a future was killed". If you want a different
   name, or a `what()` that includes the numeric error, it is a one-line rename before landing.
4. **A stale doc claim I did not touch.** `wait_all`'s doc comment says "killing one kills all
   (sig_killer installed in all)", but `wait_all` installs no killer and no `sig_killer` exists in
   colib.h; with this fix, killing one member throws out of the await without killing the others.
   I left the sentence in place and did not quietly rewrite a contract you wrote; it needs your
   ruling (delete the claim, or make wait_all actually kill its members on one member's kill).
5. **A teardown edge, stated for honesty.** At pool teardown, `destroy_state` can fire a future's
   EXIT with a *stale* wait error in `state->err` (e.g. ERROR_WAKEUP left by an earlier
   `stop_io`-forced wake of an io the task then survived), and the new branch would store a
   `task_killed_t` for a teardown, not a kill. Nothing observable follows: the teardown destroys
   the future's waiter in the same sweep (it is parked on the future's own semaphore), and no one
   can await the future after the pool is gone. The pre-fix code took the `T{}` branch on the same
   edge with the same (non-)consequence. If you want the distinction hardened anyway, a dedicated
   kill marker in `state_t` instead of the error value is the heavier variant.

Decided at apply time, 2026-10-29, on the user's word: (1) (b), a pair like `create_timeo`'s,
no exception type: errors are values, a task's own exception is still rethrown, T must be default
constructible; (2) `wait_all` reports per member, a tuple of pairs, so nothing of the healthy
members is lost; (3) no `task_killed_t`; (4) the "killing one kills all" claim is deleted with the
TODO above it: no killer was ever installed, a kill of one member leaves the others running and
each reports for itself; (5) the teardown edge is closed by the pair's rule, ERROR_OK only when the
task returned: a death that is no kill reads ERROR_GENERIC, a kill its own error.
