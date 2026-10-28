# Proposal: an external awaitable is asked like any holder (fix #47)

Status: `[APPLIED 2026-10-12 on the user's word, together with FIX_44]`. The diff below is against
commit 9a13a9a ("fix 02") with FIX_44 applied, and its checks are in "How it was checked".

After the apply, `colib.h` is commit 9a13a9a with FIX_44's diff and this one on top, byte for byte,
and `make` in `tests/` builds and runs its 90 tests against it: `[PASSED: 90]/[FAILED: 0]`.

The flag was renamed `CO_MODIF_UNINHERIT_ON_EXTERN_RESUME` after the apply, on the user's word (see
"Open questions", 1); the diff and the test below show its first name.

## The bug

With FIX_44, a kill that runs its target until its next wait asks whatever took the target on the
way to let it go (RECLAIM), except an external awaitable: a target held by one reads
`STATE_EXTERNAL`, and the kill throws `kill_not_colib_owned_t`. The target lives on with the
awaitable, although an external hold is a wait (DESIGN.md, 3), the kind of stop where a kill ends
its target, and the kill's rule is to end its target or throw only when someone keeps it
(DESIGN.md, 1). The awaitable also has no way to be told anything: it isn't a modif, so it can't
answer RECLAIM, and it can't hear the coroutine's EXIT.

## The rule

An external awaitable is a holder like any other, and it answers through modifs:

- **`external_on_suspend(h, hold_mods)`** attaches `hold_mods` to the coroutine for as long as the
  awaitable holds it. They carry the new flag `CO_MODIF_UNINHERIT_ON_RESUME`, and
  `external_on_resume()` and `external_sched_resume()` take them off when the hold ends.
- **A kill that runs into the hold asks it** through RECLAIM, the same question it asks any
  holder. A RECLAIM modif among `hold_mods` can keep the coroutine (the kill throws) or let it go.
- **With no answer, it lets go**, as for any holder. The kill ends the coroutine at the hold,
  which is a wait. The awaitable object lives in the coroutine's frame for the length of the
  `co_await`, so it is destroyed with it, and its destructor is where it cancels what it started.
  The EXIT callbacks of `hold_mods` hear of the death too.
- **An awaitable that gives the coroutine straight back** (`external_sched_resume()` inside its
  `await_suspend()`) holds nothing, as before: the target goes on.

| the awaitable holding the target         | with FIX_44              | now                        |
|------------------------------------------|--------------------------|----------------------------|
| attaches no modifs                       | throws; the target lives | ends the target at the     |
|                                          | with the awaitable       | hold; the awaitable is     |
|                                          |                          | destroyed with it          |
| attaches a RECLAIM modif that keeps the  | throws (the modif isn't  | throws; the hold's modifs  |
| target                                   | asked)                   | come off when it gives the |
|                                          |                          | target back                |
| attaches a RECLAIM modif that lets the   | throws (the modif isn't  | ends the target at the     |
| target go                                | asked)                   | hold; the hold's EXIT      |
|                                          |                          | callbacks hear of it       |
| gives the target straight back to the    | the target goes on       | unchanged                  |
| ready queue                              |                          |                            |

## Where the fix lands

| `colib.h` region                         | what changes                                | +/-     |
|------------------------------------------|---------------------------------------------|---------|
| `CO_MODIF_RECLAIM_CBK` doc               | external awaitables answer too              | +3 -2   |
| `modif_flags_e`                          | `CO_MODIF_UNINHERIT_ON_RESUME`              | +2 -0   |
| `external_on_suspend` doc and            | `hold_mods`, what a kill does at the hold   | +10 -3  |
| declaration                              |                                             |         |
| `external_on_resume` /                   | the hold's modifs come off                  | +2 -0   |
| `external_sched_resume` docs             |                                             |         |
| `create_killer` doc                      | an external awaitable is asked the same way | +3 -2   |
| the external functions                   | attach `hold_mods`; take them off when the  | +9 -2   |
|                                          | hold ends                                   |         |
| `add_modifs()`                           | its body moves into `add_modifs_to_state()` | +3 -3   |
| `add_modifs_to_state()`,                 | new helpers                                 | +17 -0  |
| `drop_modifs_with()`                     |                                             |         |
| `killer_state_t::drive()`                | an external hold is asked, and let go it    | +7 -8   |
|                                          | ends the target                             |         |
| total                                    |                                             | +56 -20 |

