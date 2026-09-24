# Ordering the modif callbacks

Two changes, in order. **Part 1** reverses the order of the closing callbacks between modifs.
**Part 2** fixes the order between kinds of callback (`tests/BUGS.md` #1). Both come after
`killer_diff.md`.

# Part 1: reversed order for the closing callbacks

This is the step after `killer_diff.md`. The diff below goes on top of that one, with both its
section 3 and its section 4 applied. It is kept separate so the killer review stays as it is.
Nothing is applied to the repo.

## What changes

Modif callbacks come in open/close pairs:

| Opens | Closes |
|---|---|
| CALL / SCHED | EXIT |
| ENTER | LEAVE |
| WAIT_IO | UNWAIT_IO |
| WAIT_SEM | UNWAIT_SEM |

The closing callbacks (EXIT, LEAVE, UNWAIT_IO, UNWAIT_SEM) now run in the reverse order of the
modif table, so the pairs nest like constructors and destructors. Take `a` added before `b`: `a`
opens, `b` opens, `b` closes, `a` closes. When modif `b` depends on something `a` set up, `a` is
no longer torn down while `b` still uses it. The user can't fix this ordering on their side,
because the table's order is set by colib.

What stays as it is:

- The opening callbacks (CALL, SCHED, ENTER, the WAITs, WAIT_YIELD) run in table order, as today.
- WAIT_YIELD has no closing callback. A yield is closed by its ENTER, which is an opening callback.
- The dispatch rule from `killer_diff.md` is unchanged, and so is the set of callbacks that always
  all run.

Today no promise depends on this order: colib's own modifs (the killer, `create_future`, the
tracer) each touch only their own state. So this is about user modifs that depend on each other.

This is about the order *between modifs* for the same kind of callback. The order *between kinds*
(WAIT against LEAVE, UNWAIT against ENTER) has to nest the same way, otherwise the reversal above
only fixes half the picture. That's `tests/BUGS.md` #1, and part 2 below proposes the fix.

## The diff

```diff
--- a/colib.h
+++ b/colib.h
@@ -2523,9 +2523,15 @@ inline error_e do_generic_modifs(state_t
             cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
             cbk_id == CO_MODIF_ENTER_CBK || cbk_id == CO_MODIF_UNWAIT_IO_CBK ||
             cbk_id == CO_MODIF_UNWAIT_SEM_CBK;
+    /* the closing ones run in reverse, so they nest like destructors */
+    constexpr bool reversed =
+            cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
+            cbk_id == CO_MODIF_UNWAIT_IO_CBK || cbk_id == CO_MODIF_UNWAIT_SEM_CBK;
     error_e result = ERROR_OK;
     if (auto modif_table = state->modif_table) {
-        for (auto &modif : modif_table->table[cbk_id]) {
+        auto &cbks = modif_table->table[cbk_id];
+        for (size_t i = 0; i < cbks.size(); i++) {
+            auto &modif = reversed ? cbks[cbks.size() - 1 - i] : cbks[i];
             error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
             if (ret < 0 && !ignored_ret)
                 return ret;
```

The loop goes by index instead of a range-for, so one loop body serves both directions.

## The test: `tests/011-007-modifs_close_order.cpp`

Two modifs, `a` then `b`, log every ENTER, LEAVE, WAIT_SEM, UNWAIT_SEM and EXIT on a coroutine
that waits on a semaphore once and then returns:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test51 - Modifs: closing callbacks run in reverse order
================================================================================================= */

/* Two modifs, `a` added before `b`, on one coroutine. The opening callbacks (ENTER, WAIT_SEM) run
in the order the modifs were added, the closing ones (LEAVE, UNWAIT_SEM, EXIT) in reverse, so the
pairs nest like constructors and destructors: a opens, b opens, b closes, a closes. */

static std::string test51_order;

