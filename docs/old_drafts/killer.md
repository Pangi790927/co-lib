# The killer, back in shape

A diff on top of today's `colib.h` (the combined diff and `sem_list.md` already applied). Nothing is
applied yet. It was made on a scratch copy and tested there.

## What was wrong

The killer between `struct killer_state_t {` and `/* Debug stuff` kept its own copy of what the
engine already knows, and did by hand what the engine already does:

1. **Its own walker.** `call_stack` was a `std::stack` rebuilt from CALL/SCHED/EXIT. It copied the
   chain the frames already form through `caller_state`.
2. **`unwind()` was `destroy_state()`**, the same walk with a stopping point.
3. **The WAIT/UNWAIT tracking (`io_desc`, `sem`) and `drop()`** existed only to learn what the top
   was waiting on, and then to replay the same "close the wait, destroy" that
   `sem_internal_t::clear()` and every `io_pool_t::clear()` already did, each in its own copy.
4. **`take_turn`, `queue_caller` and a placeholder in the ready queue** were a second scheduling
   path.
5. **Five flags** mirrored states the state machine already records.

## What changes

**The engine keeps the facts, once, for everyone:**
- **A state remembers the wait it's in**, from its WAIT to its UNWAIT: the kind (io or semaphore)
  and the `io_desc_t *` or `sem_t *`. `do_wait_*_modifs` records it and `do_unwait_*_modifs` clears
  it, so every path gets it for free.
- **`close_wait(state_t *)`** ends that wait without resuming the coroutine: it takes it out of its
  queue and runs its UNWAIT. It's used by `sem_internal_t::clear()`, the three `io_pool_t::clear()`
  (which no longer rebuild a temporary `io_desc_t` just to pass to UNWAIT), the pool's `clear()`, and
  the killer.
- **The pool's `clear()` now closes the wait of a coroutine that was woken but never resumed** before
  destroying it. Before, that coroutine left without its UNWAIT.
- **`destroy_state(from, root)`** is the existing walk with an optional stopping point. A root that
  was called is only exited, and left to its caller.
- **`wait_had_effect(state_t *)`**: the wait a coroutine was woken from already did its work (a
  semaphore token taken, or an io that moved its data, asked from the backend).

**The killer only decides.**
- **It holds `e`, `root`, `top` and three flags** (`dying`, `driving`, `killing`). `top` is one
  pointer: CALL moves it to the callee, EXIT back to `caller_state`.
- **`kill()` reads the target's state:**

  | The target is | The kill |
  |---|---|
  | running | defers, and throws `kill_deferred_t` |
  | waiting on an io | stops the io, which queues it, then treats it as woken |
  | woken, and its wait had an effect | resumes it up to its next wait (the drive) |
  | anything else | `close_wait()` and `destroy_state(top, root)` |

  A called root's caller takes the target's turn in the ready queue with one `replace_ready()`, or
  is queued when the target had no turn.
- **Callbacks left:** CALL/SCHED/EXIT for `root` and `top`; one lambda for all three WAITs (park
  while dying); PARKED, which ends a lazily killed chain.
- **Gone:** the `std::stack`, `io_desc`/`sem`, the UNWAIT callbacks, `take_turn()`,
  `queue_caller()`, the placeholder, `drop()`, `unwind()` and `park()`.

**Behaviour is unchanged.** Every test passes as it is; none needed editing.

## Size

| | Before | After |
|---|---|---|
| The killer block (`struct killer_state_t` to `/* Debug stuff`) | 258 lines | 129 lines |
| `colib.h` | 7322 lines | 7233 lines |
| `sizeof(state_t)` (MSVC x64) | 104 | **112** |

The 8 bytes are the pointer to the wait (`wait_on`); its kind fits in the padding after `err`. It's
the price of the engine knowing, instead of the killer spying on WAIT/UNWAIT.

## Verification

Built through the scratch `tests/` makefile on Windows (MSVC 19.43):