## The change

The diff is against the working tree's `colib.h` with FIX_44 applied. The reasoning, per hunk:

1. **`CO_MODIF_RECLAIM_CBK` doc.** It names the external awaitable among the holders that answer.
2. **`CO_MODIF_UNINHERIT_ON_RESUME`.** The flag that makes a modif last one external hold.
   It is `0x8`, clear of FIX_45's `0x4`.
3. **`external_on_suspend` doc and declaration.** The new parameter, what a kill does at the hold,
   and where an awaitable cleans up.
4. **`external_on_resume` doc.** The hold's modifs come off.
5. **`external_sched_resume` doc.** The same.
6. **`create_killer` doc.** An external awaitable is asked the same way; let go, the target dies at
   the hold.
7. **The external functions.** `external_on_suspend()` attaches `hold_mods` after the LEAVE
   callbacks, so they don't see the LEAVE; the two resumes take them off before the coroutine
   runs again. The two helpers are declared here and defined with the other table helpers.
8. **`add_modifs()`.** Its body moves into `add_modifs_to_state()`, which `external_on_suspend()`
   uses too.
9. **`add_modifs_to_state()` and `drop_modifs_with()`.** The first attaches a pack to a state's
   table, allocating it if needed; the second takes off the modifs that carry a flag. If FIX_45 is
   applied, its `uninherit_on_yield()` becomes `drop_modifs_with(state,
   CO_MODIF_UNINHERIT_ON_YIELD)`.
10. **`drive()`'s comment.** An external hold is one of the stops where the target ends.
11. **`drive()`'s loop.** The external hold is asked like the rest; let go, it ends the loop as the
    kill's own park does, and `end_chain()` ends the target there.

