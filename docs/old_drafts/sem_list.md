# The semaphore list

The first change of the new phase: the semaphores' bookkeeping, cleaned up on top of today's
`colib.h` (with the combined diff already applied). Nothing is applied yet. The diff below was made
on a scratch copy and tested there.

## What it fixes and removes

**`BUGS.md` #8: a heap use-after-free.** A semaphore's `clear()` destroys its waiters. When a
waiter held the last `sem_p`, destroying it freed the semaphore in the middle of its own `clear()`.
`clear()` then kept reading its wait list and wrote `val`, all in freed memory. It was silent in
normal builds. Now `clear()` does its work in this order:
1. takes the waiters out into a local list, running each one's UNWAIT while the semaphore still
   exists;
2. sets `val`;
3. destroys the waiters.

Nothing touches `this` once destruction starts. The same holds for a user calling `sem_t::clear()`
directly. `018-018` is the regression test.

**`sem_pool` becomes an intrusive list.** The pool still needs to know its semaphores, for two
things nobody else can do:
- end their waiters when the pool dies;
- tell a semaphore that outlives the pool that the pool is gone.

But that needs no `std::set` allocated from the pool's memory: each `sem_internal_t` carries
`prev`/`next`, and the pool keeps the head. The pool's `clear()` becomes a plain loop:
1. unlink the semaphore, so its destructor won't look for the pool;
2. invalidate it;
3. clear it, which may free it.

The old `has(sem_pool, s)` check, needed because a semaphore could remove itself from the set
halfway through, is gone.

**`sem_t::invalidate_self()` is removed.** It was public, documented as "better don't touch", and
only the pool called it. The pool now calls `sem_internal_t::invalidate()` directly.

**The semaphore waiter handle goes away.** Since the intrusive queues, `sem_waiter_handle_p` was
just the waiting `state_t *`, the same pointer WAIT_SEM callbacks already get as their first
argument. So these go:
- the `sem_waiter_handle_p` alias;
- the third parameter of `CO_MODIF_WAIT_SEM_CBK`;
- the awaiter's `psem_it`;
- the debug checks' `sem_it` copy (11 lines of asserts that only duplicated the `sem` checks).

## API change

**A `CO_MODIF_WAIT_SEM_CBK` callback now takes `(state_t *, sem_t *)`**; the third parameter is
gone. A user modif written with three parameters stops compiling until it drops it. Four tests
spelled out the old signature and are updated in the tests diff below.

## Verification

Built through the scratch `tests/` makefile on Windows (MSVC 19.43):

