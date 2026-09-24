# Killer redesign: the `colib.h` diff

This is the `colib.h` diff for the killer redesign, plus the review notes. It started from
`REDESIGN_KILLER.md` with **variant B** of the lazy drop, and it has since been changed in review
(listed below). Nothing has been applied to the repo: `colib.h`, `tests/` and `docs/` are
unchanged.

How it's made: a scratch copy of `colib.h` gets the changes, and `diff -u -p` against the original
gives the hunks below. They aren't edited by hand; they're split into groups, each with an
explanation. Put together in order they make up the whole patch (`--- a/colib.h` /
`+++ b/colib.h`, 39 hunks, based on commit `45d4140`). Section 4 is one more patch on top of that.

Changes from `REDESIGN_KILLER.md` made in review:

- **Names:**
  - `ERROR_FORCE_SUSPEND` is `ERROR_SUSPENDED`.
  - `kill_incomplete_t` is `kill_deferred_t`: the kill couldn't happen now, so it's deferred to
    the target's next wait.
- **`kill_deferred_t`** inherits from `std::exception` (with `what()`) and still carries the
  `state_t *`. It sits right after `run_e`, with the other types, not in the function
  declarations, and `<exception>` is now included.
- **Comments:**
  - They're as short as the ones around them, and they don't explain this one bug fix.
  - The `error_e` block is realigned for the longer names, and so is the `dbg_enum(error_e)`
    switch.
  - These keep their original text: the `modif_e` doc, the `create_timeo` `@param timeo`, the
    `create_killer` doc (plus the contract in two lines: when the kill returns, the target is
    dead, or else the kill is deferred and throws `kill_deferred_t`), and the `make_shared`/018-014
    comment.
- **Dispatch rule:** an error wins, then `ERROR_SUSPENDED`, then `ERROR_OK`. An error means the wait
  doesn't happen. With no wait there's nothing to suspend, so the code after it runs "instantly"
  with the error. Callbacks whose return value is ignored (EXIT, LEAVE, ENTER, UNWAIT_IO,
  UNWAIT_SEM) always all run, so one of them can't keep another from running (see 3.4).
- **Retrying without handling the error spins, and that's the user's bug.** If a modif aborts a
  wait and the code retries it without handling the error, it spins. This is written down where
  modif authors look: the `CO_MODIF_WAIT_IO_CBK` doc, which the SEM and YIELD docs refer to.
- **Suspending a call:** it stays, without a comment at the call site. The rule is on
  `ERROR_SUSPENDED` itself: the modif that suspends a coroutine owns it.
- **No `keep_completed` flag.** `force_awake` always delivers a request that completed before the
  cancel, for the killer and for `co::stop_io` alike. That's the second promise: completed data is
  never dropped without someone knowing. Only the Windows backend changes; epoll, kqueue and the
  `COLIB_OS_UNKNOWN` skeleton keep their signatures.
- **The caller resumes in the target's turn.** When the target was in the ready queue (woken, maybe
  with an effect), the parent that called it is never run from inside the kill. It takes the
  target's place in the ready queue, with the target's value, error or exception, as if the
  target had run at its turn and returned. The target's slot stays in the queue as a placeholder
  while the kill runs, so tasks added or removed around it can't move it (a saved index would).
  Popping a placeholder means the scheduler ran inside a kill, which is a colib bug: that
  terminates.

Contents:

1. [Verification](#1-verification)
2. [Review notes](#2-review-notes)
3. [The diff](#3-the-diff)
4. [Extra hunk: the `co_yield` hole (tested)](#4-extra-hunk-the-co_yield-hole-tested)

---

## 1. Verification

The scratch copy of `tests/` is built through its own `windows.makefile` (MSVC 19.43, `/std:c++20`).
It uses the patched `colib.h`, the five new test files from `REDESIGN_KILLER.md` section 9, and the
two `ERROR_GENERIC -> ERROR_FINISHED` edits from 9.6. `011-006` uses `ERROR_SUSPENDED`, and
`002-004` catches `kill_deferred_t`. The whole suite runs with `make all`, again after every
review round, most recently on the diff below:

| | Result |
|---|---|
| Whole suite (53 tests, with `002-005` below) | **52 passed, 1 failed** |
| The failure: `018-011` (double ENTER on a vetoed CALL) | fails the same way on the **unpatched** `colib.h`: this is open `BUGS.md` #5, which is left alone on purpose. Not a regression. |
| `018-015` (timeo read drops bytes) on the unpatched `colib.h` | **fails**: `sent: 1000 received: 273` |
| `018-015`, `005-004`, `005-005` (the stop tests) with the patch | pass, 5 out of 5 reruns |
| `018-016`, `002-003`, `002-004`, `011-006` with the patch | pass |
| `002-005` (the caller resumes in the child's turn, see below) with the patch | passes |
| `002-005` on the diff before the caller-turn change | **fails**: the parent ran after the witness (`kill returned, ahead, witness, parent 7`) |
| `018-016` on the unpatched `colib.h` | **doesn't compile** (`ERROR_FINISHED` is not a member), see R2 |
| `003-003`, `018-005`, `018-006`, `018-014`, `009-001`, `002-001`, `011-003` | pass |

`002-005-flowctrl_killer_caller_turn.cpp` (a new test) has a parent that calls a child, and a
killer on the child. The child is woken and queued when the kill comes, with a witness task queued
behind it. It covers three cases:
- the child finishes inside the kill;
- the child dies at its next wait;
- the child removes a task queued *ahead* of it while it runs.

In all three the parent must run before the witness.

Not covered yet: a test where `co::stop_io` lands on a read that had already completed. It should
get the data (`ERROR_OK` plus the byte count), and `stop_io` should return `ERROR_FINISHED`.

---

## 2. Review notes

The model is right: every wait ends either completed or cancelled, never with its effect dropped.
Resuming ("driving") a runnable target up to its next suspension point is the smallest change that
keeps both promises without touching user code.

### Must fix

**R1: a driven root that `co_yield`s lets the whole scheduler run inside `kill()`.** *(Confirmed
by a probe.)* A scheduled root (no caller) that `co_yield`s goes through `cpp_yield_awaiter`,
which returns `pool->next_task()`. The killer's EXIT callback returns `ERROR_SUSPENDED` only when
the root has a caller, and the no-caller branch doesn't check it anyway. So other ready coroutines
run inside the kill. If none is ready, `handle_ready()` **blocks** in
`GetQueuedCompletionStatusEx`/`epoll_wait`, still inside the kill. The probe printed
`kill ret: ERROR_FINISHED witness ran inside kill: 1`. Section 4 has the fix, tested.

**R2: `018-016` can't be committed failing, as the reproduce-first workflow asks.** Its last line,
`kill_fn() == co::ERROR_FINISHED`, doesn't compile against today's `colib.h`. Remove that line;
`002-003` and the edited `002-002` already check `ERROR_FINISHED`. The 9.6 edits (`002-002`,
`018-005`) go in the same commit as the fix.

### Should fix

**R4: `create_timeo`'s "the kills can't throw here" has one exception.** Suppose `t` is suspended
in an external awaitable that doesn't call `external_on_suspend`. Then `entered` stays set, so to
the timer `t` looks like it's executing. `timer_elapsed_sig()` then throws `kill_deferred_t` out of
`timer_coro`, and `pool->run()` rethrows it. It's rare, but either the timer catches it (the
deferred death is fine there), or the claim needs that exception spelled out.

**R5: the second promise holds at the `co_await`, not necessarily for `create_timeo`'s caller.**
Take a composite `t`, say a `read`, then parsing, then another `read`. The code after the first
`read` runs and consumes the bytes, then `t` dies at the second `read`, and the caller gets
`ERROR_TIMEO`. For the ssh loop `t` *is* one `co::read`, so it always completes. This should be
said plainly in `docs/04_lifetimes.md`.

### Minor

- **R9:** `dbg_create_tracer` has no `CO_MODIF_WAIT_YIELD_CBK` entry.
- **R10:** on Windows, `io_had_effect` also says "effect" for a request that *failed*, because
  `handle_ready_events` sets `ERROR_OK` for every dequeued packet. That already happens today. The
  result is harmless: the target is driven instead of dropped, and it sees its failure.

### Resolved in this diff

- **R3 (a double free in `unwind()`):** today, an EXIT callback ordered before the killer's that
  returns an error stops the dispatch. The killer's callback then never pops its stack, and the
  unwind destroys the same frame twice. Callbacks whose return value is ignored now always all run.
- **R6 (`COLIB_OS_UNKNOWN` backends breaking):** gone. `force_awake` keeps its signature.
- **R7 (a timer handle in the cancel path):** a timer-flagged `io_data_t` is never reported as a
  completed read: `finished = ok && !(flags & IO_FLAG_TIMER)`.
- **R8 (suspending a call):** kept. The ownership rule is stated on `ERROR_SUSPENDED`.

**One earlier worry that turned out to be wrong.** With the new dispatch rule, the killer's WAIT
callback can answer `ERROR_SUSPENDED` (setting `parked` and posting the drop) while a later modif
aborts that same wait. That's still safe. The posted drop only runs once the resume has returned
to `run()`, and by then the chain has either parked at a later wait, which is the right one to
drop, or finished, in which case the stack is empty and the drop does nothing. While it's running,
any kill sees `entered` and throws before it reads `parked`. So the killer needs no change for
the new rule.

### About the unexplained access violation (`REDESIGN_KILLER.md` section 11)

This is a guess, to check with `018-015` under a debugger before the fix. When `CancelIoEx`
returns `ERROR_NOT_FOUND`, today's Windows `force_awake` doesn't wait before draining the port with
a zero timeout. A completion packet that isn't in the port yet at that moment gets dequeued later,
after its `io_data_t` has been freed. `handle_ready_events` then writes through freed memory. The
patched `force_awake` always waits (`GetOverlappedResult(..., TRUE)`), for the killer and for
`co::stop_io`.

### `co::stop_io` after this change

A request that completed before the stop is delivered to the coroutine as a normal completion,
and `stop_io` returns `ERROR_FINISHED`. The contract doesn't change: after `stop_io` the request is
off the engine (`awake_io` dequeues it) and the OS is done with its `OVERLAPPED`, so `close()`
right after it is safe. A `stop_io` that finds a request already completed now returns
`ERROR_FINISHED` instead of `ERROR_OK`. It's positive, so `< 0` checks still read it as success.

### Good side effects

- An aborted wait and a failed `wait_io()` registration now run UNWAIT. Today they don't, so the
  killer's `io_desc` can point at a dead awaiter.
- `create_timeo` no longer hangs when the timer's sleep fails.
- The posted destroy becomes a list. A single slot would leak once one resume finishes two roots,
  which the drive makes possible.

### Decisions left

- **Variant A or B** of the lazy drop. The diff is B. Variant A needs its LEAVE callback to stop the
  other LEAVE callbacks, and with the new dispatch rule LEAVE always runs all of them, so A no
  longer fits as written. B it is, unless you say otherwise.
- **A second exception in the same resume.** The diff still has "the first one wins" in
  `post_exception`. The proposal on the table is to terminate instead. With a drive, two scheduled
  roots can end with an exception in one resume, and `run()` can only hand out one per call.

---

## 3. The diff

### 3.1 Includes, `error_e`, `kill_deferred_t`, `modif_e`, `modif_t::variant_t`

The two new codes are positive, so every `ret < 0` / `ASSERT_*` check still reads them as success.
`kill_deferred_t` sits with the types. `CO_MODIF_WAIT_YIELD_CBK` goes at the end, so existing
indices keep their values, and the variant's new alternative has the same index. The
`CO_MODIF_WAIT_IO_CBK` doc gets the spin note.

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
@@ -675,14 +676,16 @@ using modif_pack_t = std::vector<modif_p
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
@@ -693,6 +696,14 @@ enum run_e : int32_t {
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
@@ -715,7 +726,8 @@ enum modif_e : int32_t {
     CO_MODIF_ENTER_CBK,
 
     /*! This is called when a corutine is waiting for an IO (after the leave cbk). If the return
-    value is not ERROR_OK, then the wait is aborted. */
+    value is negative, then the wait is aborted. A negative value is returned by the wait at
+    once: code that retries the wait without handling that error spins. */
     CO_MODIF_WAIT_IO_CBK,
 
     /*! This is called when the io is done and the corutine that awaited it is resumed */
@@ -727,6 +739,9 @@ enum modif_e : int32_t {
     /*! This is similar to unwait_io, but on a semaphore */
     CO_MODIF_UNWAIT_SEM_CBK,
 
+    /*! This is similar to wait_io, but on co::yield() and co::force_stop() */
+    CO_MODIF_WAIT_YIELD_CBK,
+
     CO_MODIF_COUNT,
 };
 
@@ -1145,7 +1160,9 @@ struct modif_t {
         std::function<error_e(state_t *, sem_t *, sem_waiter_handle_p)>,
 
          /* unwait_sem_cbk - No handle here, as the semaphore is no longer in the waiting list */
-        std::function<error_e(state_t *, sem_t *)>
+        std::function<error_e(state_t *, sem_t *)>,
+
+        std::function<error_e(state_t *)>               /* wait_yield_cbk */
     >;
 
     /*! This is the callback that will be called on the location specified by type. It must be
```

### 3.2 `create_killer`'s doc

The original doc, plus the contract in two lines.

```diff
--- a/colib.h
+++ b/colib.h
@@ -1552,6 +1569,8 @@ inline task<sem_p> create_sem(int64_t va
  * callback that runs as a side effect of the kill it already triggered - is caught and rejected
  * the same way (also reported as "nothing to kill"), rather than corrupting the in-progress
  * unwind.
+ * When the kill returns, the target is dead. A kill called from inside the target can't be done
+ * now: it is deferred to the target's next wait and throws kill_deferred_t.
  * @param pool The pool on which to bind this killer. The killer may outlive it: nothing of the
  *             killer is allocated from the pool.
  * @param e The error value that will be set inside the killed coroutine on kill
```

### 3.3 Debug checks

The macro in both branches and the forward declaration. The definition is at the end of the file
(3.12).

```diff
--- a/colib.h
+++ b/colib.h
@@ -2231,6 +2250,7 @@ struct dbg_scope_t {
 # define COLIB_DEBUG_CHECK_UNWAIT_IO(s, io) dbg_check_modif_unwait_io(s, io)
 # define COLIB_DEBUG_CHECK_WAIT_SEM(s, sem, it) dbg_check_modif_wait_sem(s, sem, it)
 # define COLIB_DEBUG_CHECK_UNWAIT_SEM(s, sem) dbg_check_modif_unwait_sem(s, sem)
+# define COLIB_DEBUG_CHECK_WAIT_YIELD(s) dbg_check_modif_wait_yield(s)
 
 # define COLIB_ENABLE_DEBUG_CHECK_ASSERT(x, fmt, ...) \
 do { \
@@ -2250,6 +2270,7 @@ inline void dbg_check_modif_wait_io(stat
 inline void dbg_check_modif_unwait_io(state_t *s, io_desc_t &io);
 inline void dbg_check_modif_wait_sem(state_t *s, sem_t *sem, sem_waiter_handle_p it);
 inline void dbg_check_modif_unwait_sem(state_t *s, sem_t *sem);
+inline void dbg_check_modif_wait_yield(state_t *s);
 
 struct dbg_check_state_t {
     uint32_t called : 1 = false;
@@ -2277,6 +2298,7 @@ inline std::map<pool_t *,
 # define COLIB_DEBUG_CHECK_UNWAIT_IO(...) ;
 # define COLIB_DEBUG_CHECK_WAIT_SEM(...) ;
 # define COLIB_DEBUG_CHECK_UNWAIT_SEM(...) ;
+# define COLIB_DEBUG_CHECK_WAIT_YIELD(...) ;
 # define COLIB_ENABLE_DEBUG_CHECK_ASSERT(x, fmt, ...) ;
 #endif /*COLIB_ENABLE_DEBUG_CHECKS*/
 
```

### 3.4 Modif table, dispatch, `do_yield_modifs`

The tenth vector, plus a `static_assert` so the table can't fall behind `modif_e` again. The
dispatch:
- the first negative return stops it and is the result: the wait doesn't happen;
- otherwise `ERROR_SUSPENDED`, if any callback returned it;
- otherwise `ERROR_OK`.

EXIT/LEAVE/ENTER/UNWAIT_* ignore their return value, so they always all run.
`auto modif_table = state->modif_table` copies the `shared_ptr`, which keeps the table alive if a
callback destroys the frame.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2449,9 +2471,11 @@ struct modif_table_t {
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
+            std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}},
             std::vector<modif_p, allocator_t<modif_p>>{allocator_t<modif_p>{pool}}
     } {}
 
+    static_assert(CO_MODIF_COUNT == 10, "one vector per modif_e in the constructor above");
     std::array<std::vector<modif_p, allocator_t<modif_p>>, CO_MODIF_COUNT> table;
 };
 
@@ -2491,18 +2515,25 @@ inline void inherit_modifs(state_t *stat
         state->modif_table = new_table;   
 }
 
+/* An error wins (the wait doesn't happen), then ERROR_SUSPENDED, then ERROR_OK */
 template <modif_e cbk_id, typename ...Args>
 inline error_e do_generic_modifs(state_t *state, Args&& ...args) {
+    /* the return value of those is ignored, so one of them can't stop the others */
+    constexpr bool ignored_ret =
+            cbk_id == CO_MODIF_EXIT_CBK || cbk_id == CO_MODIF_LEAVE_CBK ||
+            cbk_id == CO_MODIF_ENTER_CBK || cbk_id == CO_MODIF_UNWAIT_IO_CBK ||
+            cbk_id == CO_MODIF_UNWAIT_SEM_CBK;
+    error_e result = ERROR_OK;
     if (auto modif_table = state->modif_table) {
         for (auto &modif : modif_table->table[cbk_id]) {
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
@@ -2561,6 +2592,12 @@ inline error_e do_unwait_sem_modifs(stat
     return do_generic_modifs<CO_MODIF_UNWAIT_SEM_CBK>(state, sem);
 }
 
+inline error_e do_yield_modifs(state_t *state) {
+    COLIB_DEBUG_TRACE("   YIELD: %s state: %p", dbg_name(state->self).c_str(), state);
+    COLIB_DEBUG_CHECK_WAIT_YIELD(state);
+    return do_generic_modifs<CO_MODIF_WAIT_YIELD_CBK>(state);
+}
+
 /* considering you may want to create a modif at runtime this seems to be the best way */
 template <modif_e type_id, typename Cbk>
 inline modif_p create_modif(modif_flags_e flags, Cbk&& cbk) {
```

### 3.5 `task<T>::await_suspend`

`ERROR_SUSPENDED` on a call: the caller has left and the callee never starts. Control goes back to
whoever resumed the caller (usually `pool_t::run`), and nothing is queued. The modif that
suspended it owns both frames.

```diff
--- a/colib.h
+++ b/colib.h
@@ -2679,7 +2716,10 @@ inline handle<void> task<T>::await_suspe
 
     inherit_modifs(state, caller.promise().state.modif_table, CO_MODIF_INHERIT_ON_CALL);
 
-    if (do_call_modifs(state) != ERROR_OK) {
+    error_e err = do_call_modifs(state);
+    if (err == ERROR_SUSPENDED)
+        return std::noop_coroutine();
+    if (err != ERROR_OK) {
         do_entry_modifs(&caller.promise().state);
         return caller;
     }
```

### 3.6 Windows `io_pool_t::force_awake`

It always waits for the cancel to settle. A request that completed successfully before the cancel
(and isn't a timer) is delivered with `ERROR_OK` and its byte count, and the function returns
`ERROR_FINISHED`. The handle-wide loop checks `< 0`, so that positive code isn't taken for a
failure.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3424,6 +3464,8 @@ struct io_pool_t {
             COLIB_ENABLE_DEBUG_CHECK_ASSERT(data, "invalid data ptr");
             COLIB_ENABLE_DEBUG_CHECK_ASSERT(data->h, "invalid inner handle");
             COLIB_DEBUG_TRACE("awake: handle: %p", data->h);
+            bool finished = false;
+            DWORD transferred = 0;
             if ((data->flags & io_data_t::IO_FLAG_TIMER) &&
                     (data->flags & io_data_t::IO_FLAG_TIMER_RUN))
             {
@@ -3435,19 +3477,16 @@ struct io_pool_t {
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
@@ -3458,6 +3497,13 @@ struct io_pool_t {
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
 
@@ -3473,7 +3519,7 @@ struct io_pool_t {
             }
             for (auto data : datas) {
                 error_e err;
-                if ((err = awake_data(data)) != ERROR_OK)
+                if ((err = awake_data(data)) < 0)
                     return err;
             }
         }
```

### 3.7 `pool_internal_t`

`run()` calls `run_posted()` instead of the single-slot destroy:
- `post_to_destroy` becomes a list;
- the first posted exception wins;
- `post_after_resume` carries the lazy drop;
- `is_ready()` is a lookup that doesn't remove;
- `replace_ready()` puts one state in another's place in the ready queue;
- `next_task_state()` terminates if it pops a killer's placeholder (a null `self`).

The members use `std::vector` with the default allocator, so nothing posted is freed into a pool
that may already be gone.

```diff
--- a/colib.h
+++ b/colib.h
@@ -3799,10 +3845,7 @@ struct pool_internal_t {
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
@@ -3819,11 +3862,31 @@ struct pool_internal_t {
     }
 
     void post_to_destroy(state_t *s) {
-        posted_to_destroy = s;
+        posted_to_destroy.push_back(s);
     }
 
     void post_exception(std::exception_ptr exc) {
-        posted_exception = exc;
+        if (!posted_exception)
+            posted_exception = exc;     /* the first one wins */
+    }
+
+    /* runs after the current resume, before the next task */
+    void post_after_resume(std::function<void(void)> fn) {
+        posted_after_resume.push_back(std::move(fn));
+    }
+
+    /* running the posted work can post more */
+    void run_posted() {
+        while (posted_after_resume.size() || posted_to_destroy.size()) {
+            auto fns = std::move(posted_after_resume);
+            posted_after_resume.clear();
+            for (auto &fn : fns)
+                fn();
+            auto to_destroy = std::move(posted_to_destroy);
+            posted_to_destroy.clear();
+            for (auto s : to_destroy)
+                s->self.destroy();
+        }
     }
 
     void post_stop() {
@@ -3838,6 +3901,19 @@ struct pool_internal_t {
         ready_tasks.push_front(state);
     }
 
+    bool is_ready(state_t *state) {
+        return std::find(ready_tasks.begin(), ready_tasks.end(), state) != ready_tasks.end();
+    }
+
+    /* puts `with` in the place of `state` in the ready queue */
+    bool replace_ready(state_t *state, state_t *with) {
+        auto it = std::find(ready_tasks.begin(), ready_tasks.end(), state);
+        if (it == ready_tasks.end())
+            return false;
+        *it = with;
+        return true;
+    }
+
     bool remove_ready(state_t *state) {
         COLIB_DEBUG_TRACE_SCOPE("state: %p", state);
 
@@ -3893,6 +3969,8 @@ struct pool_internal_t {
 
         if (!ready_tasks.empty()) {
             auto ret = ready_tasks.front();
+            if (!ret->self)
+                std::terminate();   /* a killer's placeholder: the scheduler ran inside a kill */
             ready_tasks.pop_front();
             COLIB_DEBUG_TRACE("next_state: %p", ret);
             return ret;
@@ -3960,7 +4038,8 @@ private:
     timer_pool_t timer_pool;
 
     std::exception_ptr posted_exception = nullptr;
-    state_t *posted_to_destroy = nullptr;
+    std::vector<state_t *> posted_to_destroy;
+    std::vector<std::function<void(void)>> posted_after_resume;
     bool posted_stop = false;
 
     /* bookkeeping for end of life destruction */
```

### 3.8 Exit paths: `cpp_yield_awaiter`, `final_awaiter_cleanup`

`ERROR_SUSPENDED` from the EXIT callbacks means "don't jump into the caller, the modif queued it".
The no-caller branch of `cpp_yield_awaiter` doesn't check it yet; that's R1, fixed in section 4.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4022,9 +4101,11 @@ inline handle<void> cpp_yield_awaiter(st
 
     /* from the point of view of the corutine modifications we are exiting here, this keeps the
     call stack proper */
-    do_exit_modifs(yielding_task_state);
+    error_e exit_err = do_exit_modifs(yielding_task_state);
 
     if (caller_state) {
+        if (exit_err == ERROR_SUSPENDED)
+            return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
 
@@ -4034,11 +4115,12 @@ inline handle<void> cpp_yield_awaiter(st
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

### 3.9 Awaiters: `yield`, `io`, `sem`, `force_stop`

Every awaiter has three outcomes from its WAIT/YIELD callbacks:
- **suspended**: unwait, leave, return `noop`, and touch nothing after that;
- **aborted**: unwait, and continue right away with the error;
- **wait**: as today.

`yield_awaiter_t` and `stop_awaiter_t` get a `triggered` flag, so a refused yield doesn't cause a
second ENTER.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4167,22 +4249,32 @@ struct yield_awaiter_t {
 
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
-        do_leave_modifs(&h.promise().state);
-        pool->get_internal()->push_ready(&h.promise().state);
+
+        error_e err = do_yield_modifs(state);
+        if (err == ERROR_SUSPENDED) {
+            /* parked by a modif */
+            do_leave_modifs(state);
+            return std::noop_coroutine();
+        }
+        if (err != ERROR_OK)
+            return h;       /* refused by a modif */
+
+        do_leave_modifs(state);
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
@@ -4267,8 +4359,18 @@ struct io_awaiter_t {
 
         auto pool = h.promise().state.pool;
         state = &h.promise().state;
-        /* in case we can't schedule the fd we log the failure and return the same coro */
-        if ((ret_err = do_wait_io_modifs(state, io_desc)) != ERROR_OK) {
+
+        error_e err = do_wait_io_modifs(state, io_desc);
+        if (err == ERROR_SUSPENDED) {
+            /* parked by a modif, `this` may be gone after do_leave_modifs */
+            do_unwait_io_modifs(state, io_desc);
+            do_leave_modifs(state);
+            return std::noop_coroutine();
+        }
+        if (err != ERROR_OK) {
+            /* aborted by a modif */
+            ret_err = err;
+            do_unwait_io_modifs(state, io_desc);
             return h;
         }
 
@@ -4276,6 +4378,7 @@ struct io_awaiter_t {
         if (ret_err != ERROR_OK) {
             COLIB_DEBUG("Failed to register wait: %s on: %s",
                     dbg_enum(ret_err).c_str(), dbg_name(h).c_str());
+            do_unwait_io_modifs(state, io_desc);
             return h;
         }
         do_leave_modifs(state);
@@ -4443,9 +4546,16 @@ struct sem_awaiter_t {
         auto pool = sem->get_internal()->get_pool();
         psem_it = sem->get_internal()->push_waiter(state);
 
-        if (do_wait_sem_modifs(state, sem, psem_it) != ERROR_OK) {
-            COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
+        error_e err = do_wait_sem_modifs(state, sem, psem_it);
+        if (err != ERROR_OK) {
             sem->get_internal()->erase_waiter(*psem_it);
+            do_unwait_sem_modifs(state, sem);
+            if (err == ERROR_SUSPENDED) {
+                /* parked by a modif */
+                do_leave_modifs(state);
+                return std::noop_coroutine();
+            }
+            COLIB_DEBUG_TRACE("User stopped wait on semaphore: state[%p] sem[%p]", state, sem);
             return to_suspend;
         }
         do_leave_modifs(state);
@@ -4591,23 +4701,36 @@ inline task_t force_stop(int64_t stopval
             COLIB_DEBUG_TRACE_SCOPE("force_stop state: %p", &h.promise().state);
 
             state = &h.promise().state;
+
+            error_e err = do_yield_modifs(state);
+            if (err == ERROR_SUSPENDED) {
+                /* parked by a modif, the pool isn't stopped */
+                do_leave_modifs(state);
+                return std::noop_coroutine();
+            }
+            if (err != ERROR_OK)
+                return h;   /* refused by a modif */
+
             do_leave_modifs(state);
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

### 3.10 `create_timeo`

- `decided` makes the result get set and `sem` signaled exactly once.
- `timer_firing` stops `exec_coro` from killing the timer that is resuming it (a self-kill).
- The sleep-error path now signals `sem` too.

```diff
--- a/colib.h
+++ b/colib.h
@@ -5822,6 +5945,8 @@ inline task<std::pair<T, error_e>> creat
         task<T> t;
         T ret;
         int id;
+        bool decided = false;       /* the result is set and sem signaled */
+        bool timer_firing = false;  /* timer_coro is inside timer_elapsed_sig() */
     };
 
     auto tstate = std::shared_ptr<timer_state_t>(alloc<timer_state_t>(pool),
@@ -5844,7 +5969,10 @@ inline task<std::pair<T, error_e>> creat
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
@@ -5852,18 +5980,22 @@ inline task<std::pair<T, error_e>> creat
 
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
```

### 3.11 The killer: `io_had_effect`, `killer_state_t`, `create_killer`

This replaces the local `kill_state_t` and the `sig_kill` lambda. `kill()` works out what the
target is doing:
- executing;
- parked;
- scheduled but never started;
- runnable, with or without an effect;
- waiting on an io;
- waiting on a semaphore;
- suspended on an external awaitable.

Depending on which, it drops the target, resumes it up to its next wait, or throws
`kill_deferred_t`. Once the chain is `dying`, the callbacks answer every wait with `park()`.

When the target is in the ready queue, `take_turn()` replaces it there with a placeholder before
dropping or resuming it. `queue_caller()` then puts the target's caller in that placeholder's
place, falling back to the back of the queue when the target had no place. A placeholder nobody
took is removed at the end.

```diff
--- a/colib.h
+++ b/colib.h
@@ -5881,133 +6013,217 @@ inline task<std::pair<T, error_e>> creat
     return ret_coro;
 }
 
-/* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
-/* this is inherited by-call and it must kill all coros in the call path and also stop all waiters
-(io and sem) */
-/* CAUTION: This will be similar to an exception thrown on the active await and the call stack */
-inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e) {
-    struct kill_state_t {
-        std::stack<state_t *> call_stack;
-        io_desc_t *io_desc = nullptr;
-        sem_t *sem = nullptr;
-        sem_waiter_handle_p it;
-        bool killing_activated = false;
-    };
+#if COLIB_OS_WINDOWS
+/* a completed IOCP request already moved its bytes, timers and wake-ups moved nothing */
+inline bool io_had_effect(const io_desc_t& io) {
+    return io.data && !(io.data->flags & io_data_t::IO_FLAG_TIMER) && io.data->state &&
+            io.data->state->err == ERROR_OK;
+}
+#else /* COLIB_OS_WINDOWS */
+/* epoll/kqueue only wait for readiness, the read happens after the resume */
+inline bool io_had_effect(const io_desc_t&) {
+    return false;
+}
+#endif /* COLIB_OS_WINDOWS */
 
-    /* TODO: fix, calling killer from killer (as a result of killing a coro) is not ok,
-    maybe have a way to guard, error out or something? We need to somehow warn the user
-    that he did that */
+/* One per create_killer() call, shared by its modifs and its kill function */
+struct killer_state_t : std::enable_shared_from_this<killer_state_t> {
+    killer_state_t(error_e e) : e(e) {}
+
+    error_e e;                          /* the err of a killed root that was called */
+
+    std::stack<state_t *> call_stack;   /* top() is the innermost frame */
+    io_desc_t *io_desc = nullptr;       /* the io the top waits on */
+    sem_t *sem = nullptr;               /* the semaphore the top waits on */
+    sem_waiter_handle_p it;             /* the top's place in the semaphore's wait list */
+
+    bool entered = false;   /* the top is executing, or was scheduled and never started */
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
 
-    /* Plain std::make_shared, not the pool's allocator: the killer and its modif pack are the
-    user's to hold, and may be let go of after the pool is gone, which would then free the state -
-    and the shared_ptr's own count - into a pool that no longer exists. Same reasoning as modif_t's
-    own allocation (see 018-010); 018-014 is this one's regression test. 2026-09-23 05:06 */
-    (void)pool;
-    auto kstate = std::make_shared<kill_state_t>();
+inline error_e killer_state_t::kill() {
+    /* called from our own unwind */
+    if (unwinding)
+        return ERROR_GENERIC;
+    if (call_stack.empty())
+        return ERROR_FINISHED;
 
-    COLIB_DEBUG_TRACE("created killer: %p", kstate.get());
+    state_t *top = call_stack.top();
+    pool_internal_t *pool = top->pool->get_internal();
+    bool in_ready = pool->is_ready(top);
 
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
+    /* executing, it dies at its next wait */
+    if (entered && !in_ready) {
+        dying = true;
+        throw kill_deferred_t(top);
+    }
 
-        kstate->killing_activated = true;
+    /* parked, the posted drop didn't run yet */
+    if (parked) {
+        unwind();
+        return ERROR_OK;
+    }
 
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
+    if (in_ready) {
+        if (entered) {
+            /* scheduled, never started */
+            pool->remove_ready(top);
+            do_leave_modifs(top);
+            unwind();
+            return ERROR_OK;
         }
+        /* woken: with an effect it runs up to its next wait, else it is dropped */
+        return take_turn(top, (io_desc && io_had_effect(*io_desc)) || sem);
+    }
 
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
+    if (io_desc) {
+        /* waiting on an io, an io that completed meanwhile is delivered */
+        error_e ret = pool->stop_io(*io_desc, ERROR_WAKEUP);
+        if (ret != ERROR_OK && ret != ERROR_FINISHED) {
+            COLIB_DEBUG("WARNING: failed to stop the io of a killed coroutine: %s",
+                    dbg_enum(ret).c_str());
         }
+        return take_turn(top, ret == ERROR_FINISHED);   /* stop_io queued it */
+    }
 
-        /* Now we unwind the call stack, removing all except the last one. The last one will be
-        removed by it's caller */
-        while (kstate->call_stack.size() > 1) {
-            COLIB_DEBUG_TRACE("Unwinding: %p", kstate->call_stack.top());
-            state = kstate->call_stack.top();
-            do_exit_modifs(state); /* OBS: the self(killer) exit modif will pop the stack */
-            state->self.destroy();
-        }
+    if (sem) {
+        /* waiting on a semaphore, no token was taken */
+        sem->get_internal()->erase_waiter(*it);
+        drop(top);
+        return ERROR_OK;
+    }
 
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
+    /* suspended on an external awaitable */
+    unwind();
+    return ERROR_OK;
+}
 
-        return ERROR_OK;
-    };
+/* The target is in the ready queue: its place there is kept (a placeholder) for its caller */
+inline error_e killer_state_t::take_turn(state_t *top, bool effect) {
+    pool_internal_t *pool = top->pool->get_internal();
+    state_t placeholder;
+    pool->replace_ready(top, &placeholder);
+    pool->remove_ready(top);
+    turn = &placeholder;
+
+    error_e ret = ERROR_OK;
+    if (effect)
+        ret = drive();
+    else
+        drop(top);
 
-    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
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
+    if (io_desc) {
+        io_desc_t &io = *io_desc;
+        do_entry_modifs(top);
+        do_unwait_io_modifs(top, io);
+        do_leave_modifs(top);
+    }
+    else if (sem) {
+        sem_t *s = sem;
+        do_entry_modifs(top);
+        do_unwait_sem_modifs(top, s);
+        do_leave_modifs(top);
+    }
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
+/* the answer of the wait cbks once the chain is dying */
+inline error_e killer_state_t::park(state_t *s) {
+    parked = true;
+    if (!driving) {
+        /* nobody resumes it for us, drop it after this resume */
+        auto self = shared_from_this();
+        s->pool->get_internal()->post_after_resume([self]{
+            if (self->parked && !self->unwinding && !self->call_stack.empty())
+                self->unwind();
+        });
+    }
+    return ERROR_SUSPENDED;
+}
 
+/* CAUTION: this doesn't kill sched paths (for example 'futures' or 'wait_all') */
+/* this is inherited by-call and it must kill all coros in the call path and also stop all waiters
+(io and sem) */
+/* CAUTION: This will be similar to an exception thrown on the active await and the call stack */
+inline std::pair<modif_pack_t, std::function<error_e(void)>> create_killer(pool_t *pool, error_e e) {
+    /* Plain std::make_shared, not the pool's allocator: the killer and its modif pack are the
+    user's to hold, and may be let go of after the pool is gone, which would then free the state -
+    and the shared_ptr's own count - into a pool that no longer exists. Same reasoning as modif_t's
+    own allocation (see 018-010); 018-014 is this one's regression test. 2026-09-23 05:06 */
+    (void)pool;
+    auto kstate = std::make_shared<killer_state_t>(e);
+
+    COLIB_DEBUG_TRACE("created killer: %p", kstate.get());
+
+    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
     modif_pack_t pack;
     pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
         [kstate](state_t *s) -> error_e {
@@ -6026,18 +6242,40 @@ inline std::pair<modif_pack_t, std::func
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
+                if (s->caller_state) {
+                    kstate->queue_caller(s->caller_state);
+                    return ERROR_SUSPENDED;
+                }
+            }
+            return ERROR_OK;
+        }
+    ));
+    pack.push_back(create_modif<CO_MODIF_ENTER_CBK>(flags,
+        [kstate](state_t *s) -> error_e {
+            (void)s;
+            kstate->entered = true;
+            return ERROR_OK;
+        }
+    ));
+    pack.push_back(create_modif<CO_MODIF_LEAVE_CBK>(flags,
+        [kstate](state_t *s) -> error_e {
+            (void)s;
+            kstate->entered = false;
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
@@ -6054,9 +6292,10 @@ inline std::pair<modif_pack_t, std::func
     ));
     pack.push_back(create_modif<CO_MODIF_WAIT_SEM_CBK>(flags,
         [kstate](state_t *s, sem_t *sem, sem_waiter_handle_p it) -> error_e {
-            (void)s;
             COLIB_DEBUG_TRACE("WAIT_SEM[%p]: tracking killer: %p sem: %p it-ptr: %p",
                     kstate.get(), s, sem, it.get());
+            if (kstate->dying)
+                return kstate->park(s);
             kstate->sem = sem;
             kstate->it = it;
             return ERROR_OK;
@@ -6075,8 +6314,15 @@ inline std::pair<modif_pack_t, std::func
             return ERROR_OK;
         }
     ));
+    pack.push_back(create_modif<CO_MODIF_WAIT_YIELD_CBK>(flags,
+        [kstate](state_t *s) -> error_e {
+            if (kstate->dying)
+                return kstate->park(s);
+            return ERROR_OK;
+        }
+    ));
 
-    return {pack, sig_kill};
+    return {pack, [kstate]() -> error_e { return kstate->kill(); }};
 }
 
 /* Debug stuff
```

### 3.12 `dbg_enum(error_e)` and `dbg_check_modif_wait_yield`

```diff
--- a/colib.h
+++ b/colib.h
@@ -6106,14 +6352,16 @@ inline dbg_string_t dbg_name(handle<P> h
 
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
 
@@ -6510,6 +6758,18 @@ inline void dbg_check_modif_unwait_sem(s
     dbg_state.sem_it = nullptr;
 }
 
+inline void dbg_check_modif_wait_yield(state_t *s) {
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s, "no-state");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(s->pool, "no-pool");
+    auto &pool_map = dbg_check_coro_states[s->pool];
+    auto &dbg_state = pool_map[s];
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.summon_cnt, "never called, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(dbg_state.entered, "never entered, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!(dbg_state.entered && dbg_state.left), "already left, but yield");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.io, "yield while waiting on io");
+    COLIB_ENABLE_DEBUG_CHECK_ASSERT(!dbg_state.sem, "yield while waiting on sem");
+}
+
 
 #endif /* COLIB_ENABLE_DEBUG_CHECKS */
 
```

---

## 4. Extra hunk: the `co_yield` hole (tested)

This goes on top of section 3 and fixes R1. While resuming the target, the killer takes over the
root's exit whether or not it has a caller, and `cpp_yield_awaiter`'s no-caller branch respects
that. `final_awaiter_cleanup`'s no-caller branch already returns `noop`.

```diff
--- a/colib.h
+++ b/colib.h
@@ -4108,6 +4108,8 @@ inline handle<void> cpp_yield_awaiter(st
             return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
+    if (exit_err == ERROR_SUSPENDED)
+        return std::noop_coroutine();       /* a modif took over */
 
     return yielding_task_state->pool->get_internal()->next_task();
 }
@@ -6248,10 +6250,9 @@ inline std::pair<modif_pack_t, std::func
             if (kstate->driving && kstate->call_stack.empty()) {
                 /* the root exited while driven, its caller is queued, not resumed */
                 kstate->finished = true;
-                if (s->caller_state) {
+                if (s->caller_state)
                     kstate->queue_caller(s->caller_state);
-                    return ERROR_SUSPENDED;
-                }
+                return ERROR_SUSPENDED;
             }
             return ERROR_OK;
         }
```

Tested with sections 3 and 4 both applied. The R1 probe passes, and so do `002-003`, `002-004`,
`018-015`, `018-016`, `009-001`, `003-003` and `011-006`.