```diff
--- colib.h (commit 9a13a9a + FIX_44)
+++ colib.h (patched)
@@ -779,8 +779,9 @@
     CO_MODIF_RETURN_CBK,
 
     /*! This is called when a kill that ran its chain (a wait had already done its work) needs this
-    corutine back from a modif that took it on the way: a park, a taken call, or a RETURN modif
-    of its callee that took it as the caller. Only this corutine's modifs are asked, so a modif
+    corutine back from what took it on the way: a park, a taken call, a RETURN modif of its callee
+    that took it as the caller, or an external awaitable (through the modifs it attached with
+    external_on_suspend()). Only this corutine's modifs are asked, so a modif
     that holds a corutine must be among its modifs (a RETURN modif that takes the caller must be
     on the caller too, inherited from it for example). The modif that holds it lets it go: it
     forgets it, stops whatever would resume or destroy it, and returns ERROR_OK; a modif that
@@ -799,6 +800,8 @@
     CO_MODIF_INHERIT_NONE = 0x0,    /*!< Not inherited */
     CO_MODIF_INHERIT_ON_CALL = 0x1, /*!< Inherited on call */
     CO_MODIF_INHERIT_ON_SCHED = 0x2,/*!< Inherited on sched */
+    CO_MODIF_UNINHERIT_ON_RESUME = 0x8, /*!< Removed from a corutine when the external hold it was
+                                        attached for ends (see external_on_suspend()) */
 };
 
 
@@ -1373,15 +1376,22 @@
  * Usage:
  * - Call this in your awaitable’s await_suspend before performing any custom suspension logic.
  * - Returns a raw pointer to the coroutine’s state, which must later be resumed.
- * - Until then the coroutine's state reads STATE_EXTERNAL, and a kill can't end it: it throws
- *   kill_not_colib_owned_t.
+ * - Until then the coroutine's state reads STATE_EXTERNAL. A kill that runs into this hold asks
+ *   the awaitable to let the coroutine go (see CO_MODIF_RECLAIM_CBK) through the modifs given in
+ *   hold_mods; with none, it lets go. Let go, the kill ends the coroutine here, and the awaitable
+ *   object is destroyed with its frame: cancel any pending operation in its destructor. Kept, the
+ *   kill throws kill_not_colib_owned_t.
  * 
  * @tparam P Coroutine promise type
  * @param colib_coro The coroutine handle being suspended
+ * @param hold_mods Modifs attached to the coroutine while the awaitable holds it; create them with
+ *                  CO_MODIF_UNINHERIT_ON_RESUME, so external_on_resume() or
+ *                  external_sched_resume() takes them off when the hold ends
  * @return Pointer to the internal coroutine state, used for later resumption
  */
 template <typename P>
-inline state_t *external_on_suspend(std::coroutine_handle<P> colib_coro);
+inline state_t *external_on_suspend(std::coroutine_handle<P> colib_coro,
+        const modif_pack_t &hold_mods = {});
 
 /*! @fn
  * Resumes a coroutine that was suspended using external_on_suspend. This ensures the library 
@@ -1390,6 +1400,7 @@
  * Usage:
  * - Call this when you want to resume the coroutine.
  * - Returns a coroutine handle that should be awaited or scheduled next.
+ * - The modifs flagged CO_MODIF_UNINHERIT_ON_RESUME come off the coroutine: the hold is over.
  * 
  * @param state The state of the coroutine to resume
  * @return Coroutine handle ready for execution
@@ -1408,6 +1419,7 @@
  *   system.
  * - The task will be added inside the ready_tasks of the state's pool, so resuming the pool will
  *   eventually execute the scheduled task and the provided await_resume will be called
+ * - The modifs flagged CO_MODIF_UNINHERIT_ON_RESUME come off the coroutine: the hold is over.
  * 
  * @param state Pointer to the suspended coroutine’s state
  */
@@ -1638,8 +1650,9 @@
  * else took it on the way, the kill asks for it back through the RECLAIM callbacks (see
  * CO_MODIF_RECLAIM_CBK): let go, it goes on as it would have without that modif, to its next
  * wait, where it dies, or to its own end; kept, the kill throws kill_not_colib_owned_t and the
- * holder keeps it. An external awaitable can't be asked: one that holds it keeps it, and the kill
- * throws.
+ * holder keeps it. An external awaitable that holds it is asked the same way, through the modifs
+ * it attached with external_on_suspend(); let go (with none, it lets go), the target dies at that
+ * hold, which is a wait.
  *
  * The kill function returns ERROR_OK when it killed the target and ERROR_FINISHED when there was
  * nothing left to kill. Called from a callback of its own kill (EXIT, UNWAIT, or the target's
@@ -4503,23 +4516,30 @@
     return std::shared_ptr<pool_t>(new pool_internal_t{});
 }
 
+inline void add_modifs_to_state(pool_t *pool, state_t *state, const modif_pack_t &mods);
+inline void drop_modifs_with(state_t *state, modif_flags_e flag);
+
 /* External Part
 ------------------------------------------------------------------------------------------------- */
 
 template <typename P>
-inline state_t *external_on_suspend(handle<P> colib_coro) {
+inline state_t *external_on_suspend(handle<P> colib_coro, const modif_pack_t &hold_mods) {
     auto state = &colib_coro.promise().state;
     do_leave_modifs(state);
-    state_access_t::set(state, STATE_EXTERNAL);    /* held outside colib: a kill can't ask */
+    if (!hold_mods.empty())
+        add_modifs_to_state(state->pool, state, hold_mods);    /* they last this hold */
+    state_access_t::set(state, STATE_EXTERNAL);    /* held outside colib: a kill asks the hold */
     return state;
 }
 
 inline handle<void> external_on_resume(state_t *state) {
+    drop_modifs_with(state, CO_MODIF_UNINHERIT_ON_RESUME);     /* the hold is over */
     do_entry_modifs(state);
     return state->self;
 }
 
 inline void external_sched_resume(state_t *state) {
+    drop_modifs_with(state, CO_MODIF_UNINHERIT_ON_RESUME);     /* the hold is over */
     state->pool->get_internal()->push_ready(state);
 }
 
@@ -6244,9 +6264,9 @@
     }
 }
 
-template <typename T>
-inline task<T> add_modifs(pool_t *pool, task<T> t, const modif_pack_t& _mods) {
-    auto &table = t.h.promise().state.modif_table;
+/* adds `_mods` to the table of `state`, which gets one from `pool` if it has none */
+inline void add_modifs_to_state(pool_t *pool, state_t *state, const modif_pack_t &_mods) {
+    auto &table = state->modif_table;
     if (!table)
         table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                 dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
@@ -6254,6 +6274,23 @@
     add_modifs_to_table(table, mods);
     if (get_modif_table_sz(table) == 0)
         table = nullptr;
+}
+
+/* takes the modifs flagged `flag` off the table of `state` */
+inline void drop_modifs_with(state_t *state, modif_flags_e flag) {
+    if (!state->modif_table)
+        return;
+    for (auto &cbk_table : state->modif_table->table)
+        std::erase_if(cbk_table, [flag](const modif_p &modif) {
+            return modif && (modif->flags & flag);
+        });
+    if (get_modif_table_sz(state->modif_table) == 0)
+        state->modif_table = nullptr;
+}
+
+template <typename T>
+inline task<T> add_modifs(pool_t *pool, task<T> t, const modif_pack_t& _mods) {
+    add_modifs_to_state(pool, &t.h.promise().state, _mods);
     return t;
 }
 
@@ -6504,10 +6541,10 @@
         return ERROR_OK;
     }
 
-    /* its wait already did its work: the code after it runs, here, until its next wait, where
-    on_wait parks it and it ends, or until its end. Something else that took it on the way (a
-    call, a return, an external awaitable) is asked to let it go (RECLAIM): let go, it goes on as
-    colib would have gone on; kept, the kill throws */
+    /* its wait already did its work: the code after it runs, here, until its next wait, where it
+    ends (on_wait parks it, or an external awaitable holds it), or until its end. Whatever took it
+    on the way (a call, a return, an external awaitable) is asked to let it go (RECLAIM): let go
+    at a call or a return, it goes on as colib would have gone on; kept, the kill throws */
     error_e drive() {
         pool_internal_t *pool = top->pool->get_internal();
         state_t *caller = root->caller_state;   /* a root without one frees itself at its end */
@@ -6520,13 +6557,12 @@
             top->self.resume();     /* runs until it stops */
             if (completed)
                 return root_returned(pool, caller);
-            if (top && (top->get_state() == STATE_EXTERNAL ||
-                    exec_reclaim_modifs(top) != ERROR_OK)) {
+            if (top && exec_reclaim_modifs(top) != ERROR_OK) {
                 queue_caller(pool, nullptr);    /* a holder keeps it: the slot is dropped */
                 throw kill_not_colib_owned_t(top);
             }
-            if (!top || our_park)
-                break;              /* its next wait: it ends here */
+            if (!top || our_park || top->get_state() == STATE_EXTERNAL)
+                break;              /* its next wait, colib's or an awaitable's: it ends here */
             if (top->get_state() == STATE_READY)
                 pool->remove_ready(top);    /* given back to the ready queue: it runs here */
         }
```