| | Result |
|---|---|
| Whole suite | **all pass except `018-011` and `018-017`** (`BUGS.md` #5, #6, unrelated). `018-018` now **passes** |
| `018-018` on today's `colib.h` | segfaults (it poisons freed memory itself) |
| A probe with two semaphores owned only by their waiters, then the pool dies, under AddressSanitizer | today's `colib.h`: `heap-use-after-free` in `sem_internal_t::clear()`. With this diff: clean |
| `018-018` built with AddressSanitizer | passes, no report |

The pseudocode at the end of `colib.h` still mentions `sem_it`. That block is already out of date in
several places, so it isn't touched here.

---

## The `colib.h` diff

The hunks are in file order, so together they make the whole patch (26 hunks).

### 1. `sem_t`: no `invalidate_self()`

```diff
--- a/colib.h
+++ b/colib.h
@@ -1007,7 +1007,6 @@ struct sem_t {
     /*! Again, beeter don't touch, same as pool. This is public only to ease the writing of the
      * implementation. @{ */
     sem_internal_t *get_internal();
-    void invalidate_self();
     /*! @} */
 
 protected:
```

### 2. No waiter handle: the alias, and the WAIT_SEM callback's signature

```diff
--- a/colib.h
+++ b/colib.h
@@ -1144,8 +1143,6 @@ private:
     state_list_t *list = nullptr;           /* the queue it is linked in, if any */
 };
 
-/*! The waiter in a semaphore's wait list, given as a parameter to the wait_sem modif callback */
-using sem_waiter_handle_p = state_t *;
             
 
 /*! Modifs, corutine modifications. Those modifications controll the way a corutine behaves when
@@ -1167,7 +1164,7 @@ struct modif_t {
         std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */
 
          /* wait_sem_cbk */
-        std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,
+        std::function<error_e(state_t *, sem_t *)>,
 
          /* unwait_sem_cbk - No handle here, as the semaphore is no longer in the waiting list */
         std::function<error_e(state_t *, sem_t *)>,
```

### 3. Debug-check declarations

The WAIT_SEM check loses its handle, and the recorded state loses `sem_it`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2259,7 +2256,7 @@ struct dbg_scope_t {
 # define COLIB_DEBUG_CHECK_ENTER(s) dbg_check_modif_enter(s)
 # define COLIB_DEBUG_CHECK_WAIT_IO(s, io) dbg_check_modif_wait_io(s, io)
 # define COLIB_DEBUG_CHECK_UNWAIT_IO(s, io) dbg_check_modif_unwait_io(s, io)
-# define COLIB_DEBUG_CHECK_WAIT_SEM(s, sem, it) dbg_check_modif_wait_sem(s, sem, it)
+# define COLIB_DEBUG_CHECK_WAIT_SEM(s, sem) dbg_check_modif_wait_sem(s, sem)
 # define COLIB_DEBUG_CHECK_UNWAIT_SEM(s, sem) dbg_check_modif_unwait_sem(s, sem)
 # define COLIB_DEBUG_CHECK_WAIT_YIELD(s) dbg_check_modif_wait_yield(s)
 
@@ -2279,7 +2276,7 @@ inline void dbg_check_modif_leave(state_
 inline void dbg_check_modif_enter(state_t *s);
 inline void dbg_check_modif_wait_io(state_t *s, io_desc_t &io);
 inline void dbg_check_modif_unwait_io(state_t *s, io_desc_t &io);
-inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem, sem_waiter_handle_p it);
+inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem);
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem);
 inline void dbg_check_modif_wait_yield(state_t *s);
 
@@ -2292,7 +2289,6 @@ struct dbg_check_state_t {
     io_desc_t *io = nullptr; /* it's ok, to compare ptrs, else we would copy and incr the
                                 ref of a pointer on windows that would alter the behaviour */
     sem_t *sem = nullptr;
-    state_t *sem_it = nullptr;
 };
 
 inline std::map<pool_t *,
```

### 4. `do_wait_sem_modifs`

```diff
--- a/colib.h
+++ b/colib.h
@@ -2667,10 +2663,10 @@ inline error_e do_unwait_io_modifs(state
     return do_generic_modifs<CO_MODIF_UNWAIT_IO_CBK>(state, io_desc);
 }
 