| | Result |
|---|---|
| Whole suite, plus 16 killer/wait/clear tests rebuilt with `COLIB_ENABLE_DEBUG_CHECKS` | **all pass except `018-011` and `018-017`** (`BUGS.md` #5, #6, as before) |
| Hidden `ASSERT_COFN` failures in the logs | none |
| `018-015`, `002-005`, `002-007`, `003-003`, 5 reruns | pass every time |

`BUGS.md` #6 (a vetoed call leaves a freed frame for the killer) is not fixed by this: `top` can still
point at a callee destroyed without its EXIT. It's the same bug in a smaller shape.

---

## The diff

The hunks are in file order, so together they make the whole patch (14 hunks).

### 1. `state_t` records its wait

The kind sits in the padding after `err`; the pointer is new.

```diff
--- a/colib.h
+++ b/colib.h
@@ -1116,8 +1116,11 @@ private:
     friend struct state_list_t;
     friend struct state_access_t;
 
+    enum wait_e : uint8_t { WAIT_NONE, WAIT_IO, WAIT_SEM };
+
     state_e state = STATE_LEFT;
     bool enter_on_start = false;            /* scheduled: ENTER when the pool first runs it */
+    wait_e wait = WAIT_NONE;                /* the wait it is in, from WAIT to UNWAIT */
 
 public:
     pool_t *pool = nullptr;                 /*!< the pool of this coro */
@@ -1141,6 +1144,7 @@ private:
     state_t *prev = nullptr;
     state_t *next = nullptr;
     state_list_t *list = nullptr;           /* the queue it is linked in, if any */
+    void *wait_on = nullptr;                /* the io_desc_t or the sem_t of that wait */
 };
 
             
```

### 2. `state_access_t`

Recording, clearing and reading the wait.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2387,6 +2391,15 @@ struct state_access_t {
         if (s->list)
             s->list->unlink(s);
     }
+    static void set_wait(state_t *s, io_desc_t *io) { s->wait = state_t::WAIT_IO; s->wait_on = io; }
+    static void set_wait(state_t *s, sem_t *sem) { s->wait = state_t::WAIT_SEM; s->wait_on = sem; }
+    static void clear_wait(state_t *s) { s->wait = state_t::WAIT_NONE; s->wait_on = nullptr; }
+    static io_desc_t *wait_io(state_t *s) {
+        return s->wait == state_t::WAIT_IO ? (io_desc_t *)s->wait_on : nullptr;
+    }
+    static sem_t *wait_sem(state_t *s) {
+        return s->wait == state_t::WAIT_SEM ? (sem_t *)s->wait_on : nullptr;
+    }
 };
 
 /* Those are needed for destroy_state, internally and to call it */