## The regression test

`tests/018-036-reproduced_killer_external_reclaim.cpp` (Test90). Each case drives a target whose
semaphore wait already gave it its token into an external awaitable that holds it:

1. an awaitable with no modifs, which cleans up in its destructor: the kill returns `ERROR_OK`,
   the target is destroyed once, and the awaitable's destructor ran while it held the target;
2. an awaitable whose RECLAIM callback keeps the target: the kill throws; the awaitable later
   gives it back, the target finishes, and the hold's modifs are gone from it;
3. an awaitable whose RECLAIM callback lets the target go: the kill ends it, and the hold's EXIT
   callback hears of it once.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <vector>

/* Test90 - Reproduced Bugs: a kill that ran its target into an external awaitable couldn't end it
================================================================================================= */

/* Was an open question of FIX_44: an external awaitable that took the target during a kill's run
(external_on_suspend()) couldn't be asked to let it go, so the kill threw kill_not_colib_owned_t
and the target lived on, although an external hold is a wait, where a kill ends its target.

Now the hold is asked like any holder, through the modifs the awaitable attached with
external_on_suspend() (flagged CO_MODIF_UNINHERIT_ON_RESUME, so they come off when the hold ends);
with none, it lets go. Let go, the kill ends the target at the hold, and the awaitable object,
which lives in the target's frame, is destroyed with it: its destructor cancels what it started.
Kept, the kill throws.

The runs, each on a target whose semaphore wait already gave it its token: an awaitable with no
modifs, which cleans up in its destructor; one whose RECLAIM callback keeps the target, which it
later gives back, after which its modifs are gone; one whose RECLAIM callback lets the target go,
and whose EXIT callback then hears of its death. */

