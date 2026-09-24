# The `colib.h` change, in one diff

This is one diff, from today's `colib.h` (commit `45d4140`) to the end state of the whole review,
with explanations around each touched part. The code in it is the same code that was reviewed
step by step in:
- `killer_diff.md` (sections 3 and 4);
- `reversed_modifs.md` (both parts);
- `state_queues.md`.

On top of those, one more simplification, made possible by the last two (section 2 below). This
file replaces the four as the thing to apply; they stay as the review history. Nothing is applied
to the repo.

It's two patches: `colib.h` (section 5), and the tests (section 6).

---

## 1. What the change does

**The killer: `create_killer`, `create_timeo`.** A kill keeps two promises:
- when it returns, the target is dead;
- a wait that already completed is never thrown away.

A target whose wait completed (a Windows read that already moved its bytes, or a semaphore token
already given) is resumed inside the kill, up to its next wait, and dies there, before that wait
does anything. If its root finishes instead, the kill returns `ERROR_FINISHED`. When a kill is
called from inside its own target, it's deferred to the target's next wait and throws
`kill_deferred_t`. A killed child's caller resumes in the child's place in the ready queue, with
the child's result or error, and never from inside the kill. In `create_timeo`, `exec_coro` and
`timer_coro` no longer kill each other: there's one decision point.

**The modif dispatch.**
- The priority is: an error, then `ERROR_SUSPENDED`, then `ERROR_OK`. An error means the wait doesn't
  happen, so there's nothing to suspend. A negative return from a WAIT callback is returned by the
  wait at once, and code that retries it without handling the error spins; the docs say so.
- `ERROR_SUSPENDED` means a modif suspends the coroutine and owns it from then on. Once the
  resume that suspended it has returned, the pool calls the owner back with the new
  `CO_MODIF_PARKED_CBK`; the owner then resumes or destroys the coroutine.
- Callbacks whose return value is ignored (EXIT, LEAVE, ENTER, the UNWAITs) always all run, so one
  can't keep the others from running.
- Every WAIT gets its UNWAIT, including an aborted one.
- The closing callbacks (EXIT, LEAVE, the UNWAITs) run in the reverse order of the opening ones.
- The kinds nest: a coroutine is running (ENTER..LEAVE) or waiting (WAIT..UNWAIT), never both. That
  fixes `BUGS.md` #1.
- `CO_MODIF_WAIT_YIELD_CBK` is new, for `yield()` and `force_stop()`.

**Coroutine state and queues.**
- `state_t` knows where it is: `get_state()`, one of `STATE_RUNNING`, `READY`, `WAITING_SEM`,
  `WAITING_IO`, `PARKED` (suspended by a modif, its owner not called back yet), `DONE` (a finished
  scheduled root, destroyed after the resume) or `LEFT`.
- The ready queue, the semaphore wait lists, and the pool's parked and done lists are linked through
  `state_t` itself: O(1) operations, no allocation, at most one queue per coroutine.
- A destroyed coroutine leaves its queue by itself.
- The links are private to colib.

**`pool_t` and `sem_t` without the pimpl pointer.** `pool_internal_t` derives from `pool_t`, and
`sem_internal_t` from `sem_t`. No definition moved, and `get_internal()` is a `static_cast`.

**Windows `force_awake`.** It always waits for the cancel to settle. An io that completed first is
delivered, and `ERROR_FINISHED` is returned, for the killer and for `co::stop_io` alike.

---

## 2. New in this stitch: the killer reads the state

With the state from `state_queues.md`, the killer no longer infers what its target is doing:

- **`kill()` switches on `top->get_state()`:**

  | State | What the kill does |
  |---|---|
  | `RUNNING` | the target is executing: the kill is deferred, and throws |
  | `READY` | resumed up to its next wait if its wait had an effect, dropped otherwise |
  | `WAITING_IO` | the io is stopped, and an io that completed meanwhile is delivered |
  | `WAITING_SEM` | the target leaves the wait list, then is dropped |
  | `PARKED` | parked, and the pool hasn't called back yet: unwound |
  | `LEFT` | on an external awaitable: unwound |

  Gone: `entered`, the `is_ready()` search, the semaphore waiter handle `it`, the killer's ENTER
  and LEAVE callbacks, and `pool_internal_t::is_ready()`, which had no other user.
- **To make that possible, `sched()` no longer ENTERs a coroutine at schedule time.** The ENTER now
  happens when the pool first runs it (`next_task_state()`). That was the one place where ENTER
  didn't mean "starts running", and it was the only reason the killer tracked ENTER/LEAVE: a
  scheduled, never-started coroutine is now just `READY`, and it has no ENTER to balance with a
  LEAVE. **This is a behaviour change:** a scheduled coroutine's ENTER callbacks now run when it
  starts, not when it's scheduled. The ordering tests (`011-003`, `011-006`, `011-007`) pass
  unchanged, since ENTER is still the first thing each coroutine sees.
- **The `co_yield` probe from `killer_diff.md` R1 is now a test:** `002-006`.
- **The lazy drop is a node, not a lambda.** `park()` used to post a `std::function` that captured
  a `shared_ptr` to the killer. Now:
  - the awaiter's park path links the parked state into the pool's `parked` list (`STATE_PARKED`),
    for any modif that answered `ERROR_SUSPENDED`;
  - after the resume, `run()` pops each one and calls `CO_MODIF_PARKED_CBK` on it;
  - the killer's PARKED callback unwinds.

  If something destroys the chain first (a drive, or another killer), `~state_t()` unlinks it and
  nobody is called back; `002-007` covers that. The finished-root destroys (`posted_to_destroy`)
  became the same kind of list, `STATE_DONE`. Gone: `post_after_resume`, its `std::function`
  vector, the killer's `enable_shared_from_this`, and the `driving` special case in `park()`.
- **No backend detail outside the backends.** The killer's `io_had_effect()` read IOCP internals
  (`io_data_t` flags) behind `#if COLIB_OS_WINDOWS`. Now each `io_pool_t` answers
  `completed(io_desc)` itself: did the io already do its work before its coroutine resumed? epoll
  and kqueue say no (readiness only), and IOCP says yes for a request that moved its data. The
  killer only asks.

---

## 3. Behaviour and API changes, all in one place

- **Error codes:**
  - `ERROR_FINISHED` (3) and `ERROR_SUSPENDED` (2) are new. Both are positive, so `< 0` checks
    read them as success.
  - A kill of an already-finished target returns `ERROR_FINISHED` instead of `ERROR_GENERIC`.
  - A kill can throw `kill_deferred_t`, which derives from `std::exception`.
- **Two exceptions in one resume terminate.** Only possible with a drive: a scheduled root resumed
  inside a kill throws out of its root, and then the killer's own scheduled root throws before any
  wait. `run()` can hand out one exception per call. Dropping the second (the old "the first one
  wins" slot) would lose it silently, and it can't be given to a later `run()` that didn't fail.
  A probe shows the difference: before this change, `run()` threw only the first exception and the
  second was gone; now the process terminates.
- **`co::stop_io`** can return `ERROR_FINISHED` and deliver the data of an io that had already
  completed.
- **Modif callbacks:**
  - WAIT/UNWAIT callbacks see LEAVE before WAIT, and UNWAIT before ENTER.
  - A scheduled coroutine's ENTER comes at its first run.
  - Closing callbacks run in reverse order.
  - An aborted wait runs its UNWAIT.
- **`sem_waiter_handle_p` is now `state_t *`.** Callbacks that only take the parameter still
  compile; code that calls `.get()` on it doesn't.
- **A `COLIB_OS_UNKNOWN` backend's `io_pool_t`** receives the ready queue as `state_list_t &`,
  and must implement `bool completed(const io_desc_t &)`.
- **New:** `state_e` (with `STATE_PARKED` and `STATE_DONE`), `state_t::get_state()`,
  `CO_MODIF_WAIT_YIELD_CBK`, `CO_MODIF_PARKED_CBK`, `kill_deferred_t`.

---

## 4. Verification

Built through the scratch `tests/` makefile on Windows (MSVC 19.43), with both patches applied:

| | Result |
|---|---|
| Whole suite, with the new tests and 13 killer/wait/clear tests rebuilt with `COLIB_ENABLE_DEBUG_CHECKS` | **all pass except `018-011` and `018-017`**, which fail the same way on today's `colib.h` (`BUGS.md` #5, #6) |
| `018-015` (Windows timeo read drops bytes) on today's `colib.h` | fails: 273 of 1000 bytes arrived. Passes with the patch |
| The "in two queues at once" `terminate` | never fired |
| Linux (epoll) and kqueue | **not compiled here**; they need a Linux build |

Every step before this stitch was verified the same way (see the four review files). The two
patches below were checked to reapply exactly to today's `colib.h` and `tests/`, giving the tested
files.

**Landing order, reproduce-first:** commit `018-015` and `018-016` first, failing. For that,
`018-016`'s last line (`kill_fn() == co::ERROR_FINISHED`) must be left out until the fix, since
`ERROR_FINISHED` doesn't exist yet. Then this patch, and remove their `BUGS.md` entries.

**Still open** (details in `killer_state.md`):
- `BUGS.md` #5, #6, #7;
- the debug checks keeping their own map instead of checking `state` transitions.

---

## 5. The `colib.h` diff

The hunks are in file order, so together they make the whole patch (87 hunks).

### 5.1 Includes

`<exception>`, for `kill_deferred_t`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -383,6 +383,7 @@ SOFTWARE.
 #include <cinttypes>
 #include <coroutine>
 #include <deque>
+#include <exception>
 #include <functional>
 #include <list>
 #include <map>
```

### 5.2 `error_e`, `kill_deferred_t`, `modif_e`

Two new codes, and the `error_e` block realigned for the longer names. `kill_deferred_t` sits with the other types. The `CO_MODIF_WAIT_IO_CBK` doc says "negative", and warns about retrying without handling the error. UNWAIT fires before ENTER. `CO_MODIF_WAIT_YIELD_CBK` and `CO_MODIF_PARKED_CBK` go at the end, so the existing values keep their numbers.

```diff
--- a/colib.h
+++ b/colib.h
@@ -648,6 +649,7 @@ struct pool_t;
 struct modif_t;
 struct sem_t;
 struct state_t;
+struct state_list_t;
 
 /*! This is a private table that holds the modifications inside the corutine state */
 struct modif_table_t;
@@ -675,14 +677,16 @@ using modif_pack_t = std::vector<modif_p
 /*! Most of the functions from this library return this error type. Warnings or non-errors are
  * positive, while errors are negative. */
 enum error_e : int32_t {
-    ERROR_YIELDED =  1, /*!< not really an error, but used to signal that the coro yielded */
-    ERROR_OK      =  0,
-    ERROR_GENERIC = -1, /*!< generic error, can use log_str to find the error, or sometimes errno */
-    ERROR_TIMEO   = -2, /*!< the error comes from a modif, namely a timeout */
-    ERROR_WAKEUP  = -3, /*!< the error comes from force awaking the awaiter */
-    ERROR_USER    = -4, /*!< the error comes from a modif, namely an user defined modif, users can
-                        use this if they wish to return from modif cbks */
-    ERROR_DEPEND  = -5, /*!< the error comes from a depend modif, i.e. depended function failed */
+    ERROR_FINISHED  =  3, /*!< not an error, the target (of a kill/stop) had already finished */
+    ERROR_SUSPENDED =  2, /*!< not an error, a modif suspended the coroutine and now owns it */
+    ERROR_YIELDED   =  1, /*!< not really an error, but used to signal that the coro yielded */
+    ERROR_OK        =  0,
+    ERROR_GENERIC   = -1, /*!< generic error, can use log_str to find the error, or sometimes errno */
+    ERROR_TIMEO     = -2, /*!< the error comes from a modif, namely a timeout */
+    ERROR_WAKEUP    = -3, /*!< the error comes from force awaking the awaiter */
+    ERROR_USER      = -4, /*!< the error comes from a modif, namely an user defined modif, users can
+                          use this if they wish to return from modif cbks */
+    ERROR_DEPEND    = -5, /*!< the error comes from a depend modif, i.e. depended function failed */
 };
 
 /*! Return type of pool_t::run event loop. */
@@ -693,6 +697,14 @@ enum run_e : int32_t {
     RUN_STOPPED = -3, /*!< can be re-run (comes from force_stop) */
 };
 
+/*! Thrown by a kill called from inside its target, the kill is deferred to its next wait */
+struct kill_deferred_t : public std::exception {
+    explicit kill_deferred_t(state_t *target) : target(target) {}
+    const char *what() const noexcept override { return "colib: kill deferred"; }
+
+    state_t *target;    /*!< the innermost frame of the target */
+};
+
 
 /*! This is the modification type of the modification and it describes the place that this
  * modification should be called from. */
@@ -715,10 +727,12 @@ enum modif_e : int32_t {
     CO_MODIF_ENTER_CBK,
 
     /*! This is called when a corutine is waiting for an IO (after the leave cbk). If the return
-    value is not ERROR_OK, then the wait is aborted. */
+    value is negative, then the wait is aborted. A negative value is returned by the wait at
+    once: code that retries the wait without handling that error spins. */
     CO_MODIF_WAIT_IO_CBK,
 
-    /*! This is called when the io is done and the corutine that awaited it is resumed */
+    /*! This is called when the io is done and the corutine that awaited it is resumed (before the
+    enter cbk) */
     CO_MODIF_UNWAIT_IO_CBK,
 
     /*! This is similar to wait_io, but on a semaphore */
@@ -727,6 +741,13 @@ enum modif_e : int32_t {
     /*! This is similar to unwait_io, but on a semaphore */
     CO_MODIF_UNWAIT_SEM_CBK,
 
+    /*! This is similar to wait_io, but on co::yield() and co::force_stop() */
+    CO_MODIF_WAIT_YIELD_CBK,
+
+    /*! This is called once a coroutine suspended by a modif (ERROR_SUSPENDED) is off the stack, after
+    the resume that suspended it. The modif that suspended it must resume or destroy it. */
+    CO_MODIF_PARKED_CBK,
+
     CO_MODIF_COUNT,
 };
 
```

### 5.3 `pool_t` and `sem_t` lose their pointer

Their destructors and constructors move to the derived types (section 5.11 and 5.14).

```diff
--- a/colib.h
+++ b/colib.h
@@ -859,8 +880,6 @@ struct pool_t {
     pool_t &operator = (pool_t& sem) = delete;
     pool_t &operator = (pool_t&& sem) = delete;
 
-    ~pool_t() { clear(); }
-
     /*! Schedules the task with the modifications specified in v to be executed on the pool.
      * That is, it adds the task to the ready_queue.
      * 
@@ -925,10 +944,8 @@ protected:
     std::unique_ptr<allocator_memory_t> allocator_memory;
 
     friend inline std::shared_ptr<pool_t> create_pool();
+    friend struct pool_internal_t;
     pool_t();
-
-private:
-    std::unique_ptr<pool_internal_t> internal;
 };
 
 /*! This is a semaphore working on a pool. It can be awaited to decrement it's count and .signale()
@@ -954,9 +971,6 @@ struct sem_t {
     sem_t &operator = (sem_t& sem) = delete;
     sem_t &operator = (sem_t&& sem) = delete;
 
-    /* If the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire
-    call stack) */
-    ~sem_t();
 
     /*! This awaiter object returns an unlocker that has the `lock` member function doing nothing
      * and `unlock` function calling `signal` on the semaphore, meaning it can be used inside a
@@ -993,7 +1007,7 @@ struct sem_t {
     /*! Again, beeter don't touch, same as pool. This is public only to ease the writing of the
      * implementation. @{ */
     sem_internal_t *get_internal();
-    void invalidate_self() { internal = nullptr; }
+    void invalidate_self();
     /*! @} */
 
 protected:
@@ -1001,11 +1015,9 @@ protected:
     friend inline T *alloc(pool_t *, Args&&...);
 
     friend inline sem_p create_sem(pool_t *pool, int64_t val);
+    friend struct sem_internal_t;
 
-    sem_t(pool_t *pool, int64_t val = 0);
-
-private:
-    std::unique_ptr<sem_internal_t> internal;
+    sem_t() {}
 };
 
 
```

### 5.4 `state_e`, `state_t`, `sem_waiter_handle_p`, the WAIT_YIELD slot in `modif_t`

`get_state()` is public. The node (`state`, `enter_on_start`, `prev`, `next`, `list`) is private.

```diff
--- a/colib.h
+++ b/colib.h
@@ -1082,6 +1094,17 @@ COLIB_OS_UNKNOWN_IO_DESC
 
 #endif /* COLIB_OS_UNKNOWN */
 
+/*! Where a coroutine is, see state_t::get_state() */
+enum state_e : int32_t {
+    STATE_LEFT = 0,     /*!< suspended, not waiting on anything colib knows (a call, a park) */
+    STATE_RUNNING,      /*!< executing */
+    STATE_READY,        /*!< in its pool's ready queue */
+    STATE_WAITING_SEM,  /*!< in a semaphore's wait list */
+    STATE_WAITING_IO,   /*!< waiting on an io */
+    STATE_PARKED,       /*!< suspended by a modif (ERROR_SUSPENDED), its owner not called back yet */
+    STATE_DONE,         /*!< a scheduled root that finished, destroyed after this resume */
+};
+
 /*! Internal state of corutines that is independent of the return value of the corutine.
  * This structure, as explained above, is the common type for all coroutines from this library.
  * It also holds a user pointer user_ptr that can be used. This pointer can be useful when
@@ -1101,26 +1124,23 @@ struct state_t {
     std::shared_ptr<void> user_ptr;         /*!< this is a pointer that the user can use for whatever
                                             he feels like. This library will not touch this pointer */
 
-    ~state_t();                             /*!< Only used on debug */
+    state_e get_state() const { return state; }
+
+    ~state_t();                             /*!< Unlinks it from the queue it is in */
+
+private:
+    friend struct state_list_t;
+    friend struct state_access_t;
+
+    state_e state = STATE_LEFT;
+    bool enter_on_start = false;            /* scheduled: ENTER when the pool first runs it */
+    state_t *prev = nullptr;
+    state_t *next = nullptr;
+    state_list_t *list = nullptr;           /* the queue it is linked in, if any */
 };
 
-/*! This is mostly internal. Internal pointer to an iterator inside the semaphore awaiter queue. It
- * will be given as a parameter inside the callback of a modifier, */
-using sem_waiter_handle_t = std::list<                          /* List with the semaphore waiters */
-    std::pair<
-        state_t *,                  /* The waiting corutine state */
-        std::shared_ptr<void>       /* Where the shared_ptr is actually stored */
-    >,
-    allocator_t<std::
-        pair<
-            state_t *,
-            std::shared_ptr<void>
-        >
-    >                               /* profiling shows the default is slow */
->::iterator;                        /* iterator in the respective list */
-using sem_waiter_handle_p =
-        std::shared_ptr<sem_waiter_handle_t>;   /* if this pointer is not available, the waiter was
-                                                   evicted from the waiters list */
+/*! The waiter in a semaphore's wait list, given as a parameter to the wait_sem modif callback */
+using sem_waiter_handle_p = state_t *;
             
 
 /*! Modifs, corutine modifications. Those modifications controll the way a corutine behaves when
@@ -1141,11 +1161,14 @@ struct modif_t {
         std::function<error_e(state_t *, io_desc_t&)>,  /* wait_io_cbk */
         std::function<error_e(state_t *, io_desc_t&)>,  /* unwait_io_cbk */
 
-         /* wait_sem_cbk - OBS: the std::shared_ptr<void> part can be ignored, it's internal */
+         /* wait_sem_cbk */
         std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,
 
          /* unwait_sem_cbk - No handle here, as the semaphore is no longer in the waiting list */
-        std::function<error_e(state_t *, sem_t *)>
+        std::function<error_e(state_t *, sem_t *)>,
+
+        std::function<error_e(state_t *)>,              /* wait_yield_cbk */
+        std::function<error_e(state_t *)>               /* parked_cbk */
     >;
 
     /*! This is the callback that will be called on the location specified by type. It must be
```

### 5.5 `create_killer`'s doc

The original doc, plus the contract in two lines.

```diff
--- a/colib.h
+++ b/colib.h
@@ -1552,6 +1575,8 @@ inline task<sem_p> create_sem(int64_t va
  * callback that runs as a side effect of the kill it already triggered - is caught and rejected
  * the same way (also reported as "nothing to kill"), rather than corrupting the in-progress
  * unwind.
+ * When the kill returns, the target is dead. A kill called from inside the target can't be done
+ * now: it is deferred to the target's next wait and throws kill_deferred_t.
  * @param pool The pool on which to bind this killer. The killer may outlive it: nothing of the
  *             killer is allocated from the pool.
  * @param e The error value that will be set inside the killed coroutine on kill
```

### 5.6 Debug-check declarations

The WAIT_YIELD check. A semaphore wait is recorded by the waiting state.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2231,6 +2256,7 @@ struct dbg_scope_t {
 # define COLIB_DEBUG_CHECK_UNWAIT_IO(s, io) dbg_check_modif_unwait_io(s, io)
 # define COLIB_DEBUG_CHECK_WAIT_SEM(s, sem, it) dbg_check_modif_wait_sem(s, sem, it)
 # define COLIB_DEBUG_CHECK_UNWAIT_SEM(s, sem) dbg_check_modif_unwait_sem(s, sem)
+# define COLIB_DEBUG_CHECK_WAIT_YIELD(s) dbg_check_modif_wait_yield(s)
 
 # define COLIB_ENABLE_DEBUG_CHECK_ASSERT(x, fmt, ...) \
 do { \
@@ -2250,6 +2276,7 @@ inline void dbg_check_modif_wait_io(stat
 inline void dbg_check_modif_unwait_io(state_t *s, io_desc_t &io);
 inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem, sem_waiter_handle_p it);
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem);
+inline void dbg_check_modif_wait_yield(state_t *s);
 
 struct dbg_check_state_t {
     uint32_t called : 1 = false;
@@ -2260,7 +2287,7 @@ struct dbg_check_state_t {
     io_desc_t *io = nullptr; /* it's ok, to compare ptrs, else we would copy and incr the
                                 ref of a pointer on windows that would alter the behaviour */
     sem_t *sem = nullptr;
-    sem_waiter_handle_t *sem_it = nullptr;
+    state_t *sem_it = nullptr;
 };
 
 inline std::map<pool_t *,
