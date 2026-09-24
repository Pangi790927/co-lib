# Killer restructure, no parking in the pool, ENTER on resume, `state_t` layout

> **Correction (after this was written).** Two claims below are wrong, and the code has changed since:
>
> - **"No parking in the pool" caused a heap use-after-free.** Destroying a coroutine inside its own
>   handle-returning `await_suspend` is not safe on MSVC, which stores the returned handle in the
>   frame after `await_suspend` returns. ASan caught it in `002-004`/`002-007` (silent in normal
>   builds), and `003-004` segfaulted. Fixed:
>   - A root with no caller no longer suspends at its end (`final_awaiter_ends()` in
>     `final_suspend`'s `await_ready`), so the compiler frees it after it leaves.
>   - Parked coroutines go back to a pool list (`push_parked`/`run_parked`), and their PARKED
>     callbacks run after the resume returns. `STATE_PARKED` is back.
>   - The killer's `on_exit` no longer needs the `done()`/`ERROR_YIELDED` check: while it drives,
>     nobody else can destroy the chain.
> - **`sizeof(state_t)` is 112, not 104.** The extra 8 bytes are `wait_on`, which only goes away with
>   the `close_wait` ownership rework.
>
> The diff below is from before these fixes.

Applied directly to `colib.h`, as asked. The diff at the end is against `colib.h` right after
`killer.md` was applied.

## What changed

**1. The killer lives in `killer_state_t`.**
- Its modif callbacks (`on_call`, `on_sched`, `on_exit`, `on_wait`, `on_parked`), its kill function
  (`kill`), and three helpers (`drive`, `end_chain`, `queue_caller`) are all defined inside the
  struct.
- `create_killer()` only wires each callback to its member, one line each. The `dying_wait` lambda
  is gone; it's `on_wait`.
- `end_chain()` owns the whole ending: it sets the root's `err`, queues the root's caller, closes
  the wait, and destroys the chain.

**2. The drive happens before the parent is queued, as you described.**
- While the target runs, a placeholder (`turn`) holds its place in the ready queue, and the parent
  isn't queued. The root is the boundary.
- **If it ends by itself** (returns, throws, or `co_yield`s), `on_exit` gives the parent that place
  and answers `ERROR_SUSPENDED`, so `final_awaiter_cleanup` doesn't jump into it. The parent resumes
  at its turn and finds the result, or the exception, like any finished call.
- **If it reaches a wait**, it's destroyed there, and `end_chain()` gives the parent the same place,
  with `err = e`.
- You were right that running the parent inside the kill is dicey: the parent may carry its own
  killer, or be the one that called `kill()`.

**3. No parking in the pool.**
- The park path's last action (in each awaiter, and in `task<T>::await_suspend` for a suspended
  call) calls the PARKED callbacks right there, inside `await_suspend`.
- The modif that parked the coroutine may destroy it on the spot and return `ERROR_FINISHED`. That
  stops the PARKED dispatch, so no later callback sees a freed state. That answers your "sure?":
  the dispatcher guarantees it, not a comment.
- Destroying a coroutine inside its own `await_suspend` is allowed, as long as nothing touches it
  afterwards, and the park paths only return `noop` after that call.
- A finished scheduled root destroys itself in `final_awaiter_cleanup` the same way, after handing
  its exception to the pool.
- **Gone:** the parked list, the done list, `push_parked()`, `post_to_destroy()`, `run_posted()`,
  `STATE_PARKED` and `STATE_DONE`. The pool no longer owns any coroutine it isn't scheduling.

**4. ENTER on the first resume.**
- A coroutine's first resume passes through `initial_suspend()`'s awaiter, which fires ENTER, both
  for one the pool starts and for one its caller calls. So `sched()`, `task<T>::await_suspend` and
  `next_task_state()` no longer fire or check anything, and `enter_on_start` is gone.
- **A generator's later resumes** now get their ENTER from the `co_yield` awaiter. The debug checks
  caught that it was missing, on `009-001`.