static const char *test90_file = "018-036-reproduced_killer_external_reclaim.cpp";

static int test90_destructed = 0;
static bool test90_got_token = false;
static bool test90_after = false;
static co::state_t *test90_held = nullptr;      /* what the awaitable would resume later */
static bool test90_cancelled = false;           /* the awaitable's destructor ran while holding */
static bool test90_threw = false;
static co::error_e test90_ret = co::ERROR_GENERIC;
static int test90_reclaims = 0;
static int test90_exits = 0;
static size_t test90_hold_mods_after = 99;      /* hold modifs left once the hold ended */

struct test90_marker_t {
    ~test90_marker_t() { test90_destructed++; }
};

static void test90_reset() {
    test90_destructed = 0;
    test90_got_token = false;
    test90_after = false;
    test90_held = nullptr;
    test90_cancelled = false;
    test90_threw = false;
    test90_ret = co::ERROR_GENERIC;
    test90_reclaims = 0;
    test90_exits = 0;
    test90_hold_mods_after = 99;
}

/* an awaitable that holds the coroutine until someone resumes test90_held */
struct test90_hold_t {
    co::modif_pack_t hold_mods;

    ~test90_hold_t() {
        if (test90_held) {
            test90_cancelled = true;    /* it would cancel its pending operation here */
            test90_held = nullptr;
        }
    }

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        test90_held = co::external_on_suspend(h, hold_mods);
        return std::noop_coroutine();
    }

    void await_resume() { test90_held = nullptr; }
};

static co::task_t test90_victim(co::sem_p sem, co::modif_pack_t hold_mods) {
    test90_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test90_got_token = true;
    co_await test90_hold_t{hold_mods};
    test90_after = true;        /* runs only once the awaitable gives it back */
    size_t left = 0;
    for (auto &modif : co_await co::task_modifs())
        left += (modif->flags & co::CO_MODIF_UNINHERIT_ON_RESUME) != 0;
    test90_hold_mods_after = left;
    co_return 0;
}

static co::task_t test90_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the target waits on sem */
    sem->signal();              /* it takes the token and is queued, not resumed yet */
    try {
        test90_ret = kill_fn(); /* its wait did its work: the kill runs it into the awaitable */
    }
    catch (co::kill_not_colib_owned_t &e) {
        test90_threw = (e.target == test90_held);
    }
    co_return 0;
}

/* the modifs an awaitable attaches while it holds the target: RECLAIM answers `answer` */
static co::modif_pack_t test90_hold_mods(co::error_e answer) {
    auto f = co::CO_MODIF_UNINHERIT_ON_RESUME;
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [answer](co::state_t *) -> co::error_e {
                test90_reclaims++;
                return answer;
            }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(f,
            [](co::state_t *) -> co::error_e {
                test90_exits++;         /* the target died while held */
                return co::ERROR_OK;
            }));
    return pack;
}