@@ -2277,6 +2304,7 @@ inline std::map<pool_t *,
 # define COLIB_DEBUG_CHECK_UNWAIT_IO(...) ;
 # define COLIB_DEBUG_CHECK_WAIT_SEM(...) ;
 # define COLIB_DEBUG_CHECK_UNWAIT_SEM(...) ;
+# define COLIB_DEBUG_CHECK_WAIT_YIELD(...) ;
 # define COLIB_ENABLE_DEBUG_CHECK_ASSERT(x, fmt, ...) ;
 #endif /*COLIB_ENABLE_DEBUG_CHECKS*/
 
```

### 5.7 `state_list_t` and `state_access_t`

The intrusive queue. Linking a coroutine that's already queued terminates, since it's a colib bug. `state_access_t` is colib's only way into the private node.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2289,9 +2317,76 @@ constexpr auto has(T&& data_struct, K&&
             != std::forward<T>(data_struct).end();
 }
 
-using sem_wait_list_t = std::list<std::pair<state_t *, std::shared_ptr<void>>,
-        allocator_t<std::pair<state_t *,std::shared_ptr<void>>>>;
-using sem_wait_list_it = sem_wait_list_t::iterator;
+/* A queue of coroutines, linked through their own state_t: no allocation, a coroutine is in at
+most one queue, and it leaves it when destroyed. `kind` is the state of the ones linked in it. */
+struct state_list_t {
+    state_list_t(state_e kind) : kind(kind) {}
+    state_list_t(const state_list_t &) = delete;
+    state_list_t &operator = (const state_list_t &) = delete;
+    ~state_list_t() { while (head) unlink(head); }
+
+    void push_back(state_t *s) { link(s, tail, nullptr); }
+    void push_front(state_t *s) { link(s, nullptr, head); }
+    state_t *front() { return head; }
+    state_t *pop_front() {
+        state_t *s = head;
+        if (s)
+            unlink(s);
+        return s;
+    }
+    bool has(state_t *s) const { return s->list == this; }
+    bool empty() const { return !head; }
+    size_t size() const { return cnt; }
+
+    void unlink(state_t *s) {
+        (s->prev ? s->prev->next : head) = s->next;
+        (s->next ? s->next->prev : tail) = s->prev;
+        s->prev = s->next = nullptr;
+        s->list = nullptr;
+        s->state = STATE_LEFT;
+        cnt--;
+    }
+
+    /* puts `with` in the place of `s` */
+    void replace(state_t *s, state_t *with) {
+        state_t *after = s->prev;
+        unlink(s);
+        link(with, after, after ? after->next : head);
+    }
+
+private:
+    void link(state_t *s, state_t *prev, state_t *next) {
+        if (s->list)
+            std::terminate();   /* in two queues at once: a colib bug */
+        s->prev = prev;
+        s->next = next;
+        (prev ? prev->next : head) = s;
+        (next ? next->prev : tail) = s;
+        s->list = this;
+        s->state = kind;
+        cnt++;
+    }
+
+    state_e kind;
+    state_t *head = nullptr;
+    state_t *tail = nullptr;
+    size_t cnt = 0;
+};
+
+/* colib's own access to the private parts of state_t */
+struct state_access_t {
+    static void set(state_t *s, state_e state) { s->state = state; }
+    static void set_enter_on_start(state_t *s) { s->enter_on_start = true; }
+    static bool take_enter_on_start(state_t *s) {
+        bool ret = s->enter_on_start;
+        s->enter_on_start = false;
+        return ret;
+    }
+    static void unlink(state_t *s) {
+        if (s->list)
+            s->list->unlink(s);
+    }
+};
 
 /* Those are needed for destroy_state, internally and to call it */
 inline error_e do_leave_modifs(state_t *state);
```

### 5.8 Modif table and dispatch

The tenth vector and its `static_assert`. The dispatch rule: an error wins, then `ERROR_SUSPENDED`; callbacks whose return is ignored always all run; the closing ones run in reverse. ENTER and LEAVE set the state. `do_yield_modifs` and `do_parked_modifs`, plus the declaration of `push_parked()`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2449,9 +2544,12 @@ struct modif_table_t {
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
+            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
+            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}}
     } {}
 
+    static_assert(CO_MODIF_COUNT == 11, "one vector per modif_e in the constructor above");
     std::array<std::vector<modif_p, allocator_t<modif_p>>, CO_MODIF_COUNT> table;
 };
 
@@ -2491,18 +2589,31 @@ inline void inherit_modifs(state_t *stat
         state->modif_table = new_table;   
 }
 
+/* An error wins (the wait doesn't happen), then ERROR_SUSPENDED, then ERROR_OK */
 template <modif_e cbk_id, typename ...Args>
 inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