-inline error_e do_wait_sem_modifs(state_t *state, sem_t *sem, sem_waiter_handle_p _it) {
+inline error_e do_wait_sem_modifs(state_t *state, sem_t *sem) {
     COLIB_DEBUG_TRACE("    SEM: %s state: %p sem:%p", dbg_name(state->self).c_str(), state, sem);
-    COLIB_DEBUG_CHECK_WAIT_SEM(state, sem, _it);
-    return do_generic_modifs<CO_MODIF_WAIT_SEM_CBK>(state, sem, _it);
+    COLIB_DEBUG_CHECK_WAIT_SEM(state, sem);
+    return do_generic_modifs<CO_MODIF_WAIT_SEM_CBK>(state, sem);
 }
 
 inline error_e do_unwait_sem_modifs(state_t *state, sem_t *sem) {
```

### 5. `pool_internal_t`: the semaphore list

`add_sem`/`rm_sem` take a `sem_internal_t *`, and are defined after it. The `std::set` and its pool allocator become one head pointer.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3896,8 +3892,7 @@ struct pool_internal_t : public pool_t {
     :   pool(this),
         ready_tasks{STATE_READY},
         io_pool{this, ready_tasks},
-        timer_pool(this, io_pool),
-        sem_pool{allocator_t<sem_t *>{this}}
+        timer_pool(this, io_pool)
     {}
 
     ~pool_internal_t() { clear(); }
@@ -4117,13 +4112,9 @@ struct pool_internal_t : public pool_t {
         return timer_pool.free_timer(timer);
     }
 
-    void add_sem(sem_t *s) {
-        sem_pool.insert(s);
-    }
-
-    void rm_sem(sem_t *s) {
-        sem_pool.erase(s);
-    }
+    /* every semaphore of the pool, so clear() can end their waiters; defined after sem_internal_t */
+    void add_sem(sem_internal_t *s);
+    void rm_sem(sem_internal_t *s);
 
     /* This function needs the semaphore definition so it is implemented bellow the semaphore */
     error_e clear();
@@ -4146,7 +4137,7 @@ private:
     bool posted_stop = false;
 
     /* bookkeeping for end of life destruction */
-    std::set<sem_t *, std::less<sem_t *>, allocator_t<sem_t *>> sem_pool;
+    sem_internal_t *sems = nullptr;
 
 #if COLIB_ENABLE_MULTITHREAD_SCHED
     std::mutex lock;
```

### 6. `sem_internal_t`: its destructor, `clear()`, the waiters, the list, and the pool's `clear()`

The destructor unlinks first, then clears. `clear()` closes the waiters, sets `val`, then destroys them. `push_waiter()` returns nothing. `prev`/`next` live in the semaphore, and the pool's `clear()` is the plain loop.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4538,8 +4529,8 @@ struct sem_internal_t : public sem_t {
             COLIB_DEBUG_TRACE("already handled");
             return ;
         }
-        clear(0);
         pool->get_internal()->rm_sem(this);
+        clear(0);
     }
 
     /* the pool died: nothing of it may be touched anymore */