static int test90_run(co::modif_pack_t hold_mods, bool keeps) {
    test90_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test90_victim(sem, hold_mods), killer_mods));
    pool->sched(test90_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test90_got_token));

    if (keeps) {
        ASSERT_FN(CHK_BOOL(test90_threw));                  /* the awaitable kept it */
        ASSERT_FN(CHK_BOOL(test90_destructed == 0 && test90_reclaims == 1));
        co::external_sched_resume(test90_held);             /* it gives the target back */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test90_after && test90_destructed == 1));
        ASSERT_FN(CHK_BOOL(test90_hold_mods_after == 0));   /* the hold's modifs came off */
        ASSERT_FN(CHK_BOOL(test90_exits == 0));             /* gone before the target ended */
    }
    else {
        ASSERT_FN(CHK_BOOL(test90_ret == co::ERROR_OK));    /* the kill ended it at the hold */
        ASSERT_FN(CHK_BOOL(!test90_after && test90_destructed == 1));
        ASSERT_FN(CHK_BOOL(test90_cancelled));              /* the awaitable cleaned up */
        ASSERT_FN(CHK_BOOL(test90_held == nullptr));
        ASSERT_FN(CHK_BOOL(test90_reclaims == (hold_mods.empty() ? 0 : 1)));
        ASSERT_FN(CHK_BOOL(test90_exits == (hold_mods.empty() ? 0 : 1)));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test90_run(co::modif_pack_t{}, false);            /* no modifs: it lets go */
    if (ret == 0)
        ret = test90_run(test90_hold_mods(co::ERROR_USER), true);   /* it keeps the target */
    if (ret == 0)
        ret = test90_run(test90_hold_mods(co::ERROR_OK), false);    /* it lets the target go */
    print_test_result(test90_file, ret >= 0);
    return ret;
}
```

FIX_44's `018-035` changes in one case, the external awaitable that holds the target without
modifs: it now lets go, and the kill ends the target instead of throwing.

```diff
--- tests/018-035-reproduced_killer_drive_reclaim.cpp (FIX_44)
+++ tests/018-035-reproduced_killer_drive_reclaim.cpp (with FIX_47)
@@ -21,10 +21,11 @@
 ERROR_OK) or keeps it (returns an error or ERROR_SUSPENDED). Kept, the kill throws
 kill_not_colib_owned_t and the holder keeps it. Let go at a wait, the kill ends it there; let go
 at a call or a return, it goes on as colib would have, until its next wait or its end. An external
-awaitable can't be asked: while one holds the target (STATE_EXTERNAL) the kill throws, and a target
-it gave straight back to the ready queue goes on. For the root's caller, the caller's modifs are
-asked (a RETURN modif that takes it is on it too, inherited from it): one that keeps it resumes
-it, one that lets it go has the kill queue it, and one that already gave it back is left alone.
+awaitable is asked through the modifs it attached (with none, it lets go): let go, the kill ends
+the target at the hold, a wait; a target it gave straight back to the ready queue goes on. For the
+root's caller, the caller's modifs are asked (a RETURN modif that takes it is on it too, inherited
+from it): one that keeps it resumes it, one that lets it go has the kill queue it, and one that
+already gave it back is left alone.
 
 The runs, each on a target whose semaphore wait already gave it its token: a modif parks it at its
 next wait and lets it go, added before and after the killer's pack; the same modif keeps it, by an
@@ -291,6 +292,8 @@
 struct test89_hold_t {
     bool hands_back;            /* it gives the target straight back to the ready queue */
 
+    ~test89_hold_t() { test89_held = nullptr; }    /* destroyed while holding: it lets go */
+
     bool await_ready() { return false; }
 
     template <typename P>
@@ -332,12 +335,10 @@
         ASSERT_FN(CHK_BOOL(test89_destructed == 1));
     }
     else {
-        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the awaitable keeps it */
-        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
-        co::external_sched_resume(test89_held);             /* its holder gives it back */
-        ASSERT_FN(pool->run());
-        ASSERT_FN(CHK_BOOL(test89_after));                  /* it ran on, and ended by itself */
+        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* asked, silent: it let go */
+        ASSERT_FN(CHK_BOOL(!test89_after));                 /* the kill ended it at the hold */
         ASSERT_FN(CHK_BOOL(test89_destructed == 1));
+        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the awaitable went with it */
     }
     ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
     return 0;