- That awaiter also resets `err` from `ERROR_YIELDED` to `ERROR_OK`: a resumed generator is no
  longer yielded.
- The order is unchanged. A call is LEAVE(caller), CALL(callee), ENTER(callee).

**5. `state_t`: one private block at the top, then the public fields.**
- `sizeof(state_t)` is **104**: the two small private fields and `err` share one 8-byte slot.
- **Nothing was added to `state_t` this round.** I did add a `destroying` bit along the way, which
  was wrong, and it's removed. What it was for:
  - the killer needs to know whether a root it's resuming ended by itself, or was destroyed by
    another killer (`002-007`);
  - `s->self.done()` (it returned or threw) or `err == ERROR_YIELDED` (it yielded) already tells
    that.

## Warnings about tidiness

- **The killer block grew from 129 to 162 lines.** The members are defined inside the struct, the
  way you asked, and `on_exit`/`drive` gained the "ended by itself / ended by another killer" case.
  The logic is all in one place now, but it's longer.
- **The `ERROR_FINISHED` rule for PARKED is one special case in `do_generic_modifs`.**

## Verification

| | Result |
|---|---|
| The tests this touches (`002-003` .. `002-007`, `009-001`, `011-006`, `012-002`, `018-016`) | pass |
| `002-006` and `009-001` with `COLIB_ENABLE_DEBUG_CHECKS` (the `co_yield` paths) | pass |
| Whole suite and all 16 debug-check variants, on the no-parking version just before the `destroying` removal | all pass except `018-011`/`018-017` (open bugs #5, #6) |
| Whole suite on this exact file | **running**; this line gets updated when it finishes |
| `018-015` flake: one failure seen (964 sent, 963 received) | didn't reproduce: 0 of 40 on this round's file, 0 of 40 on the file before it. Unexplained, watching for it |

## Research: `close_wait()` and ownership (not changed)

You're right that `close_wait()` is backwards. It unlinks the coroutine first, and then answers
"which io or semaphore was it waiting on?" from the state's own copy (`wait`/`wait_on`). In an
ownership model, the owner knows and the owner lets go.

**Where a coroutine lives, and who knows what it waits on:**

| Situation | Linked in | Who knows the io/semaphore |
|---|---|---|
| waiting on a semaphore | that semaphore's wait list | the semaphore (the list's owner) |
| waiting on an io | not a list: epoll keeps it in `fd_data_t`, IOCP in `io_data_t` | the io pool, only by searching; the awaiter in the frame has the `io_desc_t` |
| **woken, not yet resumed** | the pool's ready queue | **only the awaiter in its frame**: the wake moved it to the ready queue, but its UNWAIT only runs when it resumes |

**The third row is the real problem.** The wake hands the coroutine from its owner to the ready
queue before its wait is closed, so between the wake and the resume nobody who owns it knows what it
waited on. That gap is why `wait_on` exists. It's the ownership inversion you're pointing at.

**A fix that respects ownership: whoever wakes it closes its wait.**
- `sem_internal_t::signal()`, the io pool delivering a completion, and `force_awake()`/`stop_io()`
  run the UNWAIT while they still own the coroutine, then hand it to the ready queue.
- A woken coroutine then has no open wait, and its awaiter's resume only fires ENTER.
- `close_wait()` then only exists for a coroutine that's still waiting. It asks the owner to let go,
  and the owner runs UNWAIT and unlinks, in that order.
- "Did the wake have an effect?" becomes one bit, recorded at the wake.

**Why this is still research and not a diff:**
1. **UNWAIT callbacks would run in the waker's context:** inside `signal()` (called by another
   coroutine), or inside the io pool's event loop. A user's UNWAIT callback that signals or kills
   would then re-enter loops that aren't written for it. Each loop would first have to take its
   waiters out, the way `sem_internal_t::clear()` does now.