@@ -4578,11 +4569,16 @@ struct sem_internal_t : public sem_t {
 
     error_e clear(int64_t val = 0) {
         COLIB_DEBUG_TRACE_SCOPE("sem_t::clear");
-        while (state_t *to_awake = waiting_on_sem.pop_front()) {
-            do_unwait_sem_modifs(to_awake, this);
-            destroy_state(to_awake);
+        /* a waiter may hold the last sem_p: once one is destroyed `this` may be gone, so the waiters
+        are taken out and closed first, and destroyed last */
+        state_list_t closed{STATE_WAITING_SEM};
+        while (state_t *s = waiting_on_sem.pop_front()) {
+            do_unwait_sem_modifs(s, this);
+            closed.push_back(s);
         }
         this->val = val;
+        while (state_t *s = closed.pop_front())
+            destroy_state(s);
         return ERROR_OK;
     }
 
@@ -4596,9 +4592,8 @@ protected:
     friend sem_t;
     friend inline sem_p create_sem(pool_t *pool, int64_t val);
 
-    sem_waiter_handle_p push_waiter(state_t *state) {
+    void push_waiter(state_t *state) {
         waiting_on_sem.push_back(state);
-        return state;
     }
 
 
@@ -4612,29 +4607,39 @@ private:
         return ERROR_OK;
     }
 
+    friend struct pool_internal_t;
+
     pool_t *pool = nullptr;
     int64_t val;
     state_list_t waiting_on_sem;
+    sem_internal_t *prev = nullptr;     /* in its pool's list of semaphores */
+    sem_internal_t *next = nullptr;
 };
 
+inline void pool_internal_t::add_sem(sem_internal_t *s) {
+    s->next = sems;
+    if (sems)
+        sems->prev = s;
+    sems = s;
+}
+
+inline void pool_internal_t::rm_sem(sem_internal_t *s) {
+    (s->prev ? s->prev->next : sems) = s->next;
+    if (s->next)
+        s->next->prev = s->prev;
+    s->prev = s->next = nullptr;
+}
+
 inline error_e pool_internal_t::clear() {
     run_posted();   /* the parked ones go back to their owners first */
     if (io_pool.clear() != ERROR_OK) {
         COLIB_DEBUG("WARNING: FAILED to clear events waiting for io");
     }
-    while (sem_pool.size()) {
-        auto s = *sem_pool.begin();
-    	COLIB_DEBUG_TRACE("Clearing semaphore: %p", s);
-        if (s->get_internal()->clear(0) != ERROR_OK) {
-            COLIB_DEBUG("WARNING: FAILED to clear events waiting on one of the semaphores");
-            return ERROR_GENERIC;
-        }
-        /* We first need to make sure that the semaphore is still in our reach and it didn't
-        manage to destroy itself, remember `s` is non-owning */
-        if (has(sem_pool, s))
-            s->invalidate_self(); 	// we invalidate the semaphore so that if there are any
-								    // dangling pointers, they won't do anything anymore
-        sem_pool.erase(s);
+    while (sem_internal_t *s = sems) {
+        COLIB_DEBUG_TRACE("Clearing semaphore: %p", s);
+        rm_sem(s);          /* its destructor won't look for the pool anymore */
+        s->invalidate();    /* a semaphore that outlives the pool does nothing */
+        s->clear(0);        /* may free s itself (a waiter held the last sem_p) */
     }
     COLIB_DEBUG_TRACE("Cleaning wait queue");
     while (state_t *state = ready_tasks.pop_front())
```

### 7. `sem_awaiter_t`

The waiting state is the handle.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4664,10 +4669,10 @@ struct sem_awaiter_t {
         auto pool = sem->get_internal()->get_pool();
 
         do_leave_modifs(state);
-        psem_it = sem->get_internal()->push_waiter(state);
-        error_e err = do_wait_sem_modifs(state, sem, psem_it);
+        sem->get_internal()->push_waiter(state);
+        error_e err = do_wait_sem_modifs(state, sem);
         if (err != ERROR_OK) {
-            sem->get_internal()->erase_waiter(psem_it);
+            sem->get_internal()->erase_waiter(state);
             do_unwait_sem_modifs(state, sem);
             if (err == ERROR_SUSPENDED) {
                 push_parked(state);
@@ -4706,7 +4711,6 @@ struct sem_awaiter_t {
 
     state_t *state = nullptr;
     sem_t *sem = nullptr;
-    sem_waiter_handle_p psem_it = nullptr;
     await_state_e await_state = AWAITER_NOT_CALLED;
 };
 
```

### 8. `sem_t`'s methods

```diff
--- a/colib.h
+++ b/colib.h
@@ -4732,10 +4736,6 @@ inline sem_internal_t *sem_t::get_intern
     return static_cast<sem_internal_t *>(this);
 }
 
-inline void sem_t::invalidate_self() {
-    get_internal()->invalidate();
-}
-
 inline sem_p create_sem(pool_t *pool, int64_t val) {
 	/* We no longer allocate semaphores with the internal allocator,
 	because they may survive outside of the pool, and we would not have how
```

### 9. The killer and the tracer

The same signature change for their WAIT_SEM callbacks.

```diff
--- a/colib.h
+++ b/colib.h
@@ -6341,9 +6341,9 @@ inline std::pair<modif_pack_t, std::func
         }
     ));
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
-        [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
-            COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p it-ptr: %p",
-                    kstate.get(), s, sem, it);
+        [kstate](state_t *s, sem_t *sem) -> error_e {
+            COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p",
+                    kstate.get(), s, sem);
             if (kstate->dying)
                 return kstate->park(s);
             kstate->sem = sem;
@@ -6532,7 +6532,7 @@ inline modif_pack_t dbg_create_tracer(po
         }
     ));
     mods.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
-        [] (state_t *s, sem_t *, sem_waiter_handle_p) -> error_e {
+        [] (state_t *s, sem_t *) -> error_e {
             COLIB_DEBUG(">   SEM: %s", dbg_name(s->self).c_str());
             return ERROR_OK;
         }
```

### 10. Debug checks

Every `sem_it` assert goes; the `sem` asserts next to them already check the same.

```diff
--- a/colib.h
+++ b/colib.h
@@ -6686,11 +6686,9 @@ inline void dbg_check_modif_call(state_t
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sched, "");
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "never alive io?");
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "never alive sem?");
-        COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "never alive sem? (it)");
     }
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "called while waiting io?");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "called while waiting sem?");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "called while waiting sem? (it)");
     dbg_state.summon_cnt++;
     dbg_state.called = true;
 }
@@ -6708,11 +6706,9 @@ inline void dbg_check_modif_sched(state_
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sched, "");
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "never alive io?");
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "never alive sem?");
-        COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "never alive sem? (it)");
     }
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "sched while waiting io?");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "sched while waiting sem?");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "sched while waiting sem? (it)");
     dbg_state.sched = true;
     dbg_state.summon_cnt++;
 }
@@ -6725,7 +6721,6 @@ inline void dbg_check_modif_exit(state_t
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but exited");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "exited while waiting io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "exited while waiting sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "exited while waiting sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && !dbg_state.left),
             "exited but didn't leave");
     pool_map.erase(s);
@@ -6767,7 +6762,6 @@ inline void dbg_check_modif_wait_io(stat
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
     dbg_state.io = &io;
 }
 
@@ -6780,16 +6774,14 @@ inline void dbg_check_modif_unwait_io(st
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but un-io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "un-waited io while waiting on sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "un-waited io while waiting on sem (it)");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.io, "un-waited io while not waiting on io");
     dbg_state.io = nullptr;
 }
 
-inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem, sem_waiter_handle_p it) {
+inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem) {
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(s, "no-state");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(s->pool, "no-pool");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(sem, "wait on no sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(it, "wait on invalid it");
     auto &pool_map = dbg_check_coro_states[s->pool];
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but sem");
@@ -6797,9 +6789,7 @@ inline void dbg_check_modif_wait_sem(sta
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
     dbg_state.sem = sem;
-    dbg_state.sem_it = it;
 }
 
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem) {
@@ -6813,10 +6803,8 @@ inline void dbg_check_modif_unwait_sem(s
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "un-waited sem while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem, "un-waited sem while not waiting on sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem_it, "un-waited sem while not waiting on sem (it)");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem == sem, "un-waited a different semaphore");
     dbg_state.sem = nullptr;
-    dbg_state.sem_it = nullptr;
 }
 
 inline void dbg_check_modif_wait_yield(state_t *s) {
```

---

## The tests diff

These four tests drop the third parameter from their WAIT_SEM callbacks. `018-018` is already in
`tests/` and needs no change.

```diff
--- a/tests/011-003-modifs_lifecycle.cpp
+++ b/tests/011-003-modifs_lifecycle.cpp
@@ -50,7 +50,7 @@
             test30_log.push_back("UNWAIT_IO"); return co::ERROR_OK;
         }));
     pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
-        [](co::state_t*, co::sem_t*, co::sem_waiter_handle_p) -> co::error_e {
+        [](co::state_t*, co::sem_t*) -> co::error_e {
             test30_log.push_back("WAIT_SEM"); return co::ERROR_OK;
         }));
     pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
--- a/tests/011-006-modifs_force_suspend.cpp
+++ b/tests/011-006-modifs_force_suspend.cpp
@@ -56,7 +56,7 @@
 
     co::modif_pack_t pack;
     pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
-        [](co::state_t *s, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
+        [](co::state_t *s, co::sem_t *) -> co::error_e {
             test50_order += "W";
             test50_parked = s;
             return co::ERROR_SUSPENDED;
--- a/tests/011-007-modifs_close_order.cpp
+++ b/tests/011-007-modifs_close_order.cpp
@@ -22,7 +22,7 @@
     pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
         [log](co::state_t *) -> co::error_e { log('L'); return co::ERROR_OK; }));
     pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
-        [log](co::state_t *, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
+        [log](co::state_t *, co::sem_t *) -> co::error_e {
             log('W'); return co::ERROR_OK; }));
     pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
         [log](co::state_t *, co::sem_t *) -> co::error_e { log('U'); return co::ERROR_OK; }));
--- a/tests/018-008-reproduced_unlocker_spurious_signal.cpp
+++ b/tests/018-008-reproduced_unlocker_spurious_signal.cpp
@@ -40,7 +40,7 @@
     auto sem = co::create_sem(pool, 0);
 
     auto abort_wait = co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
-        [](co::state_t*, co::sem_t*, co::sem_waiter_handle_p) -> co::error_e {
+        [](co::state_t*, co::sem_t*) -> co::error_e {
             return co::ERROR_GENERIC; /* reject every wait attempt on this coroutine */
         });
 
```