```

## How it was checked

All runs on Windows 10, MSVC 19.43, built through the tests makefile on scratch copies.

On the patched header, plain and with
`CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /FS /I.. /fsanitize=address'`:

```
plain  [PASSED]: 018-037-reproduced_killer_external_reclaim.cpp     exit 0   (its number before
asan   [PASSED]: 018-037-reproduced_killer_external_reclaim.cpp     exit 0    it landed as 018-036)
plain  [PASSED]: 018-035-reproduced_killer_drive_reclaim.cpp        exit 0
asan   [PASSED]: 018-035-reproduced_killer_drive_reclaim.cpp        exit 0
```

On the FIX_44 header, each case of the test built alone, ASan (the header has neither the
parameter nor the flag, so the copy calls `external_on_suspend(h)` and defines the flag's value):

| case                         | on the FIX_44 header                                              |
|------------------------------|-------------------------------------------------------------------|
| 1: no modifs                 | `Failed CHK_BOOL(test90_ret == co::ERROR_OK)`: the kill threw     |
|                              | instead of ending the target at the hold                          |
| 2: a RECLAIM that keeps      | `Failed CHK_BOOL(test90_destructed == 0 && test90_reclaims ==     |
|                              | 1)`: the awaitable was never asked                                |
| 3: a RECLAIM that lets go    | `Failed CHK_BOOL(test90_ret == co::ERROR_OK)`                     |

The whole suite (90 files: the 88 in `tests/`, FIX_44's `018-035` with this fix's change, this fix's
test (now `018-036`), and `012-002` with FIX_44's line), on scratch copies of the patched header and
of `tests/`: `make`, then `make` with the ASan flags above, then with `/O2`:

```
plain  [PASSED: 90]/[FAILED: 0]
asan   [PASSED: 90]/[FAILED: 0]
/O2    [PASSED: 90]/[FAILED: 0]
```

**NOT-CHECKED.** The epoll and kqueue backends (they don't compile on this machine; no
backend-specific line changed), and GCC and Clang (not installed here; `std::erase_if` on a vector
is C++20).

## Interactions

- **FIX_44.** Required: this fix changes its drive loop, its docs and one case of its test.
- **FIX_45, modifs that last one call.** They compose. Both add a flag (`0x4` there, `0x8` here)
  and a way to take flagged modifs off a table; whichever lands second makes `uninherit_on_yield()`
  a call of `drop_modifs_with()`.
- **FIX_19, ENTER after `external_sched_resume()`.** They compose: both touch
  `external_sched_resume()`, this fix only to take the hold's modifs off before it queues the
  coroutine.
- **`repro/44`, mode `ext`.** Its awaitable keeps a raw pointer and resumes it later without
  cleaning up in a destructor; under this fix the kill ends the target at the hold, so that later
  resume reads freed memory. The repro's holder is what the new doc tells awaitable authors not to
  write.

## Open questions

1. **The flag's name.** Settled after the apply, on the user's word: the flag is now
   `CO_MODIF_UNINHERIT_ON_EXTERN_RESUME`. `..._ON_RESUME` read like any resume, while only the end
   of an external hold (`external_on_resume()`, `external_sched_resume()`) takes it off; the new
   name says so and pairs with FIX_45's `..._ON_YIELD`. The diff and the test above show the first
   name, as applied.
2. **A kill outside a drive** still throws for a target an external awaitable holds, as for a
   target a modif parked (FIX_44's open question 2). Asking there too would make both consistent;
   for an external hold the only window is an awaitable whose own `await_suspend()` calls the kill
   after `external_on_suspend()`.
3. **Holds that end behind colib's back.** An awaitable that resumes the coroutine with
   `h.resume()` instead of `external_on_resume()` leaves its modifs attached; a later kill would
   still ask them. That path already skips ENTER; the doc of `external_on_suspend()` names the two
   resumes as the way a hold ends.
4. **A RECLAIM callback must not give the coroutine back** with `external_sched_resume()`: that
   would take its own modifs off the table while the RECLAIM list runs. "Let go" means forgetting
   it, as the RECLAIM doc says.