+    /* the return value of those is ignored, so one of them can't stop the others */
+    constexpr bool ignored_ret =
+            cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
+            cbk_id == CO_MODIF_ENTER_CBK || cbk_id == CO_MODIF_UNWAIT_IO_CBK ||
+            cbk_id == CO_MODIF_UNWAIT_SEM_CBK || cbk_id == CO_MODIF_PARKED_CBK;
+    /* the closing ones run in reverse, so they nest like destructors */
+    constexpr bool reversed =
+            cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
+            cbk_id == CO_MODIF_UNWAIT_IO_CBK || cbk_id == CO_MODIF_UNWAIT_SEM_CBK;
+    error_e result = ERROR_OK;
     if (auto modif_table = state->modif_table) {
-        for (auto &modif : modif_table->table[cbk_id]) {
+        auto &cbks = modif_table->table[cbk_id];
+        for (size_t i = 0; i < cbks.size(); i++) {
+            auto &modif = reversed ? cbks[cbks.size() - 1 - i] : cbks[i];
             error_e ret = std::get<cbk_id>(modif->cbk)(state, args...);
-            if (ret != ERROR_OK) {
-                /* If any callback returned an error, we stop their execution and return the error. */
+            if (ret < 0 && !ignored_ret)
                 return ret;
-            }
+            if (ret == ERROR_SUSPENDED)
+                result = ERROR_SUSPENDED;
         }
     }
-    return ERROR_OK;
+    return result;
 }
 
 inline error_e do_sched_modifs(state_t *state) {
@@ -2520,12 +2631,14 @@ inline error_e do_call_modifs(state_t *s
 inline error_e do_leave_modifs(state_t *state) {
     COLIB_DEBUG_TRACE("     LEAVE: %s state: %p", dbg_name(state->self).c_str(), state);
     COLIB_DEBUG_CHECK_LEAVE(state);
+    state_access_t::set(state, STATE_LEFT);
     return do_generic_modifs<CO_MODIF_LEAVE_CBK>(state);
 }
 
 inline error_e do_entry_modifs(state_t *state) {
     COLIB_DEBUG_TRACE("     ENTER: %s state: %p", dbg_name(state->self).c_str(), state);
     COLIB_DEBUG_CHECK_ENTER(state);
+    state_access_t::set(state, STATE_RUNNING);
     return do_generic_modifs<CO_MODIF_ENTER_CBK>(state);
 }
 
@@ -2561,6 +2674,20 @@ inline error_e do_unwait_sem_modifs(stat
     return do_generic_modifs<CO_MODIF_UNWAIT_SEM_CBK>(state, sem);
 }
 
+inline error_e do_yield_modifs(state_t *state) {
+    COLIB_DEBUG_TRACE("   YIELD: %s state: %p", dbg_name(state->self).c_str(), state);
+    COLIB_DEBUG_CHECK_WAIT_YIELD(state);
+    return do_generic_modifs<CO_MODIF_WAIT_YIELD_CBK>(state);
+}
+
+inline error_e do_parked_modifs(state_t *state) {
+    COLIB_DEBUG_TRACE("  PARKED: %s state: %p", dbg_name(state->self).c_str(), state);
+    return do_generic_modifs<CO_MODIF_PARKED_CBK>(state);
+}
+
+/* a modif suspended it (ERROR_SUSPENDED): its owner is called back after this resume */
+inline void push_parked(state_t *state);
+
 /* considering you may want to create a modif at runtime this seems to be the best way */
 template <modif_e type_id, typename Cbk>
 inline modif_p create_modif(modif_flags_e flags, Cbk&& cbk) {
```

### 5.9 `task<T>::await_suspend`

`ERROR_SUSPENDED` on a call: the callee is parked, and the modif that suspended it owns both frames.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2679,7 +2806,12 @@ inline handle<void> task<T>::await_suspe
 
     inherit_modifs(state, caller.promise().state.modif_table, CO_MODIF_INHERIT_ON_CALL);
 
-    if (do_call_modifs(state) != ERROR_OK) {
+    error_e err = do_call_modifs(state);
+    if (err == ERROR_SUSPENDED) {
+        push_parked(state);
+        return std::noop_coroutine();
+    }
+    if (err != ERROR_OK) {
         do_entry_modifs(&caller.promise().state);
         return caller;
     }
```

### 5.10 The io pools

Every backend takes the ready queue as a `state_list_t`, including the `COLIB_OS_UNKNOWN` skeleton. Each backend answers `completed(io_desc)`: did the io already do its work before its coroutine resumed. epoll and kqueue always answer no, since they only wait for readiness; IOCP answers yes for a request that moved its data. Their `clear()` closes a waiting coroutine's wait with only its UNWAIT, since it already left. On Windows, `force_awake` always waits for the cancel to settle, and delivers an io that completed first (`ERROR_FINISHED`).

```diff
--- a/colib.h
+++ b/colib.h
@@ -2742,7 +2874,7 @@ inline state_t *task<T>::get_state() {
 #if COLIB_OS_UNIX
 
 struct io_pool_t {
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}
     {
 #ifndef KQUEUE_CLOEXEC
@@ -2799,6 +2931,9 @@ struct io_pool_t {
         return ERROR_OK;
     }
 
+    /* kqueue only waits for readiness, the io runs after the resume: nothing is done before it */
+    bool completed(const io_desc_t&) { return false; }
+
     error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
         /* TODO: figure it out, for this and for the others, maybe I can find a way not to use
         a map */
@@ -2813,7 +2948,7 @@ struct io_pool_t {
 private:
     int kq = -1;
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
 };
 
 struct timer_pool_t {
@@ -2849,7 +2984,7 @@ struct io_pool_t {
         std::vector<waiter_t, allocator_t<waiter_t>> waiters;
     };
 
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     :       pool{pool},
             fd_data_slow(allocator_t<int>{pool}),
             ret_evs(allocator_t<int>{pool}),
@@ -2994,6 +3129,9 @@ struct io_pool_t {
         }
     }
 
+    /* epoll only waits for readiness, the io runs after the resume: nothing is done before it */
+    bool completed(const io_desc_t&) { return false; }
+
     error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
         auto data = get_data(io_desc.fd);
         if (!data || !(data->mask & io_desc.events)) {
@@ -3022,9 +3160,7 @@ struct io_pool_t {
             if (auto *data = fd_data_fast[i]) {
                 for (auto &w : data->waiters) {
                     io_desc_t desc{ .fd = i, .events = w.mask };
-                    do_entry_modifs(w.state);
                     do_unwait_io_modifs(w.state, desc);
-                    do_leave_modifs(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = i, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3039,9 +3175,7 @@ struct io_pool_t {
             if (data) {
                 for (auto &w : data->waiters) {
                     io_desc_t desc{ .fd = fd, .events = w.mask };
-                    do_entry_modifs(w.state);
                     do_unwait_io_modifs(w.state, desc);
-                    do_leave_modifs(w.state);
                     destroy_state(w.state);
                 }
                 if (remove_waiter(io_desc_t{ .fd = fd, .events = 0xffff'ffff }) != ERROR_OK) {
@@ -3135,7 +3269,7 @@ private:
 
     std::vector<struct epoll_event, allocator_t<struct epoll_event>> ret_evs;
 
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
     int epoll_fd = -1;
 };
 
@@ -3254,7 +3388,7 @@ struct io_pool_t {
     using set_type = std::set<ptr_type, std::less<set_val_type>, allocator_t<set_val_type>>;
     using map_val_type = std::map<HANDLE, set_type>::value_type;
 
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}, handles{allocator_t<map_val_type>{pool}}
     {
         iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 1);
@@ -3416,6 +3550,13 @@ struct io_pool_t {
         return ERROR_OK;
     }
 
+    /* the request already did its work (moved its data) before its coroutine resumed; a timer or
+    a stop_io wake-up did nothing */
+    bool completed(const io_desc_t& io_desc) {
+        return io_desc.data && !(io_desc.data->flags & io_data_t::IO_FLAG_TIMER) &&
+                io_desc.data->state && io_desc.data->state->err == ERROR_OK;
+    }
+
     /* the state (singular) that is waiting for io_desc must be awakened */
     error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
         COLIB_ENABLE_DEBUG_CHECK_ASSERT(io_desc.h, "invalid handle");
@@ -3424,6 +3565,8 @@ struct io_pool_t {
             COLIB_ENABLE_DEBUG_CHECK_ASSERT(data, "invalid data ptr");
             COLIB_ENABLE_DEBUG_CHECK_ASSERT(data->h, "invalid inner handle");
             COLIB_DEBUG_TRACE("awake: handle: %p", data->h);
+            bool finished = false;
+            DWORD transferred = 0;
             if ((data->flags & io_data_t::IO_FLAG_TIMER) &&
                     (data->flags & io_data_t::IO_FLAG_TIMER_RUN))
             {
@@ -3435,19 +3578,16 @@ struct io_pool_t {
                 data->flags = io_data_t::io_flag_e(data->flags & ~io_data_t::IO_FLAG_TIMER_RUN);
             }
             else {
-                if (!CancelIoEx(data->h, &data->overlapped)) {
-                    if (GetLastError() != ERROR_NOT_FOUND) {
-                        COLIB_DEBUG("Failed to cancel io: %s [%x] h: %p",
-                                get_last_error().c_str(), GetLastError(), data->h);
-                        return ERROR_GENERIC;
-                    }
-                }
-                else {
-                    /* CancelIoEx is async it seems, we need to call GetOverlappedResult to actually
-                    make it wait until the cancel is sent to. */
-                    DWORD aux_bytes;
-                    GetOverlappedResult(data->h, &data->overlapped, &aux_bytes, TRUE);
+                BOOL cancelled = CancelIoEx(data->h, &data->overlapped);
+                if (!cancelled && GetLastError() != ERROR_NOT_FOUND) {
+                    COLIB_DEBUG("Failed to cancel io: %s [%x] h: %p",
+                            get_last_error().c_str(), GetLastError(), data->h);
+                    return ERROR_GENERIC;
                 }
+                /* CancelIoEx is async it seems, we need to call GetOverlappedResult to actually
+                make it wait until the cancel is sent to. */
+                BOOL ok = GetOverlappedResult(data->h, &data->overlapped, &transferred, TRUE);
+                finished = ok && !(data->flags & io_data_t::IO_FLAG_TIMER);
             }
 
             /* If events where queued we need to handle them here, that is so
@@ -3458,6 +3598,13 @@ struct io_pool_t {
                 return ERROR_GENERIC;
             }
 
+            if (finished) {
+                /* the io completed before the cancel, it is delivered */
+                data->recvlen = transferred;
+                data->state->err = ERROR_OK;
+                awake_io(data.get());
+                return ERROR_FINISHED;
+            }
             data->state->err = retcode;
             awake_io(data.get());
 
@@ -3473,7 +3620,7 @@ struct io_pool_t {
             }
             for (auto data : datas) {
                 error_e err;
-                if ((err = awake_data(data)) != ERROR_OK)
+                if ((err = awake_data(data)) < 0)
                     return err;
             }
         }
@@ -3517,9 +3664,7 @@ struct io_pool_t {
                     }
                 }
                 io_desc_t desc{ .data = data, .h = data->h };
-                do_entry_modifs(data->state);
                 do_unwait_io_modifs(data->state, desc);
-                do_leave_modifs(data->state);
                 destroy_state(data->state);
             }
         }
@@ -3570,7 +3715,7 @@ private:
     }
 
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
     HANDLE iocp = nullptr;
 
     std::map<HANDLE, set_type, std::less<HANDLE>, allocator_t<map_val_type>> handles;
@@ -3679,7 +3824,7 @@ COLIB_OS_UNKNOWN_IMPLEMENTATION
 // Those two structs need implemented:
 
 struct io_pool_t {
-    io_pool_t(pool_t *pool, std::deque<state_t *, allocator_t<state_t *>> &ready_tasks)
+    io_pool_t(pool_t *pool, state_list_t &ready_tasks)
     : pool{pool}, ready_tasks{ready_tasks}
     {}
 
@@ -3701,6 +3846,9 @@ struct io_pool_t {
     // this waiter inside this pool
     error_e add_waiter(state_t *state, const io_desc_t& io_desc) {}
 
+    // true if the io already did its work before its coroutine resumed (ex: data moved)
+    bool completed(const io_desc_t& io_desc) {}
+
     // the state (singular) that is waiting for io_desc must be awakened
     error_e force_awake(const io_desc_t& io_desc, error_e retcode) {}
 
@@ -3712,7 +3860,7 @@ struct io_pool_t {
 
 private:
     pool_t *pool = nullptr;
-    std::deque<state_t *, allocator_t<state_t *>> &ready_tasks;
+    state_list_t &ready_tasks;
 };
 
 struct timer_pool_t {
```

### 5.11 `pool_internal_t`

- It derives from `pool_t`, and its destructor clears.
- `sched()` marks the coroutine to be ENTERed on its first run, and `next_task_state()` does that ENTER. The pop marks the coroutine `RUNNING`, and terminates on a killer's placeholder.
- `run_posted()` hands every parked coroutine to its owner (`CO_MODIF_PARKED_CBK`) and destroys every finished root; both are intrusive lists (`parked_tasks`, `done_tasks`). `clear()` runs it first. A second exception posted in the same resume terminates (section 3).
- `replace_ready`/`remove_ready` are O(1).
- `io_completed()` passes the killer's question on to the backend.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3737,15 +3885,18 @@ private:
 
 #endif /* COLIB_OS_UNKNOWN */
 
-struct pool_internal_t {
-    pool_internal_t(pool_t *_pool)
-    :   pool(_pool),
-        ready_tasks{allocator_t<state_t *>{_pool}},
-        io_pool{_pool, ready_tasks},
-        timer_pool(_pool, io_pool),
-        sem_pool{allocator_t<sem_t *>{_pool}}
+/* The pool itself: pool_t is its public part, create_pool() makes one of these */
+struct pool_internal_t : public pool_t {
+    pool_internal_t()
+    :   pool(this),
+        ready_tasks{STATE_READY},
+        io_pool{this, ready_tasks},
+        timer_pool(this, io_pool),
+        sem_pool{allocator_t<sem_t *>{this}}
     {}
 
+    ~pool_internal_t() { clear(); }
+
     template <typename T>
     void sched(task<T> task, const modif_pack_t& own_modifs, modif_table_p parent_table) {
         /* first we give our new task the pool */
@@ -3760,8 +3911,7 @@ struct pool_internal_t {
         if (do_sched_modifs(state) != ERROR_OK) {
             return ;
         }
-
-        do_entry_modifs(state);
+        state_access_t::set_enter_on_start(state);
 
         /* third, we add the task to the pool */
         ready_tasks.push_back(state);
@@ -3799,10 +3949,7 @@ struct pool_internal_t {
             stopping. */
             state->self.resume();
 
-            if (posted_to_destroy) {
-                posted_to_destroy->self.destroy();
-                posted_to_destroy = nullptr;
-            }
+            run_posted();
             if (posted_exception) {
                 auto pe = posted_exception;
                 posted_exception = nullptr;
@@ -3819,13 +3966,29 @@ struct pool_internal_t {
     }
 
     void post_to_destroy(state_t *s) {
-        posted_to_destroy = s;
+        done_tasks.push_back(s);
     }
 
     void post_exception(std::exception_ptr exc) {
+        if (posted_exception)
+            std::terminate();   /* a second one in the same resume has nowhere to go */
         posted_exception = exc;
     }
 
+    void push_parked(state_t *state) {
+        parked_tasks.push_back(state);
+    }
+
+    /* running the posted work can post more */
+    void run_posted() {
+        while (!parked_tasks.empty() || !done_tasks.empty()) {
+            while (state_t *s = parked_tasks.pop_front())
+                do_parked_modifs(s);
+            while (state_t *s = done_tasks.pop_front())
+                s->self.destroy();
+        }
+    }
+
     void post_stop() {
         posted_stop = true;
     }
@@ -3838,16 +4001,20 @@ struct pool_internal_t {
         ready_tasks.push_front(state);
     }
 
+    /* puts `with` in the place of `state` in the ready queue */
+    bool replace_ready(state_t *state, state_t *with) {
+        if (!ready_tasks.has(state))
+            return false;
+        ready_tasks.replace(state, with);
+        return true;
+    }
+
     bool remove_ready(state_t *state) {
         COLIB_DEBUG_TRACE_SCOPE("state: %p", state);
-
-        auto it = std::remove(ready_tasks.begin(), ready_tasks.end(), state);
-        if (it != ready_tasks.end()) {
-            ready_tasks.erase(it, ready_tasks.end());
-            COLIB_DEBUG_TRACE("returned true");
-            return true;
-        }
-        return false;
+        if (!ready_tasks.has(state))
+            return false;
+        ready_tasks.unlink(state);
+        return true;
     }
 
     bool has_next_task_state() {
@@ -3892,8 +4059,12 @@ struct pool_internal_t {
         }
 
         if (!ready_tasks.empty()) {
-            auto ret = ready_tasks.front();
-            ready_tasks.pop_front();
+            auto ret = ready_tasks.pop_front();
+            if (!ret->self)
+                std::terminate();   /* a killer's placeholder: the scheduler ran inside a kill */
+            state_access_t::set(ret, STATE_RUNNING);
+            if (state_access_t::take_enter_on_start(ret))
+                do_entry_modifs(ret);
             COLIB_DEBUG_TRACE("next_state: %p", ret);
             return ret;
         }
@@ -3919,6 +4090,11 @@ struct pool_internal_t {
         return io_pool.force_awake(io_desc, retcode);
     }
 
+    bool io_completed(const io_desc_t& io_desc) {
+        /* the io already did its work before its coroutine resumed */
+        return io_pool.completed(io_desc);
+    }
+
     error_e get_timer(io_desc_t& new_timer) {
         /* returns a timer object, referenced by the io descriptor new_timer, gaining ownership over
         it */
@@ -3955,12 +4131,13 @@ struct pool_internal_t {
 
 private:
     pool_t *pool;
-    std::deque<state_t *, allocator_t<state_t *>> ready_tasks;
+    state_list_t ready_tasks;
     io_pool_t io_pool;
     timer_pool_t timer_pool;
 
     std::exception_ptr posted_exception = nullptr;
-    state_t *posted_to_destroy = nullptr;
+    state_list_t done_tasks{STATE_DONE};
+    state_list_t parked_tasks{STATE_PARKED};
     bool posted_stop = false;
 
     /* bookkeeping for end of life destruction */
```

### 5.12 Exit paths

`ERROR_SUSPENDED` from EXIT means "don't jump into the caller, a modif queued it", in `cpp_yield_awaiter` (with or without a caller) and in `final_awaiter_cleanup`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4022,11 +4199,15 @@ inline handle<void> cpp_yield_awaiter(st
 
     /* from the point of view of the corutine modifications we are exiting here, this keeps the
     call stack proper */
-    do_exit_modifs(yielding_task_state);
+    error_e exit_err = do_exit_modifs(yielding_task_state);
 
     if (caller_state) {
+        if (exit_err == ERROR_SUSPENDED)
+            return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
+    if (exit_err == ERROR_SUSPENDED)
+        return std::noop_coroutine();       /* a modif took over */
 
     return yielding_task_state->pool->get_internal()->next_task();
 }
@@ -4034,11 +4215,12 @@ inline handle<void> cpp_yield_awaiter(st
 inline handle<void> final_awaiter_cleanup(state_t *ending_task_state) {
     COLIB_DEBUG_TRACE_SCOPE("state: %p", ending_task_state);
     do_leave_modifs(ending_task_state);
-    /* not sure if I should do something with the return value ... */
     state_t *caller_state = ending_task_state->caller_state;
-    do_exit_modifs(ending_task_state);
+    error_e exit_err = do_exit_modifs(ending_task_state);
 
     if (caller_state) {
+        if (exit_err == ERROR_SUSPENDED)
+            return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
     auto pool = ending_task_state->pool;
```

### 5.13 `pool_t`'s methods and `create_pool()`

`get_internal()` becomes a cast, `create_pool()` builds a `pool_internal_t`, and `push_parked()` is defined.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4054,23 +4236,22 @@ inline handle<void> final_awaiter_cleanu
 
 inline pool_t::pool_t() {
     allocator_memory = std::make_unique<allocator_memory_t>();
-    internal = std::make_unique<pool_internal_t>(this);
 }
 
 template <typename T>
 inline void pool_t::sched(task<T> task, const modif_pack_t& v) {
-    internal->sched(task, v, nullptr);
+    get_internal()->sched(task, v, nullptr);
 }
 
 #if COLIB_ENABLE_MULTITHREAD_SCHED
 template <typename T>
 inline void pool_t::thread_sched(task<T> task) {
-    internal->thread_sched(task);
+    get_internal()->thread_sched(task);
 }
 #endif /* COLIB_ENABLE_MULTITHREAD_SCHED */
 
 inline run_e pool_t::run() {
-    return internal->run();
+    return get_internal()->run();
 }
 
 /*  OBS: if clear is called, it is called from outside of the pool, else this is UB
@@ -4096,11 +4277,15 @@ inline error_e pool_t::clear() {
 }
 
 inline intptr_t pool_t::get_internal_handle() {
-    return internal->get_internal_handle();
+    return get_internal()->get_internal_handle();
 }
 
 inline pool_internal_t *pool_t::get_internal() {
-    return internal.get();
+    return static_cast<pool_internal_t *>(this);
+}
+
+inline void push_parked(state_t *state) {
+    state->pool->get_internal()->push_parked(state);
 }
 
 inline error_e pool_t::stop_io(const io_desc_t& io_desc) {
@@ -4108,7 +4293,7 @@ inline error_e pool_t::stop_io(const io_
 }
 
 inline std::shared_ptr<pool_t> create_pool() {
-    return std::shared_ptr<pool_t>(new pool_t{});
+    return std::shared_ptr<pool_t>(new pool_internal_t{});
 }
 
 /* External Part
```

### 5.14 Awaiters: `yield`, `io`

LEAVE comes before WAIT. A park unwaits, links the state into the parked list and returns; an abort unwaits, ENTERs and continues. A registered io marks the coroutine `WAITING_IO`. The resume does UNWAIT, then ENTER.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4167,22 +4352,33 @@ struct yield_awaiter_t {
 
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
-        do_leave_modifs(&h.promise().state);
-        pool->get_internal()->push_ready(&h.promise().state);
+
+        do_leave_modifs(state);
+        error_e err = do_yield_modifs(state);
+        if (err == ERROR_SUSPENDED) {
+            push_parked(state);
+            return std::noop_coroutine();   /* parked by a modif */
+        }
+        if (err != ERROR_OK) {
+            do_entry_modifs(state);
+            return h;       /* refused by a modif */
+        }
+
+        pool->get_internal()->push_ready(state);
+        triggered = true;
         // TODO: is it required to call the modifs here if the returned coroutine is the same as
         // this one or does c++ call resume either way?
-        // auto ret = pool->get_internal()->next_task();
-        // if (ret == h)
-        // 	do_entry_modifs(ret);
         return pool->get_internal()->next_task();
     }
 
     void await_resume() {
         COLIB_DEBUG_TRACE_SCOPE("yield state: %p", state);
-        do_entry_modifs(state);
+        if (triggered)
+            do_entry_modifs(state);
     }
 
-    state_t *state;
+    state_t *state = nullptr;
+    bool triggered = false;
 };
 
 template <typename T>
@@ -4267,8 +4463,20 @@ struct io_awaiter_t {
 
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
-        /* in case we can't schedule the fd we log the failure and return the same coro */
-        if ((ret_err = do_wait_io_modifs(state, io_desc)) != ERROR_OK) {
+
+        do_leave_modifs(state);
+        error_e err = do_wait_io_modifs(state, io_desc);
+        if (err == ERROR_SUSPENDED) {
+            /* parked by a modif */
+            do_unwait_io_modifs(state, io_desc);
+            push_parked(state);
+            return std::noop_coroutine();
+        }
+        if (err != ERROR_OK) {
+            /* aborted by a modif */
+            ret_err = err;
+            do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
             return h;
         }
 
@@ -4276,9 +4484,11 @@ struct io_awaiter_t {
         if (ret_err != ERROR_OK) {
             COLIB_DEBUG("Failed to register wait: %s on: %s",
                     dbg_enum(ret_err).c_str(), dbg_name(h).c_str());
+            do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
             return h;
         }
-        do_leave_modifs(state);
+        state_access_t::set(state, STATE_WAITING_IO);
         triggered = true;
         return pool->get_internal()->next_task();
     }
@@ -4287,8 +4497,8 @@ struct io_awaiter_t {
         COLIB_DEBUG_TRACE_SCOPE("io-unwait state: %p", state);
 
         if (triggered) {
-            do_entry_modifs(state);
             do_unwait_io_modifs(state, io_desc);
+            do_entry_modifs(state);
         }
         if (ret_err != ERROR_OK)
             return ret_err;
```

### 5.15 `sem_internal_t`, `pool_internal_t::clear()`, `sem_awaiter_t`, `sem_t`'s methods

`sem_internal_t` derives from `sem_t`. Its wait list is intrusive and FIFO, as before. A semaphore that outlived its pool gets `pool = nullptr`. The awaiter links the waiter after LEAVE. The pool's `clear()` pops before it destroys.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4307,11 +4517,29 @@ private:
 /* Semaphore
 ------------------------------------------------------------------------------------------------- */
 
-struct sem_internal_t {
-    using sem_aloc = allocator_t<std::pair<std::coroutine_handle<void>, std::shared_ptr<void>>>;
+/* The semaphore itself: sem_t is its public part, create_sem() makes one of these */
+struct sem_internal_t : public sem_t {
+    sem_internal_t(pool_t *pool, int64_t val)
+    : pool(pool), val(val), waiting_on_sem(STATE_WAITING_SEM)
+    {
+        pool->get_internal()->add_sem(this);
+    }
 
-    sem_internal_t(pool_t *pool, int64_t val, sem_t *selfptr)
-    : pool(pool), val(val), waiting_on_sem(sem_aloc{pool}), selfptr(selfptr) {}
+    /* If the semaphore dies while waiters wait, they will all be forcefully destroyed (their entire
+    call stack) */
+    ~sem_internal_t() {
+        COLIB_DEBUG_TRACE_SCOPE("this: %p", this);
+        if (!pool) { /* we where already handled by the pool */
+            COLIB_DEBUG_TRACE("already handled");
+            return ;
+        }
+        clear(0);
+        pool->get_internal()->rm_sem(this);
+    }
+
+    /* the pool died: nothing of it may be touched anymore */
+    void invalidate() { pool = nullptr; }
+    bool valid() const { return pool; }
 
     bool await_ready() {
         if (val > 0) {
@@ -4345,21 +4573,17 @@ struct sem_internal_t {
 
     error_e clear(int64_t val = 0) {
         COLIB_DEBUG_TRACE_SCOPE("sem_t::clear");
-        while (waiting_on_sem.size()) {
-            auto to_awake = waiting_on_sem.back();
-            waiting_on_sem.pop_back();
-            do_entry_modifs(to_awake.first);
-            do_unwait_sem_modifs(to_awake.first, selfptr);
-            do_leave_modifs(to_awake.first);
-            destroy_state(to_awake.first);
+        while (state_t *to_awake = waiting_on_sem.pop_front()) {
+            do_unwait_sem_modifs(to_awake, this);
+            destroy_state(to_awake);
         }
         this->val = val;
         return ERROR_OK;
     }
 
-    /* be carefull with this one */
-    void erase_waiter(sem_wait_list_it it) {
-        waiting_on_sem.erase(it);
+    void erase_waiter(state_t *waiter) {
+        if (waiting_on_sem.has(waiter))
+            waiting_on_sem.unlink(waiter);
     }
 
 protected:
@@ -4368,12 +4592,8 @@ protected:
     friend inline sem_p create_sem(pool_t *pool, int64_t val);
 
     sem_waiter_handle_p push_waiter(state_t *state) {
-        auto p = std::shared_ptr<sem_wait_list_it>(
-                alloc<sem_wait_list_it>(pool), dealloc_create<sem_wait_list_it>(pool),
-                allocator_t<int>{pool});
-        waiting_on_sem.push_front({state, p});
-        *p = waiting_on_sem.begin();
-        return p;
+        waiting_on_sem.push_back(state);
+        return state;
     }
 
 
@@ -4383,19 +4603,17 @@ protected:
 
 private:
     error_e _awake_one() {
-        auto to_awake = waiting_on_sem.back();
-        pool->get_internal()->push_ready(to_awake.first);
-        waiting_on_sem.pop_back();
+        pool->get_internal()->push_ready(waiting_on_sem.pop_front());
         return ERROR_OK;
     }
 
     pool_t *pool = nullptr;
     int64_t val;
-    sem_wait_list_t waiting_on_sem;
-    sem_t *selfptr = nullptr;
+    state_list_t waiting_on_sem;
 };
 
 inline error_e pool_internal_t::clear() {
+    run_posted();   /* the parked ones go back to their owners first */
     if (io_pool.clear() != ERROR_OK) {
         COLIB_DEBUG("WARNING: FAILED to clear events waiting for io");
     }
@@ -4414,10 +4632,8 @@ inline error_e pool_internal_t::clear()
         sem_pool.erase(s);
     }
     COLIB_DEBUG_TRACE("Cleaning wait queue");
-    for (auto &state : ready_tasks) {
+    while (state_t *state = ready_tasks.pop_front())
         destroy_state(state);
-    }
-    ready_tasks.clear();
     return ERROR_OK;
 }
 
@@ -4441,14 +4657,21 @@ struct sem_awaiter_t {
         state = &to_suspend.promise().state;
 
         auto pool = sem->get_internal()->get_pool();
-        psem_it = sem->get_internal()->push_waiter(state);
 
-        if (do_wait_sem_modifs(state, sem, psem_it) != ERROR_OK) {
+        do_leave_modifs(state);
+        psem_it = sem->get_internal()->push_waiter(state);
+        error_e err = do_wait_sem_modifs(state, sem, psem_it);
+        if (err != ERROR_OK) {
+            sem->get_internal()->erase_waiter(psem_it);
+            do_unwait_sem_modifs(state, sem);
+            if (err == ERROR_SUSPENDED) {
+                push_parked(state);
+                return std::noop_coroutine();   /* parked by a modif */
+            }
             COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
-            sem->get_internal()->erase_waiter(*psem_it);
+            do_entry_modifs(state);
             return to_suspend;
         }
-        do_leave_modifs(state);
 
         await_state = AWAITER_SUSPEND_LAST;
         return pool->get_internal()->next_task();
@@ -4457,8 +4680,8 @@ struct sem_awaiter_t {
     sem_t::unlocker_t await_resume() {
         COLIB_DEBUG_TRACE_SCOPE("sem-unwait state: %p", state);
         if (await_state == AWAITER_SUSPEND_LAST) {
-            do_entry_modifs(state);
             do_unwait_sem_modifs(state, sem);
+            do_entry_modifs(state);
             return sem_t::unlocker_t(sem);
         }
         else if (await_state == AWAITER_READY_LAST)
@@ -4478,46 +4701,34 @@ struct sem_awaiter_t {
 
     state_t *state = nullptr;
     sem_t *sem = nullptr;
-    sem_waiter_handle_p psem_it;
+    sem_waiter_handle_p psem_it = nullptr;
     await_state_e await_state = AWAITER_NOT_CALLED;
 };
 
-inline sem_t::sem_t(pool_t *pool, int64_t val)
-: internal(std::make_unique<sem_internal_t>(pool, val, this))
-{
-    get_internal()->pool->get_internal()->add_sem(this);
-}
-
-inline sem_t::~sem_t() {
-    COLIB_DEBUG_TRACE_SCOPE("this: %p", this);
-    if (!internal) { /* we where already handled by the pool */
-        COLIB_DEBUG_TRACE("already handled");
-        return ;
-    }
-    get_internal()->clear(0);
-    get_internal()->pool->get_internal()->rm_sem(this);
-}
-
 inline sem_awaiter_t sem_t::wait() { return sem_awaiter_t(this); }
 /* A semaphore that outlived its pool was invalidated by pool_t::clear() and has no internals left:
 it does nothing and says so, as signal()'s comment promises, instead of dereferencing them. Only
 wait() is left unguarded, since every coroutine that could wait died in the same clear().
 2026-09-23 04:52 */
 inline error_e sem_t::signal(int64_t inc) {
-    return internal ? internal->signal(inc) : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->signal(inc) : ERROR_GENERIC;
 }
 inline error_e sem_t::signal_all() {
-    return internal ? internal->signal_all() : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->signal_all() : ERROR_GENERIC;
 }
 inline bool sem_t::try_dec() {
-    return internal ? internal->try_dec() : false;
+    return get_internal()->valid() ? get_internal()->try_dec() : false;
 }
 inline error_e sem_t::clear(int64_t val) {
-    return internal ? internal->clear(val) : ERROR_GENERIC;
+    return get_internal()->valid() ? get_internal()->clear(val) : ERROR_GENERIC;
 }
 
 inline sem_internal_t *sem_t::get_internal() {
-    return internal.get();
+    return static_cast<sem_internal_t *>(this);
+}
+
+inline void sem_t::invalidate_self() {
+    get_internal()->invalidate();
 }
 
 inline sem_p create_sem(pool_t *pool, int64_t val) {
@@ -4525,7 +4736,7 @@ inline sem_p create_sem(pool_t *pool, in
 	because they may survive outside of the pool, and we would not have how
 	to de-allocate them anymore, so semaphores need to be allocated with the
 	global allocator */
-    return std::shared_ptr<sem_t>(new sem_t(pool, val));
+    return std::shared_ptr<sem_t>(new sem_internal_t(pool, val));
 }
 inline sem_p create_sem(pool_p pool, int64_t val) {
     return create_sem(pool.get(), val);
```

### 5.16 `force_stop`

The same outcomes as `yield`, and the park is linked the same way.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4591,23 +4802,37 @@ inline task_t force_stop(int64_t stopval
             COLIB_DEBUG_TRACE_SCOPE("force_stop state: %p", &h.promise().state);
 
             state = &h.promise().state;
+
             do_leave_modifs(state);
+            error_e err = do_yield_modifs(state);
+            if (err == ERROR_SUSPENDED) {
+                push_parked(state);
+                return std::noop_coroutine();   /* parked by a modif, the pool isn't stopped */
+            }
+            if (err != ERROR_OK) {
+                do_entry_modifs(state);
+                return h;   /* refused by a modif */
+            }
+
             state->pool->get_internal()->push_ready_front(state);
             state->pool->get_internal()->ret_val = RUN_STOPPED;
             state->pool->get_internal()->post_stop();
             state->pool->stopval = stopval;
+            triggered = true;
             return std::noop_coroutine();
         }
 
         error_e await_resume() {
             COLIB_DEBUG_TRACE_SCOPE("force_stop state: %p", state);
-            
-            do_entry_modifs(state);
+
+            if (triggered)
+                do_entry_modifs(state);
             return ERROR_OK; /* no errors to be had here */
         }
 
         int64_t stopval = 0;
         state_t *state = nullptr;
+        bool triggered = false;
     };
     /* this interrupts the pool, if you continue it, it will continue from this place */
     co_return (co_await stop_awaiter_t{stopval});
```

### 5.17 `create_timeo` and the killer

`create_timeo` has one decision point, `decided`/`timer_firing`, and the sleep-error path now signals too. The killer is `killer_state_t`, with `kill()` switching on the target's state (and asking the backend whether a woken io already did its work). It uses `take_turn`/`queue_caller` for the caller's turn, `drive` for resuming, `park` (it only marks, the pool calls back), and `unwind`. The modif pack has CALL, SCHED, EXIT, the WAITs, the UNWAITs and PARKED (which unwinds), with no ENTER/LEAVE.

```diff
--- a/colib.h
+++ b/colib.h
@@ -5822,6 +6047,8 @@ inline task<std::pair<T, error_e>> creat
         task<T> t;
         T ret;
         int id;
+        bool decided = false;       /* the result is set and sem signaled */
+        bool timer_firing = false;  /* timer_coro is inside timer_elapsed_sig() */
     };
 
     auto tstate = std::shared_ptr<timer_state_t>(alloc<timer_state_t>(pool),
@@ -5844,7 +6071,10 @@ inline task<std::pair<T, error_e>> creat
     auto exec_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
         COLIB_DEBUG_TRACE_SCOPE("Exec for tstate: %p", tstate.get());
         tstate->ret = co_await tstate->t;
-        tstate->timer_sig();
+        /* if the timer is firing, it is the one resuming us */
+        if (!tstate->timer_firing)
+            tstate->timer_sig();
+        tstate->decided = true;
         tstate->tstate_err = ERROR_OK;
         tstate->sem->signal();
         co_return ERROR_OK;
@@ -5852,18 +6082,22 @@ inline task<std::pair<T, error_e>> creat
 
     auto timer_coro = [](std::shared_ptr<timer_state_t> tstate) -> task_t {
         COLIB_DEBUG_TRACE_SCOPE("Waiting timer for tstate: %p", tstate.get());
-        error_e err;
-        if ((err = (error_e)co_await COLIB_REGNAME(sleep_us(tstate->duration))) != ERROR_OK) {
-            tstate->tstate_err = err;
-            tstate->timer_elapsed_sig();
+        error_e err = (error_e)co_await COLIB_REGNAME(sleep_us(tstate->duration));
+        if (err != ERROR_OK)
             COLIB_DEBUG_TRACE("Timer ERRORED OUT tstate[%p]", tstate.get());
-            co_return ERROR_GENERIC;
-        }
-        COLIB_DEBUG_TRACE("Timer EXPIRED tstate[%p]", tstate.get());
-        tstate->tstate_err = ERROR_TIMEO;
+        else
+            COLIB_DEBUG_TRACE("Timer EXPIRED tstate[%p]", tstate.get());
+
+        /* the task may finish inside the kill, then it already decided */
+        tstate->timer_firing = true;
         tstate->timer_elapsed_sig();
+        if (tstate->decided)
+            co_return ERROR_OK;
+
+        tstate->decided = true;
+        tstate->tstate_err = (err != ERROR_OK) ? err : ERROR_TIMEO;
         tstate->sem->signal();
-        co_return ERROR_OK;
+        co_return (err != ERROR_OK) ? ERROR_GENERIC : ERROR_OK;
     }(tstate);
 
     add_modifs(pool, exec_coro, timer_elapsed_killer);
@@ -5881,133 +6115,175 @@ inline task<std::pair<T, error_e>> creat
     return ret_coro;
 }
 
+/* One per create_killer() call, shared by its modifs and its kill function */
+struct killer_state_t {
+    killer_state_t(error_e e) : e(e) {}
+
+    error_e e;                          /* the err of a killed root that was called */
+
+    std::stack<state_t *> call_stack;   /* top() is the innermost frame */
+    io_desc_t *io_desc = nullptr;       /* the io the top waits on */
+    sem_t *sem = nullptr;               /* the semaphore the top waits on */
+
+    bool dying = false;     /* the next wait parks */
+    bool parked = false;    /* the top parked because of `dying` */
+    bool driving = false;   /* kill() is resuming the chain */
+    bool unwinding = false; /* kill() is destroying the chain */
+    bool finished = false;  /* the root exited while driven */
+    state_t *turn = nullptr;            /* the target's place in the ready queue, for its caller */
+
+    error_e kill();
+    error_e take_turn(state_t *top, bool effect);
+    void queue_caller(state_t *caller);
+    error_e drive();
+    void drop(state_t *top);
+    void unwind();
+    error_e park(state_t *s);
+};
+
+inline error_e killer_state_t::kill() {
+    /* called from our own unwind */
+    if (unwinding)
+        return ERROR_GENERIC;
+    if (call_stack.empty())
+        return ERROR_FINISHED;
+
+    state_t *top = call_stack.top();
+    switch (top->get_state()) {
+        case STATE_RUNNING:
+            /* executing, it dies at its next wait */
+            dying = true;
+            throw kill_deferred_t(top);
+
+        case STATE_READY:
+            /* woken, or never started: with an effect it runs up to its next wait */
+            return take_turn(top,
+                    (io_desc && top->pool->get_internal()->io_completed(*io_desc)) || sem);
+
+        case STATE_WAITING_IO: {
+            /* an io that completed meanwhile is delivered */
+            error_e ret = top->pool->get_internal()->stop_io(*io_desc, ERROR_WAKEUP);
+            if (ret != ERROR_OK && ret != ERROR_FINISHED) {
+                COLIB_DEBUG("WARNING: failed to stop the io of a killed coroutine: %s",
+                        dbg_enum(ret).c_str());
+            }
+            return take_turn(top, ret == ERROR_FINISHED);   /* stop_io queued it */
+        }
+
+        case STATE_WAITING_SEM:
+            /* no token was taken */
+            sem->get_internal()->erase_waiter(top);
+            drop(top);
+            return ERROR_OK;
+
+        case STATE_PARKED:
+            /* parked, the pool didn't call us back yet */
+        case STATE_LEFT:
+            /* on an external awaitable */
+            unwind();
+            return ERROR_OK;
+    }
+    return ERROR_GENERIC;
+}
+
+/* The target is in the ready queue: its place there is kept (a placeholder) for its caller */
+inline error_e killer_state_t::take_turn(state_t *top, bool effect) {
+    pool_internal_t *pool = top->pool->get_internal();
+    state_t placeholder;
+    pool->replace_ready(top, &placeholder);
+    turn = &placeholder;
+
+    error_e ret = ERROR_OK;
+    if (effect)
+        ret = drive();
+    else
+        drop(top);
+
+    if (turn)
+        pool->remove_ready(turn);   /* no caller took it */
+    turn = nullptr;
+    return ret;
+}
+
+/* the caller resumes in the target's place in the ready queue, if it had one */
+inline void killer_state_t::queue_caller(state_t *caller) {
+    pool_internal_t *pool = caller->pool->get_internal();
+    if (turn && pool->replace_ready(turn, caller)) {
+        turn = nullptr;
+        return;
+    }
+    pool->push_ready(caller);
+}
+
+inline error_e killer_state_t::drive() {
+    state_t *top = call_stack.top();
+
+    /* runs until it parks at its next wait or its root exits */
+    dying = true;
+    driving = true;
+    top->self.resume();
+    driving = false;
+
+    if (finished)
+        return ERROR_FINISHED;
+    unwind();
+    return ERROR_OK;
+}
+
+inline void killer_state_t::drop(state_t *top) {
+    /* replay the wake-up, so the modifs see the wait end */
+    if (io_desc)
+        do_unwait_io_modifs(top, *io_desc);
+    else if (sem)
+        do_unwait_sem_modifs(top, sem);
+    unwind();
+}
+
+inline void killer_state_t::unwind() {
+    /* innermost first, our EXIT cbk pops call_stack */
+    unwinding = true;
+    while (call_stack.size() > 1) {
+        state_t *s = call_stack.top();
+        do_exit_modifs(s);
+        s->self.destroy();
+    }
+    state_t *root = call_stack.top();
+    do_exit_modifs(root);
+    if (!root->caller_state) {
+        root->self.destroy();       /* scheduled, nobody waits for it */
+    }
+    else {
+        /* called, its caller destroys it */
+        root->err = e;
+        queue_caller(root->caller_state);
+    }
+    parked = false;
+    unwinding = false;
+}
+
+/* the answer of the wait cbks once the chain is dying: the pool calls our PARKED cbk after the
+resume, unless a drive (or another killer) destroys the chain first */
+inline error_e killer_state_t::park(state_t *s) {
+    (void)s;
+    parked = true;
+    return ERROR_SUSPENDED;
+}
+
 /* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
 /* this is inherited by-call and it must kill all coros in the call path and also stop all waiters
 (io and sem) */
 /* CAUTION: This will be similar to an exception thrown on the active await and the call stack */
 inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e) {
-    struct kill_state_t {
-        std::stack<state_t *> call_stack;
-        io_desc_t *io_desc = nullptr;
-        sem_t *sem = nullptr;
-        sem_waiter_handle_p it;
-        bool killing_activated = false;
-    };
-
-    /* TODO: fix, calling killer from killer (as a result of killing a coro) is not ok,
-    maybe have a way to guard, error out or something? We need to somehow warn the user
-    that he did that */
-
     /* Plain std::make_shared, not the pool's allocator: the killer and its modif pack are the
     user's to hold, and may be let go of after the pool is gone, which would then free the state -
     and the shared_ptr's own count - into a pool that no longer exists. Same reasoning as modif_t's
     own allocation (see 018-010); 018-014 is this one's regression test. 2026-09-23 05:06 */
     (void)pool;
-    auto kstate = std::make_shared<kill_state_t>();
+    auto kstate = std::make_shared<killer_state_t>(e);
 
     COLIB_DEBUG_TRACE("created killer: %p", kstate.get());
 
-    /* ! those std::functions will not be used using our allocator */
-    auto sig_kill = [kstate, e]() -> error_e {
-        COLIB_DEBUG_TRACE_SCOPE("kstate: %p", kstate.get());
-
-        /* This is here to catch the same killer being called twice which doesn't make sense */
-        if (kstate->killing_activated) {
-            COLIB_DEBUG("ERROR: sig_kill() called reentrantly while already unwinding");
-            return ERROR_GENERIC;
-        }
-        /* If there is no call stack we have nothing to awake */
-        if (kstate->call_stack.size() == 0) {
-            COLIB_DEBUG_TRACE("No stack...");
-            return ERROR_GENERIC;
-        }
-
-        kstate->killing_activated = true;
-
-        /* the top of the stack holds a pointer to the pool */
-        auto pool = kstate->call_stack.top()->pool;
-        COLIB_DEBUG_TRACE("pool: %p", pool);
-
-        auto state = kstate->call_stack.top();
-        COLIB_DEBUG_TRACE("top state: %p", state);
-
-        /* First we must check if we are in the ready queue, case in which we
-        must pop ourselves from there. */
-        if (pool->get_internal()->remove_ready(kstate->call_stack.top())) {
-            COLIB_DEBUG_TRACE("Head was in ready queue");
-            /* There is a case where we entered a wait state, io or sem and afterwards
-            we where pushed into the waiting queue. */
-        }
-        else if (kstate->sem) {
-            COLIB_DEBUG_TRACE("Head was waiting on semaphore state: %p sem: %p",
-                    state, kstate->sem);
-
-            /* This can happen in one situation: When the coroutine is destroyed and
-            the semaphore was still waited on. So we remove ourselves from the waiting queue on
-            the semaphore and do the modifications */
-            kstate->sem->get_internal()->erase_waiter(*kstate->it);
-        }
-        else if (kstate->io_desc) {
-            COLIB_DEBUG_TRACE("Head was waiting on io state: %p io-ptr: %p",
-                    state, kstate->io_desc);
-            /* This can happen in one situation: When the coroutine is destroyed and
-            the io was still waited on. */
-
-            /* First we stop the io, this will make the coroutine be placed in the ready queue with
-            the respective error set. */
-            pool->get_internal()->stop_io(*kstate->io_desc, ERROR_WAKEUP);
-
-            /* Now we pop ourselves from the ready queue */
-            bool the_problem = pool->get_internal()->remove_ready(state);
-            (void)the_problem;
-        }
-
-        /* Depending on the case, we may need to do the modifs of the respective waiter (sem/io) */
-        if (kstate->sem) {
-            do_entry_modifs(state);
-            do_unwait_sem_modifs(state, kstate->sem);
-            do_leave_modifs(state);
-            kstate->sem = nullptr;
-        }
-        if (kstate->io_desc) {
-            do_entry_modifs(state);
-            do_unwait_io_modifs(state, *kstate->io_desc);
-            do_leave_modifs(state);
-            kstate->io_desc = nullptr;
-        }
-
-        /* Now we unwind the call stack, removing all except the last one. The last one will be
-        removed by it's caller */
-        while (kstate->call_stack.size() > 1) {
-            COLIB_DEBUG_TRACE("Unwinding: %p", kstate->call_stack.top());
-            state = kstate->call_stack.top();
-            do_exit_modifs(state); /* OBS: the self(killer) exit modif will pop the stack */
-            state->self.destroy();
-        }
-
-        if (!kstate->call_stack.top()->caller_state) {
-            COLIB_DEBUG_TRACE("Origin was sched: %p", kstate->call_stack.top());
-            /* no one is really waiting for this coroutine to return: */
-            state = kstate->call_stack.top();
-            do_exit_modifs(state); /* OBS: the self(killer) exit modif will pop the stack */
-            state->self.destroy();
-        }
-        else {
-            COLIB_DEBUG_TRACE("Origin was call: %p with caller: %p",
-                    kstate->call_stack.top(), kstate->call_stack.top()->caller_state);
-            /* The exit modifs must be called before resuming the caller */
-            state = kstate->call_stack.top();
-            do_exit_modifs(state); /* OBS: the self(killer) exit modif will pop the stack */
-            /* Finally we prepare the root of the trace and schedule it's caller */
-            state->err = e;
-            pool->get_internal()->push_ready(state->caller_state);
-        }
-
-        return ERROR_OK;
-    };
-
     modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
-
     modif_pack_t pack;
     pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
         [kstate](state_t *s) -> error_e {
@@ -6026,18 +6302,25 @@ inline std::pair<modif_pack_t, std::func
     ));
     pack.push_back(create_modif<CO_MODIF_EXIT_CBK>(flags,
         [kstate](state_t *s) -> error_e {
-            (void)s;
             COLIB_DEBUG_TRACE("EXIT[%p]: tracking killer: %p", kstate.get(), s);
             COLIB_ENABLE_DEBUG_CHECK_ASSERT(kstate->call_stack.size(), "bad-pop");
             kstate->call_stack.pop();
+            if (kstate->driving && kstate->call_stack.empty()) {
+                /* the root exited while driven, its caller is queued, not resumed */
+                kstate->finished = true;
+                if (s->caller_state)
+                    kstate->queue_caller(s->caller_state);
+                return ERROR_SUSPENDED;
+            }
             return ERROR_OK;
         }
     ));
     pack.push_back(create_modif<CO_MODIF_WAIT_IO_CBK>(flags,
         [kstate](state_t *s, io_desc_t &io_desc) -> error_e {
-            (void)s;
             COLIB_DEBUG_TRACE("WAIT_IO[%p]: tracking killer: %p io-ptr: %p",
                     kstate.get(), s, &io_desc);
+            if (kstate->dying)
+                return kstate->park(s);
             kstate->io_desc = &io_desc;
             return ERROR_OK;
         }
@@ -6054,11 +6337,11 @@ inline std::pair<modif_pack_t, std::func
     ));
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
         [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
-            (void)s;
             COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p it-ptr: %p",
-                    kstate.get(), s, sem, it.get());
+                    kstate.get(), s, sem, it);
+            if (kstate->dying)
+                return kstate->park(s);
             kstate->sem = sem;
-            kstate->it = it;
             return ERROR_OK;
         }
     ));
@@ -6069,14 +6352,26 @@ inline std::pair<modif_pack_t, std::func
             COLIB_DEBUG_TRACE("UNWAIT_SEM[%p]: tracking killer: %p sem: %p",
                     kstate.get(), s, sem);
             kstate->sem = nullptr;
-            /* The waiter handle is pool-allocated (push_waiter()), so it is let go of with the
-            wait, not held until the killer dies, which may be after the pool. 2026-09-23 05:06 */
-            kstate->it = nullptr;
+            return ERROR_OK;
+        }
+    ));
+    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags,
+        [kstate](state_t *s) -> error_e {
+            if (kstate->dying)
+                return kstate->park(s);
+            return ERROR_OK;
+        }
+    ));
+    pack.push_back(create_modif<CO_MODIF_PARKED_CBK>(flags,
+        [kstate](state_t *s) -> error_e {
+            (void)s;    /* may be destroyed by an earlier PARKED cbk, only our state is read */
+            if (kstate->parked && !kstate->unwinding && !kstate->call_stack.empty())
+                kstate->unwind();
             return ERROR_OK;
         }
     ));
 