2. **Waiting on an io has no owner pointer** unless io waits use lists, which we agreed to leave
   alone. Until then, a coroutine waiting on an io needs its `io_desc_t` from somewhere: the awaiter
   (that's the copy again) or the killer's own WAIT_IO callback (the original killer's way).
3. **Traces change:** UNWAIT moves from the resume to the wake. It's still before ENTER, so the order
   tests hold.

My read: semaphores and the ready queue can follow ownership cleanly, once their wake loops are safe
against re-entrance. Io can't, as long as io doesn't use lists. I need your call on point 2.

## The diff

The hunks are in file order (24 hunks).

### 1. `modif_e` (PARKED is immediate) and `state_e` (no PARKED/DONE)

```diff
--- a/colib.h
+++ b/colib.h
@@ -744,8 +744,9 @@ enum modif_e : int32_t {
     /*! This is similar to wait_io, but on co::yield() and co::force_stop() */
     CO_MODIF_WAIT_YIELD_CBK,
 
-    /*! This is called once a coroutine suspended by a modif (ERROR_SUSPENDED) is off the stack, after
-    the resume that suspended it. The modif that suspended it must resume or destroy it. */
+    /*! This is called right after a modif suspended a coroutine (ERROR_SUSPENDED), as the last thing
+    before it leaves the stack. The modif that suspended it owns it: it may destroy it right there
+    and return ERROR_FINISHED, and then no PARKED callback after it runs. */
     CO_MODIF_PARKED_CBK,
 
     CO_MODIF_COUNT,
@@ -1100,8 +1101,6 @@ enum state_e : uint8_t {
     STATE_READY,        /*!< in its pool's ready queue */
     STATE_WAITING_SEM,  /*!< in a semaphore's wait list */
     STATE_WAITING_IO,   /*!< waiting on an io */
-    STATE_PARKED,       /*!< suspended by a modif (ERROR_SUSPENDED), its owner not called back yet */
-    STATE_DONE,         /*!< a scheduled root that finished, destroyed after this resume */
 };
 
 /*! Internal state of corutines that is independent of the return value of the corutine.
```

### 2. `state_t`: one private block first

```diff
--- a/colib.h
+++ b/colib.h
@@ -1109,20 +1108,21 @@ enum state_e : uint8_t {
  * It also holds a user pointer user_ptr that can be used. This pointer can be useful when
  * working with modifications.*/
 struct state_t {
-    error_e err = ERROR_OK;                 /*!< holds the error return in diverse cases */
-
 private:
-    /* right after `err`: they fit in the padding before the next pointer */
     friend struct state_list_t;
     friend struct state_access_t;
 
     enum wait_e : uint8_t { WAIT_NONE, WAIT_IO, WAIT_SEM };
 
+    state_t *prev = nullptr;
+    state_t *next = nullptr;
+    state_list_t *list = nullptr;           /* the queue it is linked in, if any */
+    void *wait_on = nullptr;                /* the io_desc_t or the sem_t of that wait */
     state_e state = STATE_LEFT;
-    bool enter_on_start = false;            /* scheduled: ENTER when the pool first runs it */
     wait_e wait = WAIT_NONE;                /* the wait it is in, from WAIT to UNWAIT */
 
 public:
+    error_e err = ERROR_OK;                 /*!< holds the error return in diverse cases */
     pool_t *pool = nullptr;                 /*!< the pool of this coro */
     modif_table_p modif_table;              /*!< we allocate a table only if there are mods */
 
@@ -1139,12 +1139,6 @@ public:
     state_e get_state() const { return state; }
 
     ~state_t();                             /*!< Unlinks it from the queue it is in */
-
-private:
-    state_t *prev = nullptr;
-    state_t *next = nullptr;
-    state_list_t *list = nullptr;           /* the queue it is linked in, if any */
-    void *wait_on = nullptr;                /* the io_desc_t or the sem_t of that wait */
 };
 
             
```

### 3. `state_access_t`: no `enter_on_start`

```diff
--- a/colib.h
+++ b/colib.h
@@ -2381,12 +2375,6 @@ private:
 /* colib's own access to the private parts of state_t */
 struct state_access_t {
     static void set(state_t *s, state_e state) { s->state = state; }
-    static void set_enter_on_start(state_t *s) { s->enter_on_start = true; }
-    static bool take_enter_on_start(state_t *s) {
-        bool ret = s->enter_on_start;
-        s->enter_on_start = false;
-        return ret;
-    }
     static void unlink(state_t *s) {
         if (s->list)
             s->list->unlink(s);
```

### 4. The dispatch: a PARKED callback that ended the coroutine stops it

```diff
--- a/colib.h
+++ b/colib.h
@@ -2636,6 +2624,8 @@ inline error_e do_generic_modifs(state_t
         for (size_t i = 0; i < cbks.size(); i++) {
             auto &modif = reversed ? cbks[cbks.size() - 1 - i] : cbks[i];
             error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
+            if (cbk_id == CO_MODIF_PARKED_CBK && ret == ERROR_FINISHED)
+                return ret;     /* it ended the coroutine: nobody after it may see it */
             if (ret < 0 && !ignored_ret)
                 return ret;
             if (ret == ERROR_SUSPENDED)
@@ -2718,9 +2708,6 @@ inline error_e do_parked_modifs(state_t
     return do_generic_modifs<CO_MODIF_PARKED_CBK>(state);
 }
 
-/* a modif suspended it (ERROR_SUSPENDED): its owner is called back after this resume */
-inline void push_parked(state_t *state);
-
 /* considering you may want to create a modif at runtime this seems to be the best way */
 template <modif_e type_id, typename Cbk>
 inline modif_p create_modif(modif_flags_e flags, Cbk&& cbk) {
```

### 5. ENTER on resume: the `co_yield` awaiter, `initial_suspend()`, and the call

The call also parks a suspended callee through the PARKED callbacks directly.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2791,11 +2778,17 @@ struct task_state_t {
 
     struct cpp_yield_awaiter_t {
         bool await_ready() noexcept  { return false; }
-        void await_resume() noexcept {}
-        
+        void await_resume() noexcept {      /* its caller calls it again: it no longer yielded */
+            state->err = ERROR_OK;
+            do_entry_modifs(state);
+        }
+
         handle<void> await_suspend(handle<task_state_t<T>> yielding_task) noexcept {
-            return cpp_yield_awaiter(&yielding_task.promise().state);
+            state = &yielding_task.promise().state;
+            return cpp_yield_awaiter(state);
         }
+
+        state_t *state = nullptr;
     };
 
     task<T> get_return_object() {
@@ -2804,7 +2797,16 @@ struct task_state_t {
         return task<T>{h};
     }
 
-    std::suspend_always initial_suspend() noexcept { return {}; }
+    /* the first resume enters it: the pool starting a scheduled one, or its caller calling it */
+    struct initial_awaiter_t {
+        bool await_ready() noexcept  { return false; }
+        void await_suspend(handle<void>) noexcept {}
+        void await_resume() noexcept { do_entry_modifs(state); }
+
+        state_t *state;
+    };
+
+    initial_awaiter_t   initial_suspend() noexcept { return {&state}; }
     final_awaiter_t     final_suspend() noexcept   { return final_awaiter_t{}; }
     void                unhandled_exception()      { state.exception = std::current_exception(); }
 
@@ -2841,15 +2843,13 @@ inline handle<void> task<T>::await_suspe
 
     error_e err = do_call_modifs(state);
     if (err == ERROR_SUSPENDED) {
-        push_parked(state);
+        do_parked_modifs(state);
         return std::noop_coroutine();
     }
     if (err != ERROR_OK) {
         do_entry_modifs(&caller.promise().state);
         return caller;
     }
-
-    do_entry_modifs(state);
     return h;
 }
 
```

### 6. The pool: no flag, no posted work

```diff
--- a/colib.h
+++ b/colib.h
@@ -3940,7 +3940,6 @@ struct pool_internal_t : public pool_t {
         if (do_sched_modifs(state) != ERROR_OK) {
             return ;
         }
-        state_access_t::set_enter_on_start(state);
 
         /* third, we add the task to the pool */
         ready_tasks.push_back(state);
@@ -3978,7 +3977,6 @@ struct pool_internal_t : public pool_t {
             stopping. */
             state->self.resume();
 
-            run_posted();
             if (posted_exception) {
                 auto pe = posted_exception;
                 posted_exception = nullptr;
@@ -3994,30 +3992,12 @@ struct pool_internal_t : public pool_t {
         return ret_val;
     }
 
-    void post_to_destroy(state_t *s) {
-        done_tasks.push_back(s);
-    }
-
     void post_exception(std::exception_ptr exc) {
         if (posted_exception)
             std::terminate();   /* a second one in the same resume has nowhere to go */
         posted_exception = exc;
     }
 
-    void push_parked(state_t *state) {
-        parked_tasks.push_back(state);
-    }
-
-    /* running the posted work can post more */
-    void run_posted() {
-        while (!parked_tasks.empty() || !done_tasks.empty()) {
-            while (state_t *s = parked_tasks.pop_front())
-                do_parked_modifs(s);
-            while (state_t *s = done_tasks.pop_front())
-                s->self.destroy();
-        }
-    }
-
     void post_stop() {
         posted_stop = true;
     }
@@ -4092,8 +4072,6 @@ struct pool_internal_t : public pool_t {
             if (!ret->self)
                 std::terminate();   /* a killer's placeholder: the scheduler ran inside a kill */
             state_access_t::set(ret, STATE_RUNNING);
-            if (state_access_t::take_enter_on_start(ret))
-                do_entry_modifs(ret);
             COLIB_DEBUG_TRACE("next_state: %p", ret);
             return ret;
         }
@@ -4161,8 +4139,6 @@ private:
     timer_pool_t timer_pool;
 
     std::exception_ptr posted_exception = nullptr;
-    state_list_t done_tasks{STATE_DONE};
-    state_list_t parked_tasks{STATE_PARKED};
     bool posted_stop = false;
 
     /* bookkeeping for end of life destruction */
```

### 7. `final_awaiter_cleanup`: a finished root destroys itself

```diff
--- a/colib.h
+++ b/colib.h
@@ -4248,14 +4224,12 @@ inline handle<void> final_awaiter_cleanu
             return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
-    auto pool = ending_task_state->pool;
-    pool->get_internal()->post_to_destroy(ending_task_state);
     /* If the task that we are final_awaiting has no caller, then it is the moment to destroy it,
-    no one needs it's return value. Else it will be destroyed by the caller. */
-    if (ending_task_state->exception) {
-        pool->get_internal()->post_exception(ending_task_state->exception);
-        return std::noop_coroutine();
-    }
+    no one needs it's return value. Else it will be destroyed by the caller. It is suspended here,
+    so it can be destroyed right away, as long as nothing touches it after. */
+    if (ending_task_state->exception)
+        ending_task_state->pool->get_internal()->post_exception(ending_task_state->exception);
+    ending_task_state->self.destroy();
     return std::noop_coroutine();
 }
 
```

### 8. No `push_parked()`

```diff
--- a/colib.h
+++ b/colib.h
@@ -4309,10 +4283,6 @@ inline pool_internal_t *pool_t::get_inte
     return static_cast<pool_internal_t *>(this);
 }
 
-inline void push_parked(state_t *state) {
-    state->pool->get_internal()->push_parked(state);
-}
-
 /* the wait `s` was woken from already did its work before it resumed: a semaphore token taken, or
 an io that moved its data */
 inline bool wait_had_effect(state_t *s) {
```

### 9. The awaiters park through the PARKED callbacks; `pool_internal_t::clear()`

```diff
--- a/colib.h
+++ b/colib.h
@@ -4390,7 +4360,7 @@ struct yield_awaiter_t {
         do_leave_modifs(state);
         error_e err = do_yield_modifs(state);
         if (err == ERROR_SUSPENDED) {
-            push_parked(state);
+            do_parked_modifs(state);
             return std::noop_coroutine();   /* parked by a modif */
         }
         if (err != ERROR_OK) {
@@ -4503,7 +4473,7 @@ struct io_awaiter_t {
         if (err == ERROR_SUSPENDED) {
             /* parked by a modif */
             do_unwait_io_modifs(state, io_desc);
-            push_parked(state);
+            do_parked_modifs(state);
             return std::noop_coroutine();
         }
         if (err != ERROR_OK) {
@@ -4669,7 +4639,6 @@ inline void pool_internal_t::rm_sem(sem_
 }
 
 inline error_e pool_internal_t::clear() {
-    run_posted();   /* the parked ones go back to their owners first */
     if (io_pool.clear() != ERROR_OK) {
         COLIB_DEBUG("WARNING: FAILED to clear events waiting for io");
     }
@@ -4715,7 +4684,7 @@ struct sem_awaiter_t {
             sem->get_internal()->erase_waiter(state);
             do_unwait_sem_modifs(state, sem);
             if (err == ERROR_SUSPENDED) {
-                push_parked(state);
+                do_parked_modifs(state);
                 return std::noop_coroutine();   /* parked by a modif */
             }
             COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
@@ -4851,7 +4820,7 @@ inline task_t force_stop(int64_t stopval
             do_leave_modifs(state);
             error_e err = do_yield_modifs(state);
             if (err == ERROR_SUSPENDED) {
-                push_parked(state);
+                do_parked_modifs(state);
                 return std::noop_coroutine();   /* parked by a modif, the pool isn't stopped */
             }
             if (err != ERROR_OK) {
```

### 10. The killer

```diff
--- a/colib.h
+++ b/colib.h
@@ -6161,69 +6130,134 @@ inline task<std::pair<T, error_e>> creat
 }
 
 /* One per create_killer() call, shared by its modifs and its kill function */
+/* One per create_killer() call: the callbacks of its modifs and its kill function */
 struct killer_state_t {
     killer_state_t(error_e e) : e(e) {}
 
-    error_e e;                  /* the err of a killed root that was called */
-    state_t *root = nullptr;    /* the frame the killer was attached to */
-    state_t *top = nullptr;     /* the innermost frame of the chain, its callers lead to root */
-    bool dying = false;         /* its next wait parks */
-    bool driving = false;       /* kill() is resuming the chain */
-    bool killing = false;       /* kill() is destroying the chain */
+    /* ---------------------------------------------------------------- the modif callbacks */
 
-    error_e kill();
-    error_e drive();
-    void end_chain();
-};
+    error_e on_call(state_t *s) {
+        if (!root)
+            root = s;
+        top = s;
+        return ERROR_OK;
+    }
 
-inline error_e killer_state_t::kill() {
-    if (killing)
-        return ERROR_GENERIC;   /* called from our own destruction of the chain */
-    if (!top)
-        return ERROR_FINISHED;
+    error_e on_sched(state_t *s) {
+        root = top = s;
+        return ERROR_OK;
+    }
 
-    pool_internal_t *pool = top->pool->get_internal();
-    if (top->get_state() == STATE_RUNNING) {
-        dying = true;           /* executing: it dies at its next wait */
-        throw kill_deferred_t(top);
-    }
-    if (top->get_state() == STATE_WAITING_IO)
-        pool->stop_io(*state_access_t::wait_io(top), ERROR_WAKEUP);     /* now queued */
-
-    /* the caller of a called root resumes once the chain is gone, in the target's turn */
-    bool woken = top->get_state() == STATE_READY;
-    if (state_t *caller = root->caller_state) {
-        if (!pool->replace_ready(top, caller))
-            pool->push_ready(caller);
+    error_e on_exit(state_t *s) {
+        if (s != root) {
+            top = s->caller_state;
+            return ERROR_OK;
+        }
+        root = top = nullptr;
+        if (!turn)
+            return ERROR_OK;
+        if (!s->self.done() && s->err != ERROR_YIELDED) {
+            /* neither returned nor yielded: another killer ended it while we drove it, its caller
+            is theirs to queue */
+            s->pool->get_internal()->remove_ready(turn);
+            turn = nullptr;
+            return ERROR_OK;
+        }
+        /* ended by itself while driven: its caller gets its result in its turn, not from here */
+        completed = true;
+        queue_caller(s);
+        return ERROR_SUSPENDED;
     }
-    else
-        pool->remove_ready(top);
 
-    if (woken && wait_had_effect(top))
-        return drive();
-    end_chain();
-    return ERROR_OK;
-}
+    error_e on_wait(state_t *) {
+        return dying ? ERROR_SUSPENDED : ERROR_OK;
+    }
 
-/* its wait already did its work: the code after it runs, here, up to the next wait */
-inline error_e killer_state_t::drive() {
-    dying = driving = true;
-    top->self.resume();
-    driving = false;
-    if (!top)
-        return ERROR_FINISHED;  /* its root ended: it completed in time */
-    end_chain();
-    return ERROR_OK;
-}
+    error_e on_parked(state_t *s) {
+        /* a lazy death reached its next wait: it ends here (a drive ends it after its resume) */
+        if (!dying || top != s || killing || turn)
+            return ERROR_OK;
+        end_chain();
+        return ERROR_FINISHED;
+    }
 
-inline void killer_state_t::end_chain() {
-    killing = true;
-    close_wait(top);
-    if (root->caller_state)
-        root->err = e;
-    destroy_state(top, root);
-    killing = false;
-}
+    /* ---------------------------------------------------------------- the kill function */
+
+    error_e kill() {
+        if (killing)
+            return ERROR_GENERIC;   /* called from our own destruction of the chain */
+        if (!top)
+            return ERROR_FINISHED;
+
+        switch (top->get_state()) {
+            case STATE_RUNNING:
+                dying = true;       /* executing: it dies at its next wait */
+                throw kill_deferred_t(top);
+
+            case STATE_WAITING_IO: {
+                /* an io that completed before the stop is delivered: it did its work */
+                error_e ret = top->pool->get_internal()->stop_io(*state_access_t::wait_io(top),
+                        ERROR_WAKEUP);
+                if (ret == ERROR_FINISHED)
+                    return drive();
+                turn = top;
+                break;
+            }
+
+            case STATE_READY:
+                if (wait_had_effect(top))
+                    return drive();
+                turn = top;
+                break;
+
+            default:
+                break;
+        }
+        end_chain();
+        return ERROR_OK;
+    }
+
+    /* its wait already did its work: the code after it runs, here, up to its next wait */
+    error_e drive() {
+        state_t placeholder;        /* keeps its place in the ready queue while it runs */
+        top->pool->get_internal()->replace_ready(top, &placeholder);
+        turn = &placeholder;
+        dying = true;
+        top->self.resume();
+        if (top)
+            end_chain();            /* parked at its next wait */
+        return completed ? ERROR_FINISHED : ERROR_OK;
+    }
+
+    void end_chain() {
+        killing = true;
+        if (root->caller_state)
+            root->err = e;
+        queue_caller(root);
+        close_wait(top);
+        destroy_state(top, root);
+        killing = false;
+    }
+
+    /* the root's caller resumes in the target's turn, or at the end of the ready queue */
+    void queue_caller(state_t *root_state) {
+        pool_internal_t *pool = root_state->pool->get_internal();
+        state_t *caller = root_state->caller_state;
+        if (caller && !(turn && pool->replace_ready(turn, caller)))
+            pool->push_ready(caller);
+        if (turn)
+            pool->remove_ready(turn);
+        turn = nullptr;
+    }
+
+    error_e e;                  /* the err of a killed root that was called */
+    state_t *root = nullptr;    /* the frame the killer was attached to */
+    state_t *top = nullptr;     /* the innermost frame of the chain, its callers lead to root */
+    state_t *turn = nullptr;    /* the target's place in the ready queue, for the root's caller */
+    bool dying = false;         /* its next wait parks */
+    bool killing = false;       /* kill() is destroying the chain */
+    bool completed = false;     /* its root ended by itself while driven */
+};
 
 /* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
 /* this is inherited by-call and it must kill all coros in the call path and also stop all waiters
@@ -6235,59 +6269,28 @@ inline std::pair<modif_pack_t, std::func
     and the shared_ptr's own count - into a pool that no longer exists. Same reasoning as modif_t's
     own allocation (see 018-010); 018-014 is this one's regression test. 2026-09-23 05:06 */
     (void)pool;
-    auto kstate = std::make_shared<killer_state_t>(e);
+    auto k = std::make_shared<killer_state_t>(e);
 
-    COLIB_DEBUG_TRACE("created killer: %p", kstate.get());
-
-    auto dying_wait = [kstate](state_t *) -> error_e {
-        return kstate->dying ? ERROR_SUSPENDED : ERROR_OK;
-    };
+    COLIB_DEBUG_TRACE("created killer: %p", k.get());
 
     modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
     modif_pack_t pack;
     pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
-        [kstate](state_t *s) -> error_e {
-            if (!kstate->root)
-                kstate->root = s;
-            kstate->top = s;
-            return ERROR_OK;
-        }
-    ));
+            [k](state_t *s) { return k->on_call(s); }));
     pack.push_back(create_modif<CO_MODIF_SCHED_CBK>(flags,
-        [kstate](state_t *s) -> error_e {
-            kstate->root = kstate->top = s;
-            return ERROR_OK;
-        }
-    ));
+            [k](state_t *s) { return k->on_sched(s); }));
     pack.push_back(create_modif<CO_MODIF_EXIT_CBK>(flags,
-        [kstate](state_t *s) -> error_e {
-            if (s != kstate->root) {
-                kstate->top = s->caller_state;
-                return ERROR_OK;
-            }
-            kstate->root = kstate->top = nullptr;
-            /* ended while driven: its caller is already queued, don't jump into it */
-            return kstate->driving ? ERROR_SUSPENDED : ERROR_OK;
-        }
-    ));
+            [k](state_t *s) { return k->on_exit(s); }));
     pack.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(flags,
-        [dying_wait](state_t *s, io_desc_t &) -> error_e { return dying_wait(s); }));
+            [k](state_t *s, io_desc_t &) { return k->on_wait(s); }));
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
-        [dying_wait](state_t *s, sem_t *) -> error_e { return dying_wait(s); }));
-    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags, dying_wait));
+            [k](state_t *s, sem_t *) { return k->on_wait(s); }));
+    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags,
+            [k](state_t *s) { return k->on_wait(s); }));
     pack.push_back(create_modif<CO_MODIF_PARKED_CBK>(flags,
-        [kstate](state_t *s) -> error_e {
-            /* a lazy death parked: nobody resumes it for us */
-            if (kstate->dying && kstate->top == s && !kstate->killing) {
-                if (state_t *caller = kstate->root->caller_state)
-                    s->pool->get_internal()->push_ready(caller);
-                kstate->end_chain();
-            }
-            return ERROR_OK;
-        }
-    ));
+            [k](state_t *s) { return k->on_parked(s); }));
 
-    return {pack, [kstate]() -> error_e { return kstate->kill(); }};
+    return {pack, [k]() -> error_e { return k->kill(); }};
 }
 
 /* Debug stuff
```