```

### 3. `close_wait()` and the bounded `destroy_state()`

```diff
--- a/colib.h
+++ b/colib.h
@@ -2396,13 +2409,28 @@ inline error_e do_exit_modifs(state_t *s
 inline error_e do_unwait_io_modifs(state_t *state, io_desc_t &io_desc);
 inline error_e do_unwait_sem_modifs(state_t *state, sem_t *sem);
 
-inline void destroy_state(state_t *curr) {
+/* ends the wait `s` is in without resuming it: out of its queue, and its UNWAIT callbacks */
+inline void close_wait(state_t *s) {
+    state_access_t::unlink(s);
+    if (io_desc_t *io = state_access_t::wait_io(s))
+        do_unwait_io_modifs(s, *io);
+    else if (sem_t *sem = state_access_t::wait_sem(s))
+        do_unwait_sem_modifs(s, sem);
+}
+
+/* destroys `curr` and its callers; with a `root` it stops there, and a root that was called is
+only exited: its caller destroys it */
+inline void destroy_state(state_t *curr, state_t *root = nullptr) {
     COLIB_DEBUG_TRACE_SCOPE("to destroy: %p", curr);
     while (curr) {
         COLIB_DEBUG_TRACE("curr: %p", curr);
         state_t *next = curr->caller_state;
         do_exit_modifs(curr);
+        if (curr == root && next)
+            return;
         curr->self.destroy();
+        if (curr == root)
+            return;
         curr = next;
     }
 }
```

### 4. WAIT and UNWAIT record the wait

```diff
--- a/colib.h
+++ b/colib.h
@@ -2653,6 +2681,7 @@ inline error_e do_wait_io_modifs(state_t
     COLIB_DEBUG_TRACE("    WAIT: %s state: %p io:[%s]", dbg_name(state->self).c_str(), state,
             dbg_to_str(io_desc).c_str());
     COLIB_DEBUG_CHECK_WAIT_IO(state, io_desc);
+    state_access_t::set_wait(state, &io_desc);
     return do_generic_modifs<CO_MODIF_WAIT_IO_CBK>(state, io_desc);
 }
 
@@ -2660,18 +2689,21 @@ inline error_e do_unwait_io_modifs(state
     COLIB_DEBUG_TRACE("UNWAIT: %s state: %p io:[%s]", dbg_name(state->self).c_str(), state,
             dbg_to_str(io_desc).c_str());
     COLIB_DEBUG_CHECK_UNWAIT_IO(state, io_desc);
+    state_access_t::clear_wait(state);
     return do_generic_modifs<CO_MODIF_UNWAIT_IO_CBK>(state, io_desc);
 }
 
 inline error_e do_wait_sem_modifs(state_t *state, sem_t *sem) {
     COLIB_DEBUG_TRACE("    SEM: %s state: %p sem:%p", dbg_name(state->self).c_str(), state, sem);
     COLIB_DEBUG_CHECK_WAIT_SEM(state, sem);
+    state_access_t::set_wait(state, sem);
     return do_generic_modifs<CO_MODIF_WAIT_SEM_CBK>(state, sem);
 }
 
 inline error_e do_unwait_sem_modifs(state_t *state, sem_t *sem) {
     COLIB_DEBUG_TRACE("UNSEM: %s state: %p %p", dbg_name(state->self).c_str(), state, sem);
     COLIB_DEBUG_CHECK_UNWAIT_SEM(state, sem);
+    state_access_t::clear_wait(state);
     return do_generic_modifs<CO_MODIF_UNWAIT_SEM_CBK>(state, sem);
 }
 
```

### 5. The io pools' `clear()`

No temporary `io_desc_t` for UNWAIT: `close_wait()` uses the real one.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3160,8 +3192,7 @@ struct io_pool_t {
         for (int i = 0; i < MAX_FAST_FD_CACHE; i++) {
             if (auto *data = fd_data_fast[i]) {
                 for (auto &w : data->waiters) {
-                    io_desc_t desc{ .fd = i, .events = w.mask };
-                    do_unwait_io_modifs(w.state, desc);
+                    close_wait(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = i, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3175,8 +3206,7 @@ struct io_pool_t {
         for (auto &[fd, data] : fd_data_slow_copy) {
             if (data) {
                 for (auto &w : data->waiters) {
-                    io_desc_t desc{ .fd = fd, .events = w.mask };
-                    do_unwait_io_modifs(w.state, desc);
+                    close_wait(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = fd, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3664,8 +3694,7 @@ struct io_pool_t {
                         GetOverlappedResult(data->h, &data->overlapped, &aux_bytes, TRUE);
                     }
                 }
-                io_desc_t desc{ .data = data, .h = data->h };
-                do_unwait_io_modifs(data->state, desc);
+                close_wait(data->state);
                 destroy_state(data->state);
             }
         }
```

### 6. `wait_had_effect()`

```diff
--- a/colib.h
+++ b/colib.h
@@ -4284,6 +4313,15 @@ inline void push_parked(state_t *state)
     state->pool->get_internal()->push_parked(state);
 }
 
+/* the wait `s` was woken from already did its work before it resumed: a semaphore token taken, or
+an io that moved its data */
+inline bool wait_had_effect(state_t *s) {
+    if (state_access_t::wait_sem(s))
+        return true;
+    io_desc_t *io = state_access_t::wait_io(s);
+    return io && s->pool->get_internal()->io_completed(*io);
+}
+
 inline error_e pool_t::stop_io(const io_desc_t& io_desc) {
     return get_internal()->stop_io(io_desc, ERROR_WAKEUP);
 }
```

### 7. `sem_internal_t::clear()` and the pool's `clear()`

The pool's `clear()` now ends a woken waiter's wait too.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4572,8 +4610,8 @@ struct sem_internal_t : public sem_t {
         /* a waiter may hold the last sem_p: once one is destroyed `this` may be gone, so the waiters
         are taken out and closed first, and destroyed last */
         state_list_t closed{STATE_WAITING_SEM};
-        while (state_t *s = waiting_on_sem.pop_front()) {
-            do_unwait_sem_modifs(s, this);
+        while (state_t *s = waiting_on_sem.front()) {
+            close_wait(s);
             closed.push_back(s);
         }
         this->val = val;
@@ -4642,8 +4680,10 @@ inline error_e pool_internal_t::clear()
         s->clear(0);        /* may free s itself (a waiter held the last sem_p) */
     }
     COLIB_DEBUG_TRACE("Cleaning wait queue");
-    while (state_t *state = ready_tasks.pop_front())
+    while (state_t *state = ready_tasks.front()) {
+        close_wait(state);      /* a woken waiter that never resumed still ends its wait */
         destroy_state(state);
+    }
     return ERROR_OK;
 }
 
```

### 8. The killer

The whole block, from `struct killer_state_t` to `create_killer`'s end.

```diff
--- a/colib.h
+++ b/colib.h
@@ -6124,154 +6164,65 @@ inline task<std::pair<T, error_e>> creat
 struct killer_state_t {
     killer_state_t(error_e e) : e(e) {}
 
-    error_e e;                          /* the err of a killed root that was called */
-
-    std::stack<state_t *> call_stack;   /* top() is the innermost frame */
-    io_desc_t *io_desc = nullptr;       /* the io the top waits on */
-    sem_t *sem = nullptr;               /* the semaphore the top waits on */
-
-    bool dying = false;     /* the next wait parks */
-    bool parked = false;    /* the top parked because of `dying` */
-    bool driving = false;   /* kill() is resuming the chain */
-    bool unwinding = false; /* kill() is destroying the chain */
-    bool finished = false;  /* the root exited while driven */
-    state_t *turn = nullptr;            /* the target's place in the ready queue, for its caller */
+    error_e e;                  /* the err of a killed root that was called */
+    state_t *root = nullptr;    /* the frame the killer was attached to */
+    state_t *top = nullptr;     /* the innermost frame of the chain, its callers lead to root */
+    bool dying = false;         /* its next wait parks */
+    bool driving = false;       /* kill() is resuming the chain */
+    bool killing = false;       /* kill() is destroying the chain */
 
     error_e kill();
-    error_e take_turn(state_t *top, bool effect);
-    void queue_caller(state_t *caller);
     error_e drive();
-    void drop(state_t *top);
-    void unwind();
-    error_e park(state_t *s);
+    void end_chain();
 };
 
 inline error_e killer_state_t::kill() {
-    /* called from our own unwind */
-    if (unwinding)
-        return ERROR_GENERIC;
-    if (call_stack.empty())
+    if (killing)
+        return ERROR_GENERIC;   /* called from our own destruction of the chain */
+    if (!top)
         return ERROR_FINISHED;
 
-    state_t *top = call_stack.top();
-    switch (top->get_state()) {
-        case STATE_RUNNING:
-            /* executing, it dies at its next wait */
-            dying = true;
-            throw kill_deferred_t(top);
-
-        case STATE_READY:
-            /* woken, or never started: with an effect it runs up to its next wait */
-            return take_turn(top,
-                    (io_desc && top->pool->get_internal()->io_completed(*io_desc)) || sem);
-
-        case STATE_WAITING_IO: {
-            /* an io that completed meanwhile is delivered */
-            error_e ret = top->pool->get_internal()->stop_io(*io_desc, ERROR_WAKEUP);
-            if (ret != ERROR_OK && ret != ERROR_FINISHED) {
-                COLIB_DEBUG("WARNING: failed to stop the io of a killed coroutine: %s",
-                        dbg_enum(ret).c_str());
-            }
-            return take_turn(top, ret == ERROR_FINISHED);   /* stop_io queued it */
-        }
-
-        case STATE_WAITING_SEM:
-            /* no token was taken */
-            sem->get_internal()->erase_waiter(top);
-            drop(top);
-            return ERROR_OK;
-
-        case STATE_PARKED:
-            /* parked, the pool didn't call us back yet */
-        case STATE_LEFT:
-            /* on an external awaitable */
-            unwind();
-            return ERROR_OK;
-    }
-    return ERROR_GENERIC;
-}
-
-/* The target is in the ready queue: its place there is kept (a placeholder) for its caller */
-inline error_e killer_state_t::take_turn(state_t *top, bool effect) {
     pool_internal_t *pool = top->pool->get_internal();
-    state_t placeholder;
-    pool->replace_ready(top, &placeholder);
-    turn = &placeholder;
-
-    error_e ret = ERROR_OK;
-    if (effect)
-        ret = drive();
+    if (top->get_state() == STATE_RUNNING) {
+        dying = true;           /* executing: it dies at its next wait */
+        throw kill_deferred_t(top);
+    }
+    if (top->get_state() == STATE_WAITING_IO)
+        pool->stop_io(*state_access_t::wait_io(top), ERROR_WAKEUP);     /* now queued */
+
+    /* the caller of a called root resumes once the chain is gone, in the target's turn */
+    bool woken = top->get_state() == STATE_READY;
+    if (state_t *caller = root->caller_state) {
+        if (!pool->replace_ready(top, caller))
+            pool->push_ready(caller);
+    }
     else
-        drop(top);
+        pool->remove_ready(top);
 
-    if (turn)
-        pool->remove_ready(turn);   /* no caller took it */
-    turn = nullptr;
-    return ret;
-}
-
-/* the caller resumes in the target's place in the ready queue, if it had one */
-inline void killer_state_t::queue_caller(state_t *caller) {
-    pool_internal_t *pool = caller->pool->get_internal();
-    if (turn && pool->replace_ready(turn, caller)) {
-        turn = nullptr;
-        return;
-    }
-    pool->push_ready(caller);
+    if (woken && wait_had_effect(top))
+        return drive();
+    end_chain();
+    return ERROR_OK;
 }
 
+/* its wait already did its work: the code after it runs, here, up to the next wait */
 inline error_e killer_state_t::drive() {
-    state_t *top = call_stack.top();
-
-    /* runs until it parks at its next wait or its root exits */
-    dying = true;
-    driving = true;
+    dying = driving = true;
     top->self.resume();
     driving = false;
-
-    if (finished)
-        return ERROR_FINISHED;
-    unwind();
+    if (!top)
+        return ERROR_FINISHED;  /* its root ended: it completed in time */
+    end_chain();
     return ERROR_OK;
 }
 
-inline void killer_state_t::drop(state_t *top) {
-    /* replay the wake-up, so the modifs see the wait end */
-    if (io_desc)
-        do_unwait_io_modifs(top, *io_desc);
-    else if (sem)
-        do_unwait_sem_modifs(top, sem);
-    unwind();
-}
-
-inline void killer_state_t::unwind() {
-    /* innermost first, our EXIT cbk pops call_stack */
-    unwinding = true;
-    while (call_stack.size() > 1) {
-        state_t *s = call_stack.top();
-        do_exit_modifs(s);
-        s->self.destroy();
-    }
-    state_t *root = call_stack.top();
-    do_exit_modifs(root);
-    if (!root->caller_state) {
-        root->self.destroy();       /* scheduled, nobody waits for it */
-    }
-    else {
-        /* called, its caller destroys it */
+inline void killer_state_t::end_chain() {
+    killing = true;
+    close_wait(top);
+    if (root->caller_state)
         root->err = e;
-        queue_caller(root->caller_state);
-    }
-    parked = false;
-    unwinding = false;
-}
-
-/* the answer of the wait cbks once the chain is dying: the pool calls our PARKED cbk after the
-resume, unless a drive (or another killer) destroys the chain first */
-inline error_e killer_state_t::park(state_t *s) {
-    (void)s;
-    parked = true;
-    return ERROR_SUSPENDED;
+    destroy_state(top, root);
+    killing = false;
 }
 
 /* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
@@ -6288,90 +6239,50 @@ inline std::pair<modif_pack_t, std::func
 
     COLIB_DEBUG_TRACE("created killer: %p", kstate.get());
 
+    auto dying_wait = [kstate](state_t *) -> error_e {
+        return kstate->dying ? ERROR_SUSPENDED : ERROR_OK;
+    };
+
     modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
     modif_pack_t pack;
     pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
         [kstate](state_t *s) -> error_e {
-            COLIB_DEBUG_TRACE("CALL[%p]: tracking killer: %p", kstate.get(), s);
-            kstate->call_stack.push(s);
+            if (!kstate->root)
+                kstate->root = s;
+            kstate->top = s;
             return ERROR_OK;
         }
     ));
     pack.push_back(create_modif<CO_MODIF_SCHED_CBK>(flags,
         [kstate](state_t *s) -> error_e {
-            /* The first schedule must init the call stack */
-            COLIB_DEBUG_TRACE("SCHED[%p]: tracking killer: %p", kstate.get(), s);
-            kstate->call_stack.push(s);
+            kstate->root = kstate->top = s;
             return ERROR_OK;
         }
     ));
     pack.push_back(create_modif<CO_MODIF_EXIT_CBK>(flags,
         [kstate](state_t *s) -> error_e {
-            COLIB_DEBUG_TRACE("EXIT[%p]: tracking killer: %p", kstate.get(), s);
-            COLIB_ENABLE_DEBUG_CHECK_ASSERT(kstate->call_stack.size(), "bad-pop");
-            kstate->call_stack.pop();
-            if (kstate->driving && kstate->call_stack.empty()) {
-                /* the root exited while driven, its caller is queued, not resumed */
-                kstate->finished = true;
-                if (s->caller_state)
-                    kstate->queue_caller(s->caller_state);
-                return ERROR_SUSPENDED;
+            if (s != kstate->root) {
+                kstate->top = s->caller_state;
+                return ERROR_OK;
             }
-            return ERROR_OK;
+            kstate->root = kstate->top = nullptr;
+            /* ended while driven: its caller is already queued, don't jump into it */
+            return kstate->driving ? ERROR_SUSPENDED : ERROR_OK;
         }
     ));
     pack.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(flags,
-        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
-            COLIB_DEBUG_TRACE("WAIT_IO[%p]: tracking killer: %p io-ptr: %p",
-                    kstate.get(), s, &io_desc);
-            if (kstate->dying)
-                return kstate->park(s);
-            kstate->io_desc = &io_desc;
-            return ERROR_OK;
-        }
-    ));
-    pack.push_back(create_modif<CO_MODIF_UNWAIT_IO_CBK>(flags,
-        [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
-            (void)s;
-            (void)io_desc;
-            COLIB_DEBUG_TRACE("UNWAIT_IO[%p]: tracking killer: %p io-ptr: %p",
-                    kstate.get(), s, &io_desc);
-            kstate->io_desc = nullptr;
-            return ERROR_OK;
-        }
-    ));
+        [dying_wait](state_t *s, io_desc_t &) -> error_e { return dying_wait(s); }));
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
-        [kstate](state_t *s, sem_t *sem) -> error_e {
-            COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p",
-                    kstate.get(), s, sem);
-            if (kstate->dying)
-                return kstate->park(s);
-            kstate->sem = sem;
-            return ERROR_OK;
-        }
-    ));
-    pack.push_back(create_modif<CO_MODIF_UNWAIT_SEM_CBK>(flags,
-        [kstate](state_t *s, sem_t *sem) -> error_e {
-            (void)s;
-            (void)sem;
-            COLIB_DEBUG_TRACE("UNWAIT_SEM[%p]: tracking killer: %p sem: %p",
-                    kstate.get(), s, sem);
-            kstate->sem = nullptr;
-            return ERROR_OK;
-        }
-    ));
-    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags,
-        [kstate](state_t *s) -> error_e {
-            if (kstate->dying)
-                return kstate->park(s);
-            return ERROR_OK;
-        }
-    ));
+        [dying_wait](state_t *s, sem_t *) -> error_e { return dying_wait(s); }));
+    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags, dying_wait));
     pack.push_back(create_modif<CO_MODIF_PARKED_CBK>(flags,
         [kstate](state_t *s) -> error_e {
-            (void)s;    /* may be destroyed by an earlier PARKED cbk, only our state is read */
-            if (kstate->parked && !kstate->unwinding && !kstate->call_stack.empty())
-                kstate->unwind();
+            /* a lazy death parked: nobody resumes it for us */
+            if (kstate->dying && kstate->top == s && !kstate->killing) {
+                if (state_t *caller = kstate->root->caller_state)
+                    s->pool->get_internal()->push_ready(caller);
+                kstate->end_chain();
+            }
             return ERROR_OK;
         }
     ));
```