-    return {pack, sig_kill};
+    return {pack, [kstate]() -> error_e { return kstate->kill(); }};
 }
 
 /* Debug stuff
```

### 5.18 Debug

`dbg_enum(error_e)` realigned. `~state_t()` unlinks. The checks: LEAVE/ENTER refuse to run while waiting, a WAIT/UNWAIT requires `left`, and there's a new WAIT_YIELD check.

```diff
--- a/colib.h
+++ b/colib.h
@@ -6106,14 +6401,16 @@ inline dbg_string_t dbg_name(handle<P> h
 
 inline dbg_string_t dbg_enum(error_e code) {
     switch (code) {
-        case ERROR_YIELDED: return dbg_string_t{"ERROR_YIELDED",   allocator_t<char>{nullptr}};
-        case ERROR_OK:      return dbg_string_t{"ERROR_OK",        allocator_t<char>{nullptr}};
-        case ERROR_GENERIC: return dbg_string_t{"ERROR_GENERIC",   allocator_t<char>{nullptr}};
-        case ERROR_TIMEO:   return dbg_string_t{"ERROR_TIMEO",     allocator_t<char>{nullptr}};
-        case ERROR_WAKEUP:  return dbg_string_t{"ERROR_WAKEUP",    allocator_t<char>{nullptr}};
-        case ERROR_USER:    return dbg_string_t{"ERROR_USER",      allocator_t<char>{nullptr}};
-        case ERROR_DEPEND:  return dbg_string_t{"ERROR_DEPEND",    allocator_t<char>{nullptr}};
-        default:            return dbg_string_t{"[ERROR_UNKNOWN]", allocator_t<char>{nullptr}};
+        case ERROR_FINISHED:  return dbg_string_t{"ERROR_FINISHED",  allocator_t<char>{nullptr}};
+        case ERROR_SUSPENDED: return dbg_string_t{"ERROR_SUSPENDED", allocator_t<char>{nullptr}};
+        case ERROR_YIELDED:   return dbg_string_t{"ERROR_YIELDED",   allocator_t<char>{nullptr}};
+        case ERROR_OK:        return dbg_string_t{"ERROR_OK",        allocator_t<char>{nullptr}};
+        case ERROR_GENERIC:   return dbg_string_t{"ERROR_GENERIC",   allocator_t<char>{nullptr}};
+        case ERROR_TIMEO:     return dbg_string_t{"ERROR_TIMEO",     allocator_t<char>{nullptr}};
+        case ERROR_WAKEUP:    return dbg_string_t{"ERROR_WAKEUP",    allocator_t<char>{nullptr}};
+        case ERROR_USER:      return dbg_string_t{"ERROR_USER",      allocator_t<char>{nullptr}};
+        case ERROR_DEPEND:    return dbg_string_t{"ERROR_DEPEND",    allocator_t<char>{nullptr}};
+        default:              return dbg_string_t{"[ERROR_UNKNOWN]", allocator_t<char>{nullptr}};
     }
 }
 
@@ -6296,13 +6593,16 @@ inline dbg_string_t dbg_name(void *v) {
 
 inline state_t::~state_t() {
     COLIB_DEBUG_TRACE("ENDING STATE: %p", this);
+    state_access_t::unlink(this);
     if (this->self)
         dbg_names.erase(this->self.address());
 }
 
 #else /* COLIB_ENABLE_DEBUG_NAMES */
 