static co::modif_pack_t test51_pack(char id) {
    auto flags = co::CO_MODIF_INHERIT_NONE;
    auto log = [id](char ev) { test51_order += ev; test51_order += id; test51_order += ' '; };
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('E'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('L'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
        [log](co::state_t *, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
            log('W'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
        [log](co::state_t *, co::sem_t *) -> co::error_e { log('U'); return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(flags,
        [log](co::state_t *) -> co::error_e { log('X'); return co::ERROR_OK; }));
    return pack;
}

static co::task_t test51_waiter(co::sem_p sem) {
    co_await sem->wait();
    co_return 0;
}

static co::task_t test51_signaler(co::sem_p sem) {
    sem->signal();
    co_return 0;
}

int test51_close_order() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    auto t = test51_waiter(sem);
    co::add_modifs(pool.get(), t, test51_pack('a'));
    co::add_modifs(pool.get(), t, test51_pack('b'));
    pool->sched(t);
    pool->sched(test51_signaler(sem));
    ASSERT_FN(pool->run());

    /* sched's ENTER; the wait: WAIT, LEAVE; the resume: ENTER, UNWAIT; the return: LEAVE, EXIT */
    DBG("order: %s", test51_order.c_str());
    ASSERT_FN(CHK_BOOL(test51_order ==
            "Ea Eb Wa Wb Lb La Ea Eb Ub Ua Lb La Xb Xa "));
    return 0;
}

int main() {
    int ret = test51_close_order();
    print_test_result("011-007-modifs_close_order.cpp", ret >= 0);
    return ret;
}
```

## Verification

Built through the scratch `tests/` makefile. The `colib.h` there has `killer_diff.md` (sections 3
and 4) applied, plus this diff.

| | Result |
|---|---|
| `011-007` without this diff (killer diff only) | **fails**: `Ea Eb Wa Wb La Lb Ea Eb Ua Ub La Lb Xa Xb`. Every closing callback runs in forward order. |
| `011-007` with this diff | passes: `Ea Eb Wa Wb Lb La Ea Eb Ub Ua Lb La Xb Xa` |
| Whole suite (`011-007` and `002-005` included) | all pass except `018-011`, which fails the same way on today's `colib.h` (`BUGS.md` #5). `003-002` failed once on timing, `sleep(150ms)` measuring 149 ms while four builds ran in parallel, and passed 5 out of 5 reruns. |

## What's still left

This diff doesn't fully match open and close calls. WAIT stops at the first modif that returns an
error, and the UNWAIT callbacks always all run. So the modifs after the one that failed get an
UNWAIT for a WAIT they never saw. The killer handles that fine, since it only clears a pointer
that was already null. A user modif that counts its WAIT/UNWAIT pairs would count wrong, though.
Real destructor semantics would be "close only the ones that opened, in reverse". That needs the
awaiter to remember how far WAIT got and pass it on to UNWAIT. It belongs in `docs/TODO.md`
unless you want it here.

---

# Part 2: the order between kinds of callback (`tests/BUGS.md` #1)

Part 2 goes on top of part 1: `killer_diff.md` sections 3 and 4, then part 1, then this. Nothing is
applied to the repo.

## The problem

Today, a wait looks like this, for one coroutine:

```
ENTER ... WAIT LEAVE | ENTER UNWAIT ... LEAVE
```

The pairs **cross**. WAIT opens while the coroutine is still running, and UNWAIT closes after it is
running again. So "waiting" and "running" overlap, and the ENTER/LEAVE pair doesn't contain the
wait, nor the other way round. That's also why:
- the killer's replay and the pool's `clear()` have to fake a whole ENTER, UNWAIT, LEAVE sequence
  to close a single wait;
- the debug checks have to accept a WAIT while the coroutine is still "entered".

The `CO_MODIF_WAIT_IO_CBK` doc ("after the leave cbk") and the design notes at the end of
`colib.h` already describe the nested order.

## The fix

A coroutine is either **running** (between ENTER and LEAVE) or **waiting** (between WAIT and
UNWAIT), never both:

```
ENTER ... LEAVE | WAIT ... UNWAIT | ENTER ... LEAVE
```

What changes:

- **Suspending** (`io_awaiter_t`, `sem_awaiter_t`, `yield_awaiter_t`, `force_stop`): LEAVE comes
  first, then WAIT.
  - Parked (`ERROR_SUSPENDED`): UNWAIT, then return `noop`. The coroutine has already left.
  - Aborted (negative), or the io registration failed: UNWAIT, then ENTER, and continue. The wait
    closes and the coroutine is running again.
  - A refused yield or `force_stop`: ENTER, and continue. WAIT_YIELD has no UNWAIT.
- **Resuming**: UNWAIT first, then ENTER. Together with part 1 this nests all the way through: with
  two modifs `a`, `b` the log reads `Ea Eb Lb La Wa Wb Ub Ua Ea Eb Lb La Xb Xa`.
- **Closing a wait from outside** (the killer's `drop()`, the Linux and Windows `io_pool_t::clear()`,
  `sem_internal_t::clear()`): the waiting coroutine has already left, so only its UNWAIT runs, then
  the destroy. The fake ENTER/LEAVE around it goes away.
- **Debug checks**:
  - WAIT, UNWAIT and WAIT_YIELD require `left`;
  - ENTER and LEAVE refuse to run while the coroutine is waiting on an io or a semaphore.
- **UNWAIT doc**: it now says "(before the enter cbk)". The WAIT_IO doc's "(after the leave cbk)" is
  now true.

Nothing changes for calls, returns and `external_on_suspend`: they only have LEAVE/ENTER/EXIT, and
those already nest. The killer's logic doesn't change either. Its WAIT callbacks never look at
`entered`, and a coroutine that has parked or is waiting has left in both orders.

## The diff

```diff
--- a/colib.h
+++ b/colib.h
@@ -730,7 +730,8 @@ enum modif_e : int32_t {
     once: code that retries the wait without handling that error spins. */
     CO_MODIF_WAIT_IO_CBK,
 
-    /*! This is called when the io is done and the corutine that awaited it is resumed */
+    /*! This is called when the io is done and the corutine that awaited it is resumed (before the
+    enter cbk) */
     CO_MODIF_UNWAIT_IO_CBK,
 
     /*! This is similar to wait_io, but on a semaphore */
@@ -3068,9 +3069,7 @@ struct io_pool_t {
             if (auto *data = fd_data_fast[i]) {
                 for (auto &w : data->waiters) {
                     io_desc_t desc{ .fd = i, .events = w.mask };
-                    do_entry_modifs(w.state);
                     do_unwait_io_modifs(w.state, desc);
-                    do_leave_modifs(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = i, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3085,9 +3084,7 @@ struct io_pool_t {
             if (data) {
                 for (auto &w : data->waiters) {
                     io_desc_t desc{ .fd = fd, .events = w.mask };
-                    do_entry_modifs(w.state);
                     do_unwait_io_modifs(w.state, desc);
-                    do_leave_modifs(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = fd, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3569,9 +3566,7 @@ struct io_pool_t {
                     }
                 }
                 io_desc_t desc{ .data = data, .h = data->h };
-                do_entry_modifs(data->state);
                 do_unwait_io_modifs(data->state, desc);
-                do_leave_modifs(data->state);
                 destroy_state(data->state);
             }
         }
@@ -4258,16 +4253,15 @@ struct yield_awaiter_t {
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
 
+        do_leave_modifs(state);
         error_e err = do_yield_modifs(state);
-        if (err == ERROR_SUSPENDED) {
-            /* parked by a modif */
-            do_leave_modifs(state);
-            return std::noop_coroutine();
-        }
-        if (err != ERROR_OK)
+        if (err == ERROR_SUSPENDED)
+            return std::noop_coroutine();   /* parked by a modif */
+        if (err != ERROR_OK) {
+            do_entry_modifs(state);
             return h;       /* refused by a modif */
+        }
 
-        do_leave_modifs(state);
         pool->get_internal()->push_ready(state);
         triggered = true;
         // TODO: is it required to call the modifs here if the returned coroutine is the same as
@@ -4368,17 +4362,18 @@ struct io_awaiter_t {
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
 
+        do_leave_modifs(state);
         error_e err = do_wait_io_modifs(state, io_desc);
         if (err == ERROR_SUSPENDED) {
-            /* parked by a modif, `this` may be gone after do_leave_modifs */
+            /* parked by a modif */
             do_unwait_io_modifs(state, io_desc);
-            do_leave_modifs(state);
             return std::noop_coroutine();
         }
         if (err != ERROR_OK) {
             /* aborted by a modif */
             ret_err = err;
             do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
             return h;
         }
 
@@ -4387,9 +4382,9 @@ struct io_awaiter_t {
             COLIB_DEBUG("Failed to register wait: %s on: %s",
                     dbg_enum(ret_err).c_str(), dbg_name(h).c_str());
             do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
             return h;
         }
-        do_leave_modifs(state);
         triggered = true;
         return pool->get_internal()->next_task();
     }
@@ -4398,8 +4393,8 @@ struct io_awaiter_t {
         COLIB_DEBUG_TRACE_SCOPE("io-unwait state: %p", state);
 
         if (triggered) {
-            do_entry_modifs(state);
             do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
         }
         if (ret_err != ERROR_OK)
             return ret_err;
@@ -4459,9 +4454,7 @@ struct sem_internal_t {
         while (waiting_on_sem.size()) {
             auto to_awake = waiting_on_sem.back();
             waiting_on_sem.pop_back();
-            do_entry_modifs(to_awake.first);
             do_unwait_sem_modifs(to_awake.first, selfptr);
-            do_leave_modifs(to_awake.first);
             destroy_state(to_awake.first);
         }
         this->val = val;
@@ -4554,19 +4547,17 @@ struct sem_awaiter_t {
         auto pool = sem->get_internal()->get_pool();
         psem_it = sem->get_internal()->push_waiter(state);
 
+        do_leave_modifs(state);
         error_e err = do_wait_sem_modifs(state, sem, psem_it);
         if (err != ERROR_OK) {
             sem->get_internal()->erase_waiter(*psem_it);
             do_unwait_sem_modifs(state, sem);
-            if (err == ERROR_SUSPENDED) {
-                /* parked by a modif */
-                do_leave_modifs(state);
-                return std::noop_coroutine();
-            }
+            if (err == ERROR_SUSPENDED)
+                return std::noop_coroutine();   /* parked by a modif */
             COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
+            do_entry_modifs(state);
             return to_suspend;
         }
-        do_leave_modifs(state);
 
         await_state = AWAITER_SUSPEND_LAST;
         return pool->get_internal()->next_task();
@@ -4575,8 +4566,8 @@ struct sem_awaiter_t {
     sem_t::unlocker_t await_resume() {
         COLIB_DEBUG_TRACE_SCOPE("sem-unwait state: %p", state);
         if (await_state == AWAITER_SUSPEND_LAST) {
-            do_entry_modifs(state);
             do_unwait_sem_modifs(state, sem);
+            do_entry_modifs(state);
             return sem_t::unlocker_t(sem);
         }
         else if (await_state == AWAITER_READY_LAST)
@@ -4710,16 +4701,15 @@ inline task_t force_stop(int64_t stopval
 
             state = &h.promise().state;
 
+            do_leave_modifs(state);
             error_e err = do_yield_modifs(state);
-            if (err == ERROR_SUSPENDED) {
-                /* parked by a modif, the pool isn't stopped */
-                do_leave_modifs(state);
-                return std::noop_coroutine();
-            }
-            if (err != ERROR_OK)
+            if (err == ERROR_SUSPENDED)
+                return std::noop_coroutine();   /* parked by a modif, the pool isn't stopped */
+            if (err != ERROR_OK) {
+                do_entry_modifs(state);
                 return h;   /* refused by a modif */
+            }
 
-            do_leave_modifs(state);
             state->pool->get_internal()->push_ready_front(state);
             state->pool->get_internal()->ret_val = RUN_STOPPED;
             state->pool->get_internal()->post_stop();
@@ -6166,18 +6156,10 @@ inline error_e killer_state_t::drive() {
 
 inline void killer_state_t::drop(state_t *top) {
     /* replay the wake-up, so the modifs see the wait end */
-    if (io_desc) {
-        io_desc_t &io = *io_desc;
-        do_entry_modifs(top);
-        do_unwait_io_modifs(top, io);
-        do_leave_modifs(top);
-    }
-    else if (sem) {
-        sem_t *s = sem;
-        do_entry_modifs(top);
-        do_unwait_sem_modifs(top, s);
-        do_leave_modifs(top);
-    }
+    if (io_desc)
+        do_unwait_io_modifs(top, *io_desc);
+    else if (sem)
+        do_unwait_sem_modifs(top, sem);
     unwind();
 }
 
@@ -6689,6 +6671,8 @@ inline void dbg_check_modif_leave(state_
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but left");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but left");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but left");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "left while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "left while waiting on sem");
     dbg_state.left = true;
 }
 
@@ -6699,6 +6683,8 @@ inline void dbg_check_modif_enter(state_
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but entered");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered&& !dbg_state.left), "entered twice");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "entered while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "entered while waiting on sem");
     dbg_state.entered = true;
     dbg_state.left = false;
 }
@@ -6710,7 +6696,7 @@ inline void dbg_check_modif_wait_io(stat
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but io");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
@@ -6724,7 +6710,7 @@ inline void dbg_check_modif_unwait_io(st
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but un-io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but un-io");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but un-io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "un-waited io while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "un-waited io while waiting on sem (it)");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.io, "un-waited io while not waiting on io");
@@ -6740,7 +6726,7 @@ inline void dbg_check_modif_wait_sem(sta
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but sem");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
@@ -6756,7 +6742,7 @@ inline void dbg_check_modif_unwait_sem(s
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but un-sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but un-sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but un-sem");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "un-waited sem while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem, "un-waited sem while not waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem_it, "un-waited sem while not waiting on sem (it)");
@@ -6772,7 +6758,7 @@ inline void dbg_check_modif_wait_yield(s
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but yield");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but yield");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "yield before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "yield while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "yield while waiting on sem");
 }
```

## Tests that change

`011-003` is in the repo today, and it asserts the current (crossing) order. `011-006` and `011-007`
are the new tests from `killer_diff.md` and part 1.

```diff
--- a/tests/011-003-modifs_lifecycle.cpp
+++ b/tests/011-003-modifs_lifecycle.cpp
@@ -11,10 +11,10 @@
 ================================================================================================= */
 
 /* Covers the 7 modif_e types not exercised by 11-1-modifs.cpp (CALL/SCHED). Ordering is verified
-against colib.h's actual awaiter implementations, not just the doc comments - see BUGS.md #1 for a
-doc/implementation mismatch found while writing this: WAIT_IO/WAIT_SEM fire BEFORE LEAVE on suspend
-(io_awaiter_t::await_suspend, sem_awaiter_t::await_suspend), not after as CO_MODIF_WAIT_IO_CBK's doc
-comment claims. Resume order is ENTER then UNWAIT_IO/UNWAIT_SEM. Exit (on plain co_return) is LEAVE
+against colib.h's actual awaiter implementations, not just the doc comments. A coroutine is either
+running (between ENTER and LEAVE) or waiting (between WAIT and UNWAIT), never both: on suspend
+LEAVE fires before WAIT_IO/WAIT_SEM, on resume UNWAIT_IO/UNWAIT_SEM fires before ENTER. This used to
+be the other way around (BUGS.md #1, doc and code disagreed). Exit (on plain co_return) is LEAVE
 then EXIT.
 
 Flags use CO_MODIF_INHERIT_ON_CALL: co::wait_event() is itself a small wrapping coroutine (not a raw
@@ -94,12 +94,12 @@
 
     std::vector<std::string> expected = {
         "ENTER",                                   /* test30_child starts */
-        "WAIT_SEM", "LEAVE",                        /* suspends on sem->wait() */
-        "ENTER", "UNWAIT_SEM",                      /* resumes once signaled */
+        "LEAVE", "WAIT_SEM",                        /* suspends on sem->wait() */
+        "UNWAIT_SEM", "ENTER",                      /* resumes once signaled */
         "LEAVE",                                    /* test30_child leaves to call wait_event() */
         "ENTER",                                    /* wait_event()'s own task starts */
-        "WAIT_IO", "LEAVE",                         /* wait_event() suspends on the io_awaiter_t */
-        "ENTER", "UNWAIT_IO",                       /* wait_event() resumes once fd is readable */
+        "LEAVE", "WAIT_IO",                         /* wait_event() suspends on the io_awaiter_t */
+        "UNWAIT_IO", "ENTER",                       /* wait_event() resumes once fd is readable */
         "LEAVE", "EXIT",                            /* wait_event()'s task completes */
         "ENTER",                                    /* test30_child resumes after the call returns */
         "LEAVE", "EXIT"                              /* test30_child completes */
--- a/tests/011-006-modifs_force_suspend.cpp
+++ b/tests/011-006-modifs_force_suspend.cpp
@@ -75,9 +75,9 @@
     pool->sched(co::add_modifs(pool.get(), test50_refused_yield(), refuse));
     ASSERT_FN(pool->run());
 
-    /* sched's ENTER, then the park: WAIT_SEM, UNWAIT_SEM, LEAVE - no ENTER, it never resumed */
+    /* sched's ENTER, then the park: LEAVE, WAIT_SEM, UNWAIT_SEM - no ENTER, it never resumed */
     DBG("order: %s", test50_order.c_str());
-    ASSERT_FN(CHK_BOOL(test50_order == "EWUL"));
+    ASSERT_FN(CHK_BOOL(test50_order == "ELWU"));
     ASSERT_FN(CHK_BOOL(test50_after_yield == 1));
     return 0;
 }
--- a/tests/011-007-modifs_close_order.cpp
+++ b/tests/011-007-modifs_close_order.cpp
@@ -52,10 +52,10 @@
     pool->sched(test51_signaler(sem));
     ASSERT_FN(pool->run());
 
-    /* sched's ENTER; the wait: WAIT, LEAVE; the resume: ENTER, UNWAIT; the return: LEAVE, EXIT */
+    /* sched's ENTER; the wait: LEAVE, WAIT; the resume: UNWAIT, ENTER; the return: LEAVE, EXIT */
     DBG("order: %s", test51_order.c_str());
     ASSERT_FN(CHK_BOOL(test51_order ==
-            "Ea Eb Wa Wb Lb La Ea Eb Ub Ua Lb La Xb Xa "));
+            "Ea Eb Lb La Wa Wb Ub Ua Ea Eb Lb La Xb Xa "));
     return 0;
 }
 
```

## Verification

The scratch `tests/` has `killer_diff.md` (sections 3 and 4), part 1 and part 2 applied, with the
test changes above:

| | Result |
|---|---|
| Whole suite (`002-005` included) | all pass except `018-011`, which fails the same way on today's `colib.h` (`BUGS.md` #5) |
| `011-003`, `011-006`, `011-007` | pass, with the new orders |
| 13 killer/wait/clear tests, rebuilt with `COLIB_ENABLE_DEBUG_CHECKS` on, so the new check rules run on them | all pass: `011-003`, `018-016`, `002-002`, `002-003`, `002-004`, `003-003`, `018-005`, `018-006`, `018-015`, `001-007`, `004-001`, `009-001`, `002-001` |

Only three tests in the suite turn the debug checks on by default (`011-006`, `011-007`,
`018-011`). That's why the 13 were rebuilt with them on.

## Docs to update when it lands

- `tests/BUGS.md` #1 is fixed: remove the entry. The number stays unused, as that file's rule says.
- `docs/02_api.md`, at `CO_MODIF_WAIT_IO_CBK`: drop the "Flagged - see `tests/BUGS.md` #1" note.
- The design notes at the end of `colib.h` already describe this order for `io_awaiter` and
  `sem_awaiter`. The only difference is their aborted path, which has ENTER without UNWAIT; here
  every WAIT gets its UNWAIT.