-inline state_t::~state_t() {}
+inline state_t::~state_t() {
+    state_access_t::unlink(this);
+}
 
 template <typename ...Args>
 inline void *dbg_register_name(void *addr, const char *, Args&&... args) { return addr; }
@@ -6434,6 +6734,8 @@ inline void dbg_check_modif_leave(state_
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but left");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but left");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but left");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "left while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "left while waiting on sem");
     dbg_state.left = true;
 }
 
@@ -6444,6 +6746,8 @@ inline void dbg_check_modif_enter(state_
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but entered");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered&& !dbg_state.left), "entered twice");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "entered while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "entered while waiting on sem");
     dbg_state.entered = true;
     dbg_state.left = false;
 }
@@ -6455,7 +6759,7 @@ inline void dbg_check_modif_wait_io(stat
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but io");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
@@ -6469,7 +6773,7 @@ inline void dbg_check_modif_unwait_io(st
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but un-io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but un-io");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but un-io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-io before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "un-waited io while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "un-waited io while waiting on sem (it)");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.io, "un-waited io while not waiting on io");
@@ -6485,12 +6789,12 @@ inline void dbg_check_modif_wait_sem(sta
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but sem");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "waited while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "waited while waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem_it, "waited while waiting on sem (it)");
     dbg_state.sem = sem;
-    dbg_state.sem_it = it.get();
+    dbg_state.sem_it = it;
 }
 
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem) {
@@ -6501,7 +6805,7 @@ inline void dbg_check_modif_unwait_sem(s
     auto &dbg_state = pool_map[s];
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but un-sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but un-sem");
-    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but un-sem");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "un-sem before leave");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "un-waited sem while waiting on io");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem, "un-waited sem while not waiting on sem");
     COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.sem_it, "un-waited sem while not waiting on sem (it)");
@@ -6510,6 +6814,18 @@ inline void dbg_check_modif_unwait_sem(s
     dbg_state.sem_it = nullptr;
 }
 
+inline void dbg_check_modif_wait_yield(state_t *s) {
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s, "no-state");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s->pool, "no-pool");
+    auto &pool_map = dbg_check_coro_states[s->pool];
+    auto &dbg_state = pool_map[s];
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.left, "yield before leave");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "yield while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "yield while waiting on sem");
+}
+
 
 #endif /* COLIB_ENABLE_DEBUG_CHECKS */
 
```

---

## 6. The tests

- **New:**
  - `002-003` (the target finishes inside the kill);
  - `002-004` (a self-kill is deferred);
  - `002-005` (the caller resumes in the child's turn);
  - `002-006` (a driven root that `co_yield`s);
  - `002-007` (a parked chain destroyed by another killer before the pool calls back);
  - `011-006` (`ERROR_SUSPENDED`, the PARKED callback, and WAIT_YIELD);
  - `011-007` (the order of the callbacks);
  - `012-002` (`get_state()`, and a queued coroutine destroyed);
  - `018-015` and `018-016` (the two reproduced bugs; see the landing order in section 4).
- **Changed:**
  - `002-002` and `018-005`: a kill of a finished target returns `ERROR_FINISHED`;
  - `011-003`: the new callback order.

```diff
--- a/tests/002-002-flowctrl_create_killer.cpp
+++ b/tests/002-002-flowctrl_create_killer.cpp
@@ -46,7 +46,7 @@
     ASSERT_FN(CHK_BOOL(test19_destruct_cnt == 1)); /* victim's stack was force-destroyed, not resumed */
 
     /* nothing left to kill: colib.h's sig_kill() reports this when the call stack is empty */
-    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_GENERIC));
+    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
 
     return 0;
 }
--- /dev/null
+++ b/tests/002-003-flowctrl_killer_finished.cpp
@@ -0,0 +1,53 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test48 - Flow Control: a killed chain that finishes before it can die
+================================================================================================= */
+
+/* The target's wait completed (a semaphore gave it its token), and after it the chain never waits
+again: the killer resumes it inside the kill, the chain returns all the way up to its root and the
+kill reports ERROR_FINISHED - it wasn't killed, it completed in time, and its results exist. */
+
+static int test48_out = 0;
+static bool test48_ok = false;
+
+static co::task<int> test48_inner(co::sem_p sem) {
+    co_await sem->wait();
+    co_return 7;
+}
+
+static co::task_t test48_outer(co::sem_p sem) {
+    test48_out = co_await test48_inner(sem);    /* a 2-frame chain: inner returns into outer */
+    co_return 0;
+}
+
+static co::task_t test48_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
+    co_await co::yield();       /* outer/inner run, inner waits on `sem` */
+    sem->signal();
+    co::error_e ret = kill_fn();
+    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_FINISHED));
+    ASSERT_COFN(CHK_BOOL(test48_out == 7));     /* it ran to the end, inside the kill */
+    ASSERT_COFN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
+    test48_ok = true;
+    co_return 0;
+}
+
+int test48_killer_finished() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+
+    pool->sched(co::add_modifs(pool.get(), test48_outer(sem), mods));
+    pool->sched(test48_controller(sem, kill_fn));
+    ASSERT_FN(pool->run());
+    ASSERT_FN(CHK_BOOL(test48_ok));
+    return 0;
+}
+
+int main() {
+    int ret = test48_killer_finished();
+    print_test_result("002-003-flowctrl_killer_finished.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/002-004-flowctrl_killer_self_kill.cpp
@@ -0,0 +1,79 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test49 - Flow Control: a coroutine that kills itself
+================================================================================================= */
+
+/* The kill function can't keep its promise for an executing target: it throws kill_deferred_t
+and the target dies at its next wait. Caught: the zero-time code until that wait still runs, the
+wait doesn't. Not caught: the exception unwinds the coroutine itself, and as for any exception
+leaving a scheduled root, pool_t::run() rethrows it. */
+
+static int test49_zero_time = 0;
+static int test49_after_wait = 0;
+static int test49_destructed = 0;
+static bool test49_thrown = false;
+
+struct test49_marker_t {
+    ~test49_marker_t() { test49_destructed++; }
+};
+
+static co::task_t test49_caught(std::function<co::error_e(void)> kill_fn, co::sem_p never) {
+    test49_marker_t marker;
+    try {
+        kill_fn();
+    }
+    catch (co::kill_deferred_t &) {
+        test49_thrown = true;
+    }
+    test49_zero_time++;         /* runs: code until the next wait takes zero time */
+    co_await never->wait();     /* dies here, without waiting */
+    test49_after_wait++;        /* must not run */
+    co_return 0;
+}
+
+static co::task_t test49_uncaught(std::function<co::error_e(void)> kill_fn) {
+    test49_marker_t marker;
+    kill_fn();                  /* throws out of this coroutine */
+    test49_after_wait++;        /* must not run */
+    co_return 0;
+}
+
+int test49_self_kill() {
+    {
+        auto pool = co::create_pool();
+        auto never = co::create_sem(pool, 0);
+        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+        pool->sched(co::add_modifs(pool.get(), test49_caught(kill_fn, never), mods));
+        ASSERT_FN(pool->run());
+        ASSERT_FN(CHK_BOOL(test49_thrown));
+        ASSERT_FN(CHK_BOOL(test49_zero_time == 1));
+        ASSERT_FN(CHK_BOOL(test49_after_wait == 0));
+        ASSERT_FN(CHK_BOOL(test49_destructed == 1));
+        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
+    }
+    {
+        auto pool = co::create_pool();
+        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+        pool->sched(co::add_modifs(pool.get(), test49_uncaught(kill_fn), mods));
+        bool escaped = false;
+        try {
+            pool->run();
+        }
+        catch (co::kill_deferred_t &) {
+            escaped = true;
+        }
+        ASSERT_FN(CHK_BOOL(escaped));
+        ASSERT_FN(CHK_BOOL(test49_after_wait == 0));
+        ASSERT_FN(CHK_BOOL(test49_destructed == 2));
+    }
+    return 0;
+}
+
+int main() {
+    int ret = test49_self_kill();
+    print_test_result("002-004-flowctrl_killer_self_kill.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/002-005-flowctrl_killer_caller_turn.cpp
@@ -0,0 +1,96 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+#define COLIB_ENABLE_DEBUG_CHECKS true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test52 - Flow Control: a killed child's caller resumes in the child's turn
+================================================================================================= */
+
+/* The child (the killer's target, called by the parent) is in the ready queue when the kill comes,
+so it is resumed inside the kill. Whether it then finishes or dies at its next wait, the parent
+must not run from inside the kill: it resumes in the child's place in the ready queue, before a
+witness queued behind the child. The third case removes a task queued *ahead* of the child while
+the child is resumed, the parent still keeps the child's place (a saved index would put it after
+the witness). */
+
+enum test52_mode_e { TEST52_FINISHES, TEST52_DIES, TEST52_REMOVES_AHEAD };
+
+static std::vector<std::string> test52_log;
+static std::function<co::error_e(void)> test52_kill_ahead;
+
+static co::task<int> test52_child(co::sem_p sem, co::sem_p never, test52_mode_e mode) {
+    co_await sem->wait();
+    if (mode == TEST52_REMOVES_AHEAD)
+        test52_kill_ahead();        /* the task queued ahead of us leaves the ready queue */
+    if (mode == TEST52_DIES)
+        co_await never->wait();     /* dies here */
+    co_return 7;
+}
+
+static co::task_t test52_parent(co::sem_p sem, co::sem_p never, test52_mode_e mode,
+        co::modif_pack_t mods)
+{
+    auto pool = co_await co::get_pool();
+    auto child = test52_child(sem, never, mode);
+    co::add_modifs(pool, child, mods);
+    int ret = co_await child;
+    test52_log.push_back("parent " + std::to_string(ret));
+    co_return 0;
+}
+
+static co::task_t test52_named(const char *name) {
+    test52_log.push_back(name);
+    co_return 0;
+}
+
+static co::task_t test52_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn,
+        co::modif_pack_t ahead_mods)
+{
+    auto pool = co_await co::get_pool();
+    co_await co::yield();                                   /* the child waits on sem */
+    pool->sched(co::add_modifs(pool, test52_named("ahead"), ahead_mods));
+    sem->signal();                                          /* the child is queued after it */
+    pool->sched(test52_named("witness"));                   /* and the witness after the child */
+    kill_fn();
+    test52_log.push_back("kill returned");
+    co_return 0;
+}
+
+static int test52_run(test52_mode_e mode, const std::vector<std::string> &expected) {
+    test52_log.clear();
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto never = co::create_sem(pool, 0);
+    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+    auto [ahead_mods, kill_ahead] = co::create_killer(pool.get(), co::ERROR_USER);
+    test52_kill_ahead = kill_ahead;
+
+    pool->sched(test52_parent(sem, never, mode, mods));
+    pool->sched(test52_controller(sem, kill_fn, ahead_mods));
+    ASSERT_FN(pool->run());
+
+    for (auto &l : test52_log)
+        DBG("mode %d: %s", (int)mode, l.c_str());
+    ASSERT_FN(CHK_BOOL(test52_log == expected));
+    return 0;
+}
+
+int test52_caller_turn() {
+    /* finishes inside the kill: the parent gets 7, at the child's turn */
+    ASSERT_FN(test52_run(TEST52_FINISHES,
+            {"kill returned", "ahead", "parent 7", "witness"}));
+    /* dies at its next wait: the parent gets no value (0), at the child's turn */
+    ASSERT_FN(test52_run(TEST52_DIES,
+            {"kill returned", "ahead", "parent 0", "witness"}));
+    /* the task ahead is removed while the child runs: the parent still comes before the witness */
+    ASSERT_FN(test52_run(TEST52_REMOVES_AHEAD,
+            {"kill returned", "parent 7", "witness"}));
+    return 0;
+}
+
+int main() {
+    int ret = test52_caller_turn();
+    print_test_result("002-005-flowctrl_killer_caller_turn.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/002-006-flowctrl_killer_driven_yield.cpp
@@ -0,0 +1,60 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test55 - Flow Control: a scheduled root that co_yields while a kill resumes it
+================================================================================================= */
+
+/* The target's semaphore wait already gave it its token, so the kill resumes it up to its next
+wait. It co_yields instead: a root without a caller, so cpp_yield_awaiter would hand control to
+the next ready task - from inside the kill. Nothing but the target may run inside a kill: the
+witness queued before the kill must run after it returned. */
+
+static bool test55_witness_ran = false;
+static bool test55_witness_ran_inside_kill = false;
+static co::error_e test55_kill_ret = co::ERROR_OK;
+static bool test55_ok = false;
+
+static co::task_t test55_target(co::sem_p sem) {
+    co_await sem->wait();
+    co_yield 5;                 /* a scheduled root, no caller */
+    co_return 0;
+}
+
+static co::task_t test55_witness() {
+    test55_witness_ran = true;
+    co_return 0;
+}
+
+static co::task_t test55_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
+    auto pool = co_await co::get_pool();
+    co_await co::yield();       /* the target waits on sem */
+    sem->signal();              /* it takes the token and is queued */
+    pool->sched(test55_witness());
+    test55_kill_ret = kill_fn();
+    test55_witness_ran_inside_kill = test55_witness_ran;
+    test55_ok = true;
+    co_return 0;
+}
+
+int test55_driven_yield() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+    pool->sched(co::add_modifs(pool.get(), test55_target(sem), mods));
+    pool->sched(test55_controller(sem, kill_fn));
+    ASSERT_FN(pool->run());
+
+    ASSERT_FN(CHK_BOOL(test55_ok));
+    ASSERT_FN(CHK_BOOL(test55_kill_ret == co::ERROR_FINISHED));    /* it yielded, not killed */
+    ASSERT_FN(CHK_BOOL(!test55_witness_ran_inside_kill));
+    ASSERT_FN(CHK_BOOL(test55_witness_ran));
+    return 0;
+}
+
+int main() {
+    int ret = test55_driven_yield();
+    print_test_result("002-006-flowctrl_killer_driven_yield.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/002-007-flowctrl_killer_parked_unwound.cpp
@@ -0,0 +1,78 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+#define COLIB_ENABLE_DEBUG_CHECKS true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test56 - Flow Control: a parked chain destroyed before the pool calls its owner back
+================================================================================================= */
+
+/* The target carries two killers. The second one's kill resumes it (its semaphore wait already had
+its token); while resumed, the target kills itself through the first killer (deferred: it catches
+kill_deferred_t) and reaches its next wait, where both killers park it. The pool would call the
+PARKED callbacks after the resume, but the second killer destroys the chain first, right after its
+own resume returns. The parked state must leave the pool's parked list with its frame: no PARKED
+callback on a freed state, one destruction, and the first killer finds nothing left to kill. */
+
+static std::function<co::error_e(void)> test56_kill_first;
+static int test56_destructed = 0;
+static int test56_after_wait = 0;
+static bool test56_deferred = false;
+static bool test56_ok = false;
+
+struct test56_marker_t {
+    ~test56_marker_t() { test56_destructed++; }
+};
+
+static co::task_t test56_target(co::sem_p sem, co::sem_p never) {
+    test56_marker_t marker;
+    co_await sem->wait();               /* the token is given: the second kill resumes us */
+    try {
+        test56_kill_first();            /* we are executing: deferred to our next wait */
+    }
+    catch (co::kill_deferred_t &) {
+        test56_deferred = true;
+    }
+    co_await never->wait();             /* both killers park us here */
+    test56_after_wait++;
+    co_return 0;
+}
+
+static co::task_t test56_controller(co::sem_p sem, std::function<co::error_e(void)> kill_second) {
+    co_await co::yield();               /* the target waits on sem */
+    sem->signal();
+    co::error_e ret = kill_second();    /* resumes it, it parks, the chain is destroyed */
+    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
+    ASSERT_COFN(CHK_BOOL(test56_deferred));
+    ASSERT_COFN(CHK_BOOL(test56_destructed == 1));
+    ASSERT_COFN(CHK_BOOL(test56_kill_first() == co::ERROR_FINISHED));
+    test56_ok = true;
+    co_return 0;
+}
+
+int test56_parked_unwound() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto never = co::create_sem(pool, 0);
+    auto [first_mods, kill_first] = co::create_killer(pool.get(), co::ERROR_USER);
+    auto [second_mods, kill_second] = co::create_killer(pool.get(), co::ERROR_USER);
+    test56_kill_first = kill_first;
+
+    auto target = test56_target(sem, never);
+    co::add_modifs(pool.get(), target, first_mods);
+    co::add_modifs(pool.get(), target, second_mods);
+    pool->sched(target);
+    pool->sched(test56_controller(sem, kill_second));
+    ASSERT_FN(pool->run());
+
+    ASSERT_FN(CHK_BOOL(test56_ok));
+    ASSERT_FN(CHK_BOOL(test56_after_wait == 0));
+    ASSERT_FN(CHK_BOOL(test56_destructed == 1));
+    return 0;
+}
+
+int main() {
+    int ret = test56_parked_unwound();
+    print_test_result("002-007-flowctrl_killer_parked_unwound.cpp", ret >= 0);
+    return ret;
+}
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
--- /dev/null
+++ b/tests/011-006-modifs_force_suspend.cpp
@@ -0,0 +1,93 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+#define COLIB_ENABLE_DEBUG_CHECKS true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test50 - Modifs: ERROR_SUSPENDED and CO_MODIF_WAIT_YIELD_CBK
+================================================================================================= */
+
+/* A WAIT_SEM callback answering ERROR_SUSPENDED parks the coroutine: it has left (LEAVE), the
+wait isn't registered (no token can reach it) and the wait callbacks are unwound (UNWAIT_SEM). Once
+the resume that parked it returned, the pool calls its owner back (PARKED); nothing else runs until
+the owner destroys it. A WAIT_YIELD callback returning an
+error refuses the yield: the coroutine continues right away, and doesn't get a second ENTER (the
+debug checks would abort). */
+
+static std::string test50_order;
+static co::state_t *test50_parked = nullptr;
+static int test50_after_wait = 0;
+static int test50_destructed = 0;
+static int test50_after_yield = 0;
+
+struct test50_marker_t {
+    ~test50_marker_t() { test50_destructed++; }
+};
+
+static co::task_t test50_parked_task(co::sem_p sem) {
+    test50_marker_t marker;
+    co_await sem->wait();
+    test50_after_wait++;        /* must not run */
+    co_return 0;
+}
+
+static co::task_t test50_owner(co::sem_p sem) {
+    co_await co::yield();       /* the parked task runs and parks */
+    sem->signal();              /* no waiter: the count goes to 1, nobody is woken */
+    co_await co::yield();
+    ASSERT_COFN(CHK_BOOL(test50_parked != nullptr));
+    ASSERT_COFN(CHK_BOOL(test50_after_wait == 0));
+    co::destroy_state(test50_parked);   /* the owner ends it */
+    ASSERT_COFN(CHK_BOOL(test50_destructed == 1));
+    ASSERT_COFN(CHK_BOOL(sem->try_dec()));  /* the token stayed in the semaphore */
+    co_return 0;
+}
+
+static co::task_t test50_refused_yield() {
+    co_await co::yield();       /* refused: continues right away */
+    test50_after_yield++;
+    co_return 0;
+}
+
+int test50_force_suspend() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto flags = co::CO_MODIF_INHERIT_NONE;
+
+    co::modif_pack_t pack;
+    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
+        [](co::state_t *s, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
+            test50_order += "W";
+            test50_parked = s;
+            return co::ERROR_SUSPENDED;
+        }));
+    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
+        [](co::state_t *, co::sem_t *) -> co::error_e { test50_order += "U"; return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
+        [](co::state_t *) -> co::error_e { test50_order += "L"; return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
+        [](co::state_t *) -> co::error_e { test50_order += "E"; return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_PARKED_CBK>(flags,
+        [](co::state_t *) -> co::error_e { test50_order += "P"; return co::ERROR_OK; }));
+
+    auto refuse = co::modif_pack_t(1, co::create_modif<co::CO_MODIF_WAIT_YIELD_CBK>(flags,
+        [](co::state_t *) -> co::error_e { return co::ERROR_USER; }));
+
+    pool->sched(co::add_modifs(pool.get(), test50_parked_task(sem), pack));
+    pool->sched(test50_owner(sem));
+    pool->sched(co::add_modifs(pool.get(), test50_refused_yield(), refuse));
+    ASSERT_FN(pool->run());
+
+    /* ENTER at its first run, then the park: LEAVE, WAIT_SEM, UNWAIT_SEM, and once the resume
+    returned, PARKED (its owner is called back) - no ENTER, it never resumed */
+    DBG("order: %s", test50_order.c_str());
+    ASSERT_FN(CHK_BOOL(test50_order == "ELWUP"));
+    ASSERT_FN(CHK_BOOL(test50_after_yield == 1));
+    return 0;
+}
+
+int main() {
+    int ret = test50_force_suspend();
+    print_test_result("011-006-modifs_force_suspend.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/011-007-modifs_close_order.cpp
@@ -0,0 +1,66 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+#define COLIB_ENABLE_DEBUG_CHECKS true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test51 - Modifs: closing callbacks run in reverse order
+================================================================================================= */
+
+/* Two modifs, `a` added before `b`, on one coroutine. The opening callbacks (ENTER, WAIT_SEM) run
+in the order the modifs were added, the closing ones (LEAVE, UNWAIT_SEM, EXIT) in reverse, so the
+pairs nest like constructors and destructors: a opens, b opens, b closes, a closes. */
+
+static std::string test51_order;
+
+static co::modif_pack_t test51_pack(char id) {
+    auto flags = co::CO_MODIF_INHERIT_NONE;
+    auto log = [id](char ev) { test51_order += ev; test51_order += id; test51_order += ' '; };
+    co::modif_pack_t pack;
+    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
+        [log](co::state_t *) -> co::error_e { log('E'); return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
+        [log](co::state_t *) -> co::error_e { log('L'); return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
+        [log](co::state_t *, co::sem_t *, co::sem_waiter_handle_p) -> co::error_e {
+            log('W'); return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
+        [log](co::state_t *, co::sem_t *) -> co::error_e { log('U'); return co::ERROR_OK; }));
+    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(flags,
+        [log](co::state_t *) -> co::error_e { log('X'); return co::ERROR_OK; }));
+    return pack;
+}
+
+static co::task_t test51_waiter(co::sem_p sem) {
+    co_await sem->wait();
+    co_return 0;
+}
+
+static co::task_t test51_signaler(co::sem_p sem) {
+    sem->signal();
+    co_return 0;
+}
+
+int test51_close_order() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+
+    auto t = test51_waiter(sem);
+    co::add_modifs(pool.get(), t, test51_pack('a'));
+    co::add_modifs(pool.get(), t, test51_pack('b'));
+    pool->sched(t);
+    pool->sched(test51_signaler(sem));
+    ASSERT_FN(pool->run());
+
+    /* sched's ENTER; the wait: LEAVE, WAIT; the resume: UNWAIT, ENTER; the return: LEAVE, EXIT */
+    DBG("order: %s", test51_order.c_str());
+    ASSERT_FN(CHK_BOOL(test51_order ==
+            "Ea Eb Lb La Wa Wb Ub Ua Ea Eb Lb La Xb Xa "));
+    return 0;
+}
+
+int main() {
+    int ret = test51_close_order();
+    print_test_result("011-007-modifs_close_order.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/012-002-introspection_state.cpp
@@ -0,0 +1,98 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test54 - Coroutine introspection: state_t::get_state()
+================================================================================================= */
+
+/* A coroutine's state follows it: READY while queued, RUNNING while it runs, WAITING_SEM on a
+semaphore, LEFT while a callee runs - co::sleep_ms() is a callee too, so a sleeping coroutine is
+LEFT, and the innermost frame (co::sleep's) is the one WAITING_IO. And a coroutine destroyed while
+it is queued leaves the queue by itself: the pool never resumes the freed frame. */
+
+static std::vector<std::string> test54_log;
+static co::state_t *test54_watched = nullptr;
+
+static const char *test54_name(co::state_e s) {
+    switch (s) {
+        case co::STATE_LEFT:        return "LEFT";
+        case co::STATE_RUNNING:     return "RUNNING";
+        case co::STATE_READY:       return "READY";
+        case co::STATE_WAITING_SEM: return "WAITING_SEM";
+        case co::STATE_WAITING_IO:  return "WAITING_IO";
+        case co::STATE_PARKED:      return "PARKED";
+        case co::STATE_DONE:        return "DONE";
+    }
+    return "?";
+}
+
+static void test54_look(const char *when) {
+    test54_log.push_back(std::string(when) + ": " + test54_name(test54_watched->get_state()));
+}
+
+static co::task_t test54_callee(co::sem_p sem) {
+    co_await co::yield();               /* the caller is LEFT meanwhile */
+    co_return 0;
+}
+
+static co::task_t test54_watched_task(co::sem_p sem) {
+    test54_watched = co_await co::get_state();
+    test54_look("self");
+    co_await sem->wait();
+    co_await co::sleep_ms(1);
+    co_await test54_callee(sem);
+    co_return 0;
+}
+
+static co::task_t test54_observer(co::sem_p sem) {
+    test54_look("after its start");     /* it waits on sem */
+    sem->signal();
+    test54_look("signaled");            /* queued */
+    co_await co::yield();               /* it runs and sleeps */
+    test54_look("sleeping");            /* LEFT: it calls co::sleep_ms() */
+    co_await co::sleep_ms(5);           /* it calls the callee, which yields */
+    test54_look("calling");
+    co_return 0;
+}
+
+static bool test54_victim_ran = false;
+
+static co::task_t test54_victim() {
+    test54_victim_ran = true;
+    co_return 0;
+}
+
+int test54_states() {
+    {
+        auto pool = co::create_pool();
+        auto sem = co::create_sem(pool, 0);
+        pool->sched(test54_watched_task(sem));
+        pool->sched(test54_observer(sem));
+        ASSERT_FN(pool->run());
+
+        for (auto &l : test54_log)
+            DBG("%s", l.c_str());
+        ASSERT_FN(CHK_BOOL(test54_log == std::vector<std::string>({
+                "self: RUNNING", "after its start: WAITING_SEM", "signaled: READY",
+                "sleeping: LEFT", "calling: LEFT"})));
+    }
+    {
+        /* destroyed while queued: it leaves the queue, the pool never resumes it */
+        auto pool = co::create_pool();
+        auto victim = test54_victim();
+        pool->sched(victim);
+        co::state_t *s = &victim.h.promise().state;
+        ASSERT_FN(CHK_BOOL(s->get_state() == co::STATE_READY));
+        co::destroy_state(s);
+        ASSERT_FN(pool->run());
+        ASSERT_FN(CHK_BOOL(!test54_victim_ran));
+    }
+    return 0;
+}
+
+int main() {
+    int ret = test54_states();
+    print_test_result("012-002-introspection_state.cpp", ret >= 0);
+    return ret;
+}
--- a/tests/018-005-reproduced_killer_after_completion.cpp
+++ b/tests/018-005-reproduced_killer_after_completion.cpp
@@ -35,7 +35,7 @@
 
     /* nothing left to kill: must return cleanly, not crash */
     co::error_e ret = kill_fn();
-    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_GENERIC));
+    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_FINISHED));
 
     co_return 0;
 }
--- /dev/null
+++ b/tests/018-015-reproduced_timeo_read_drops_bytes.cpp
@@ -0,0 +1,114 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test46 - Reproduced Bugs: create_timeo(co::read(...)) drops bytes that were already read
+================================================================================================= */
+
+/* On IOCP co::read issues an overlapped ReadFile before it waits, so the kernel can complete it
+(moving the bytes out of the socket, into the buffer) before the coroutine is resumed. When
+create_timeo's timer fires in that window, its killer either destroys the already-queued reader
+(the completion is dequeued, not yet resumed) or cancels a request that already finished and
+ignores its result. Either way the caller gets ERROR_TIMEO and the bytes are gone. Found through
+bbb_repo's ssh tunnel, which reads its client socket with a 5ms create_timeo in a loop; reproduced
+standalone losing ~60% of the bytes. epoll/kqueue only wait for readiness and read() after the
+resume, so they can't lose anything this way. 2026-09-23 */
+
+#if COLIB_OS_WINDOWS
+
+#include <atomic>
+#include <random>
+
+static const int test46_cnt = 1000;
+static std::atomic<int> test46_sent{0};
+
+static void test46_sender(uint16_t port) {
+    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
+    sockaddr_in addr = {};
+    addr.sin_family = AF_INET;
+    addr.sin_port = htons(port);
+    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
+    if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
+        closesocket(s);
+        return;
+    }
+    BOOL nodelay = TRUE;
+    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
+    std::mt19937 rng(1234);
+    for (int i = 0; i < test46_cnt; i++) {
+        char c = 'a' + i % 26;
+        if (send(s, &c, 1, 0) != 1)
+            break;
+        test46_sent++;
+        Sleep(rng() % 4);   /* 0..3ms: bytes keep landing around the 5ms read timeout */
+    }
+    Sleep(300);
+    closesocket(s);
+}
+
+static co::task_t test46_reader(SOCKET srv, int *received) {
+    auto pool = co_await co::get_pool();
+    sockaddr_in peer = {};
+    uint32_t len = sizeof(peer);
+    SOCKET c = co_await co::accept(srv, (sockaddr *)&peer, &len);
+    ASSERT_COFN(CHK_BOOL(c != INVALID_SOCKET));
+
+    char buff[4096];
+    while (true) {
+        auto [ret, err] = co_await co::create_timeo(co::read((HANDLE)c, buff, sizeof(buff)),
+                pool, std::chrono::microseconds(5000));
+        if (err == co::ERROR_TIMEO)
+            continue;
+        if (err != co::ERROR_OK || ret <= 0)
+            break;
+        *received += (int)ret;
+    }
+    closesocket(c);
+    co_return 0;
+}
+
+int test46_timeo_read_drops_bytes() {
+    WSADATA wsa;
+    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
+
+    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
+    ASSERT_FN(CHK_BOOL(srv != INVALID_SOCKET));
+    FnScope close_srv([srv]{ closesocket(srv); });
+    sockaddr_in addr = {};
+    addr.sin_family = AF_INET;
+    addr.sin_port = 0;
+    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
+    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
+    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
+    int addr_len = sizeof(addr);
+    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &addr_len) == 0));
+
+    std::thread sender(test46_sender, ntohs(addr.sin_port));
+    int received = 0;
+    auto pool = co::create_pool();
+    pool->sched(test46_reader(srv, &received));
+    co::run_e ret = pool->run();
+    sender.join();
+
+    DBG("sent: %d received: %d", test46_sent.load(), received);
+    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
+    ASSERT_FN(CHK_BOOL(test46_sent.load() == test46_cnt));
+    ASSERT_FN(CHK_BOOL(received == test46_sent.load()));
+    return 0;
+}
+
+#else /* COLIB_OS_WINDOWS */
+
+int test46_timeo_read_drops_bytes() {
+    DBG("readiness-based backend: read() runs after the resume, nothing can be lost this way");
+    return 0;
+}
+
+#endif /* COLIB_OS_WINDOWS */
+
+int main() {
+    int ret = test46_timeo_read_drops_bytes();
+    print_test_result("018-015-reproduced_timeo_read_drops_bytes.cpp", ret >= 0);
+    return ret;
+}
--- /dev/null
+++ b/tests/018-016-reproduced_killer_drops_sem_token.cpp
@@ -0,0 +1,63 @@
+#define COLIB_ENABLE_DEBUG_NAMES true
+
+#include "../colib.h"
+#include "tests_common.h"
+
+/* Test47 - Reproduced Bugs: a killer destroys a coroutine a semaphore already gave its token to
+================================================================================================= */
+
+/* sem_t::signal() takes the count down and queues the waiter. A kill that runs before that waiter
+is resumed finds it in the ready queue and destroys it: the token was consumed and nobody got it.
+The same flaw as 018-015, on semaphores and on every platform. Fixed, the killer resumes the waiter
+up to its next wait (the code after the wait gets the token) and destroys it there. 2026-09-23 */
+
+static int test47_got_token = 0;
+static int test47_after_next_wait = 0;
+static int test47_destructed = 0;
+static bool test47_ok = false;
+
+struct test47_marker_t {
+    ~test47_marker_t() { test47_destructed++; }
+};
+
+static co::task_t test47_victim(co::sem_p sem, co::sem_p never) {
+    test47_marker_t marker;
+    co_await sem->wait();
+    test47_got_token++;             /* must run: the wait completed, it holds the token */
+    co_await never->wait();         /* dies here, without waiting */
+    test47_after_next_wait++;       /* must not run */
+    co_return 0;
+}
+
+static co::task_t test47_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
+    co_await co::yield();           /* the victim runs and waits on `sem` */
+    sem->signal();                  /* the victim takes the token and is queued, not resumed yet */
+    co::error_e ret = kill_fn();
+    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
+    ASSERT_COFN(CHK_BOOL(test47_got_token == 1));   /* before the fix: 0, the token was lost */
+    ASSERT_COFN(CHK_BOOL(test47_after_next_wait == 0));
+    ASSERT_COFN(CHK_BOOL(test47_destructed == 1));
+    ASSERT_COFN(CHK_BOOL(sem->try_dec() == false)); /* the token wasn't handed out twice either */
+    test47_ok = true;
+    co_return 0;
+}
+
+int test47_killer_drops_sem_token() {
+    auto pool = co::create_pool();
+    auto sem = co::create_sem(pool, 0);
+    auto never = co::create_sem(pool, 0);
+    auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
+
+    pool->sched(co::add_modifs(pool.get(), test47_victim(sem, never), mods));
+    pool->sched(test47_controller(sem, kill_fn));
+    ASSERT_FN(pool->run());
+    ASSERT_FN(CHK_BOOL(test47_ok));
+    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
+    return 0;
+}
+
+int main() {
+    int ret = test47_killer_drops_sem_token();
+    print_test_result("018-016-reproduced_killer_drops_sem_token.cpp", ret >= 0);
+    return ret;
+}
```
