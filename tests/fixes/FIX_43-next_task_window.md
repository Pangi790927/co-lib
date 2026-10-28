# Proposal: a suspension ends before the scheduler runs (fix #43)

Status: `[APPLIED 2026-10-13 on the user's word]`. The diff below is against the working tree's
`colib.h` of 2026-10-13 02:04 (commit 9a13a9a with FIX_44, FIX_45 and FIX_47 applied, and the doc
comments rewritten at 02:03; uncommitted; md5 27c49012725a). The design is the user's
(2026-10-13): the awaiters return `std::noop_coroutine()` instead of calling the scheduler from
their `await_suspend`, the rule `final_awaiter_ends()` already follows.

After the apply, `colib.h` is that header with the diff below, byte for byte (md5 b78800fd80be),
and `make` in `tests/` builds and runs its 92 tests against it: `[PASSED: 92]/[FAILED: 0]`; under
ASan, on a scratch copy with the same code, `[PASSED: 92]/[FAILED: 0]`. The regression test landed
as `tests/018-038-reproduced_next_task_window.cpp`, its placeholder number. The docs listed under
Interactions were updated with the apply (`docs/TODO.md` item 8 went), except the sentence in
`tests/CLAUDE.md`, a CLAUDE.md file, which waits for the user's word. The open questions were left
open.

## The bug

Five places end a suspension by picking the next coroutine themselves, inside the `await_suspend`
of the coroutine X that suspends. `yield_awaiter_t`, `sem_awaiter_t` and `io_awaiter_t` first link
X where it waits (the ready queue for a yield, the semaphore's wait list, the io pool);
`cpp_yield_awaiter()` leaves a generator without a caller with whoever holds it. Each then returns
`pool_internal_t::next_task()`, the handle of the coroutine that `next_task_state()` picks.
`external_wait_next_task()` is the same call, made public: an external awaitable returns it from
its own `await_suspend` (Option 3 in the docs of the external awaitables).

`next_task_state()` runs user code before it picks:

- with `COLIB_ENABLE_MULTITHREAD_SCHED`, `_take_thread_tasks()` takes in the coroutines that other
  threads handed over with `thread_sched()`, and runs their SCHED callbacks;
- when nothing is ready and an io wait is registered, `io_pool_t::handle_ready()` waits for io. On
  Windows that wait, `GetQueuedCompletionStatusEx(..., INFINITE, TRUE)`, is alertable, so the APCs
  queued to the pool's thread run inside it.

That code can end X. X sits in a colib queue, so a kill finds it owned by colib and ends it, one of
two ways. If X's wait did nothing yet, the kill ends X at once: `killer_state_t::end_chain()` frees
the frame. If the wait already did its work (the semaphore was signaled before the kill, the io
completed before the stop), `drive()` runs X on, inside the kill, to its next wait, and frees it
there: X runs past its wait while its own `await_suspend` is still on the stack. The kill isn't the
only way: a SCHED callback that clears the semaphore X waits on frees X too, whether it calls
`clear()` or drops the semaphore's last `sem_p` (its destructor clears it). Either way `next_task()`
then returns into X's `await_suspend`, which still runs on the freed frame, and the return writes
into it: MSVC keeps the handle an `await_suspend` returns in the coroutine's frame. The write (8
bytes, the handle) lands in freed memory, and the program goes on from there. GCC keeps that handle
off the frame, so with GCC the window is silent: the standard counts the coroutine as suspended
while its `await_suspend` runs, and nothing touches the frame after the free.

The same call fails a second way, inside a kill. When a kill runs X on (its wait did its work) into
an external awaitable that returns `external_wait_next_task()`, the scheduler runs inside the kill,
where the kill's placeholder holds X's place in the ready queue. With the placeholder first,
`next_task_state()` pops it and calls `std::terminate()` ("the scheduler ran inside a kill"); with
another coroutine first, that coroutine runs inside the kill. And a SCHED callback that resumes a
held coroutine, which then yields, re-enters `_take_thread_tasks()`: the nested pass takes the same
handover again, and the outer pass queues it twice (`std::terminate()`, "in two queues at once").

FIX_02 doesn't close this window. It makes a kill throw for a target that sits in no colib queue
(`STATE_LEFT`), and X here is queued (`STATE_READY`, `STATE_WAITING_SEM`, `STATE_WAITING_IO`).

**The trigger** (`repro/43-next_task_window.cpp`): X runs on the pool; another thread hands T over
with `thread_sched()`, and T carries a SCHED callback that calls X's kill; then X yields. The pool
takes T in inside X's yield, T's SCHED callback kills X, and X's frame is freed under its own
`await_suspend`. Two atomics order the handover, so the run is deterministic. The semaphore variant
(`repro/fix/43/43-sem-variant.cpp`) waits on a semaphore that nobody signals instead of yielding.

**The consequence**, confirmed under ASan for the yield program on 9dfe29e (HEAD at the time) and
for both programs on the working tree (`repro/43-next_task_window.md`), and run again on the copy
this fix was built on (`repro/fix/43/noop/base/repro/run-yield-asan.log`, `run-sem-asan.log`),
frames shortened:

```
==25560==ERROR: AddressSanitizer: heap-use-after-free on address 0x1239ae1a02b8
WRITE of size 8 at 0x1239ae1a02b8 thread T0
    #0 colib::pool_internal_t::next_task(void)
    #1 colib::yield_awaiter_t::await_suspend<struct colib::task_state_t<int>>
    #2 victim$_ResumeCoro$1                                  43-next_task_window.cpp:52
    #3 std::coroutine_handle<void>::resume(void) const
    #4 colib::pool_internal_t::run(void)
freed by thread T0 here:
    #0 operator delete(void *, unsigned __int64)
    #3 std::coroutine_handle<void>::destroy(void) const
    #4 colib::destroy_state(struct colib::state_t *, struct colib::state_t *)
    #5 colib::killer_state_t::end_chain(void)
    #6 colib::killer_state_t::kill(void)
    #11 `main'::`2'::<lambda_1>::operator()                  43-next_task_window.cpp:76
    #15 colib::do_generic_modifs<1>(struct colib::state_t *)
    #16 colib::do_sched_modifs(struct colib::state_t *)
    #17 colib::pool_internal_t::_take_thread_tasks(void)
    #18 colib::pool_internal_t::next_task_state(void)
    #19 colib::pool_internal_t::next_task(void)
    #20 colib::yield_awaiter_t::await_suspend<struct colib::task_state_t<int>>
exit 1
```

The semaphore variant fails the same way, with `sem_awaiter_t::await_suspend` in place of the
yield. The other routes, each run by the review on a copy of the base header (programs and logs
under `repro/fix/43/noop/wf-*`), all clean on the patched header:

| route                              | on the base header                       | where            |
|------------------------------------|------------------------------------------|------------------|
| an APC kills X while the pool      | ASan: `WRITE of size 8` in `next_task()` | A: a5            |
| waits for io, X waiting on a       | under `sem_awaiter_t::await_suspend`,    |                  |
| semaphore                          | freed by the kill in the APC, under      |                  |
|                                    | `handle_ready()`                         |                  |
| a SCHED callback signals X's       | ASan: the same write; X went past its    | C: c09; the      |
| semaphore, then kills X: the kill  | wait inside its own `await_suspend` and  | test's driven    |
| runs X on                          | was freed at its next wait               | case             |
| a SCHED callback clears the        | ASan: the same write; the frame was      | V: v1; I:        |
| semaphore X waits on: `clear()`,   | freed by `sem_internal_t::clear()`       | 900-002          |
| or dropping its last `sem_p`       |                                          |                  |
| a kill runs X into an awaitable    | `std::terminate()` at the kill's         | A: a3; C: c08;   |
| that returns                       | placeholder, in a plain build too        | the test's hold  |
| `external_wait_next_task()`        |                                          | case             |
| a SCHED callback resumes a held    | `std::terminate()` in                    | A: a4            |
| coroutine, which yields            | `state_list_t::link()`: the same         |                  |
|                                    | handover queued twice                    |                  |

A is `wf-review-A-resumers`, C is `wf-review-C-semantics`, I is `wf-inter-27-14-07`, and V is
`wf-verify`, all under `repro/fix/43/noop/`.

## The rule

A suspension ends before the scheduler runs. The awaiters return `std::noop_coroutine()` once X is
where it waits: X's `await_suspend` returns, its suspension is over, and control goes back to the
`resume()` call that started the current chain of coroutines, normally the one in
`pool_internal_t::run()`. Its loop already calls `next_task_state()` and resumes the coroutine that
it picks. `next_task_state()`, and the user code it runs, so runs only there, between two resumes,
where no `await_suspend` is on the stack and a coroutine ended by that code leaves nothing in use
behind. The alertable io wait, where the APCs run, is called only from `next_task_state()`, so the
APC route closes with it. `final_awaiter_ends()` follows the same rule for a coroutine without a
caller: that coroutine doesn't suspend at its end, so its frame is freed after it runs off its end,
never inside a handle-returning `await_suspend`.

One route stays open: `external_has_next_task()` also runs the SCHED callbacks of the coroutines
handed over with `thread_sched()`, wherever it is called (open question 2).

`next_task()` loses its last callers and goes.

The diff holds to DESIGN.md. It changes no code of the killer, and nothing in the library learns
about the killer (2). A kill still ends its target at a wait or throws (1, 3); one that runs its
target into an external awaitable's Option 3 now ends it at that hold, where the base ran the
scheduler inside the kill.

What a caller can observe:

- **For a chain that `run()` resumed, the order stays the same.** The same `next_task_state()`
  picks from the same queues, one return later. The review traced every path the diff changes (a
  yield, a semaphore, an io, a `co_yield` without a caller), `force_stop()` followed by a second
  `run()`, and a `run()` called from a coroutine: the order printed on both headers is the same,
  with MSVC and with GCC.
- **A wait on another pool's semaphore no longer runs that pool's coroutines.** `sem_awaiter_t`
  returned the next task of the semaphore's pool, so pool B's coroutines ran inside
  `pool_a->run()`; now the run that resumed the waiter goes on. That was the suspension half of
  FIX_27's bug (see Interactions).
- **A coroutine resumed by something other than `run()`** gives control back to that code at its
  next colib wait, instead of running the pool's next coroutine inside it. Inside colib that is the
  killer's `drive()`: its own modifs park the target at its colib waits, as before, and an external
  awaitable's Option 3 now returns to `drive()` too, which ends the target at the hold. Outside
  colib it is user code that resumes a coroutine itself: an APC, a callback, the "Task
  Initialization" example (open questions 3 and 4).
- **A posted exception or stop is seen at the next wait.** `run()` checks them each time a resume
  returns. When `run()` resumed the code that posted one, nothing changes. When `drive()` or user
  code resumed it (a root without a caller that ends with an exception inside a kill, a
  `force_stop()` in a coroutine that user code resumed), the base kept switching through
  `next_task()` before `run()` looked, and other coroutines ran first; now the next wait returns to
  `run()`, which rethrows or stops at once. A second exception from one of those coroutines used to
  end the program, since `post_exception()` terminates on a second one in the same resume; now each
  reaches its own `run()`. Two ends without a caller inside one resume from `run()` itself still end
  the program, on both headers (a coroutine that runs a root, through its kill, to a throwing end,
  and then throws out of its own end). The `run_e` values don't change.
- **`external_wait_next_task()` neither blocks nor returns a task**: it returns
  `std::noop_coroutine()`. Returned from an `await_suspend`, as its docs show, it still lets the
  pool go on with its other coroutines (open question 1).
- **A failed io poll ends `run()` at once.** Before, a poll that failed inside an awaiter's
  `next_task()` stored `RUN_ERRORED` and gave control back to `run()`, which polled again, and a
  failure that didn't repeat was absorbed. Now `run()` makes every poll, and its first failed poll
  ends the run, reported as `RUN_OK` until FIX_09 reports it as `RUN_ERRORED` (see Interactions).
  The IOCP and epoll backends retry their transient failures themselves (an APC, `EINTR`), so on
  them the visible difference is one "Failed io pool" line in the log instead of two.
- **A switch through a wait costs one return and one call more** than the transfer it replaces.
  The review measured no difference beyond about 2 ns per switch with `/O2` or GCC's `-O2`, and
  about 15 to 25 ns in the tests' unoptimized MSVC build (`repro/fix/43/noop/wf-verify/bench-*.log`,
  on a quiet machine; a first measure, taken while the suite ran, read higher).
- **A long run of waits no longer grows the stack** where the compiler doesn't turn the switch into
  a jump. Calls, returns and a `co_yield` to a caller still switch by symmetric transfer, so a long
  run of those still needs the jump. Under GCC with ASan, without `--param=asan-stack=0`, `001-001`
  overflowed its stack on the base header and passes on the patched one, while a long run of calls
  without a wait overflows on both. The header's note on compiling says so now.

## Where the fix lands

| `colib.h` region                     | what changes                                   | +/-      |
|--------------------------------------|------------------------------------------------|----------|
| the note on compiling, in the file's | calls, returns and a `co_yield` to a caller    | +6 -4    |
| header                               | transfer; a wait goes back to `pool_t::run()`  |          |
| `external_has_next_task()` doc       | drops the two lines about                      | +2 -2    |
|                                      | `external_wait_next_task()`                    |          |
| `external_wait_next_task()` doc      | it returns `std::noop_coroutine()` and doesn't | +7 -6    |
|                                      | block                                          |          |
| `pool_internal_t::next_task_state()` | a comment: the user code it runs, and why only | +5 -0    |
|                                      | `run()` calls it                               |          |
| `pool_internal_t::next_task()`       | removed: nothing calls it                      | +0 -5    |
| `cpp_yield_awaiter()`                | without a caller, it returns noop              | +1 -4    |
| `external_wait_next_task()`          | returns noop                                   | +2 -1    |
| `yield_awaiter_t::await_suspend()`   | returns noop; the TODO about it goes           | +1 -3    |
| `io_awaiter_t::await_suspend()`      | returns noop                                   | +1 -1    |
| `sem_awaiter_t::await_suspend()`     | returns noop; its `pool` local goes            | +1 -3    |
| total                                |                                                | +26 -29  |

## The change

The diff is `diff -u` of a copy of the working tree's header (`repro/fix/43/noop/base2/colib.h`)
against the patched copy (`repro/fix/43/noop/final2/colib.h`). The runs below were made on copies
with the same code: the first patched copy (`patched/`) differs from `final2` only in comments,
and the first base copy (`base/`) from `base2` only in the doc comments rewritten at 02:03. Both
were checked with `repro/fix/43/noop/code_identity.py`, which compares the headers with their
comments stripped. The reasoning, per hunk:

- **The note on compiling.** It said that every switch is a symmetric transfer. Now only calls,
  returns and a `co_yield` to a caller are, and a wait goes back to `pool_t::run()`. Its advice
  stays, for long runs of calls.
- **`external_has_next_task()` doc.** Its two usage lines promised what `external_wait_next_task()`
  no longer does (to block, to return a task). It now says only what it answers.
- **`external_wait_next_task()` doc.** It says what the function now does: control goes back to the
  `resume()` call that started the current chain of coroutines, normally `run()`'s. The parameter
  stays, so callers still compile (open question 1).
- **`next_task_state()` comment.** It names the user code the scheduler runs, and why only `run()`
  may call it, so that no awaiter calls it again. It says "modif callbacks, such as the SCHED
  callbacks", so it stays true when FIX_16 adds EXIT callbacks there and FIX_19 ENTER callbacks.
- **`next_task()`** is removed: after this diff nothing calls it.
- **`cpp_yield_awaiter()`.** Without a caller, the generator's suspension ends and `run()` goes on.
  That was already the answer when a RETURN modif took the generator over (`ERROR_SUSPENDED`), so
  the two cases became one return.
- **`external_wait_next_task()`** returns `std::noop_coroutine()`; `pool` has no use left.
- **`yield_awaiter_t::await_suspend()`.** X is in the ready queue, the awaiter returns noop, and
  `run()` resumes the first ready coroutine, X itself when it is alone. The TODO went with the call
  it was about: it asked whether returning X's own handle needed the ENTER callbacks run by hand,
  and the awaiter no longer returns X's handle (`await_resume()` runs UNYIELD and ENTER either way).
- **`io_awaiter_t::await_suspend()`** returns noop once X waits on its io.
- **`sem_awaiter_t::await_suspend()`** returns noop once X is on the wait list. Its `pool` local
  had no other use; it was the semaphore's pool, which is how a wait on another pool's semaphore ran
  that pool's scheduler. `sem_internal_t::get_pool()` is left without a caller, and FIX_27's ruling
  decides it (open question 5).

```diff
--- colib.h (working tree, 2026-10-13 02:04)
+++ colib.h (patched)
@@ -36,10 +36,12 @@
  * Compiling
  * ===============
  * 
- * C++20. The coroutines switch by symmetric transfer, which only stays on a bounded stack when the
- * compiler turns each switch into a jump. GCC does that only with -foptimize-sibling-calls (on from
- * -O2): add it at -O0, -O1 or -Og, else a program that switches for long enough overflows its
- * stack. GCC's AddressSanitizer blocks the same jump; add --param=asan-stack=0. MSVC always jumps.
+ * C++20. A call, a return and a co_yield to a caller switch coroutines by symmetric transfer, which
+ * only stays on a bounded stack when the compiler turns each switch into a jump; a wait (a yield, a
+ * semaphore, an io) goes back to pool_t::run() instead. GCC makes the jump only with
+ * -foptimize-sibling-calls (on from -O2): add it at -O0, -O1 or -Og, else a program that calls and
+ * returns for long enough without a wait overflows its stack. GCC's AddressSanitizer blocks the
+ * same jump; add --param=asan-stack=0. MSVC always jumps.
  * 
  * On Linux, colib waits until an fd is ready and then makes the plain syscall. Reads and small
  * writes are fine on a blocking fd, but ready to write only means some room (EPOLLOUT: a little of
@@ -1425,8 +1427,8 @@
  * Checks whether the library has any ready-to-run tasks.
  * 
  * Usage:
- * - Use this before calling external_wait_next_task if you want to avoid blocking.
- * - Returns true if calling external_wait_next_task will immediately return a task; false otherwise.
+ * - Returns true if the pool has a coroutine ready to run now, without waiting on io; false
+ *   otherwise.
  * 
  * @param pool The pool for which to query the ready state
  * @return True if a task is available for immediate execution
@@ -1434,16 +1436,17 @@
 inline bool external_has_next_task(pool_t *pool);
 
 /*! @fn
- * Retrieves the next coroutine that has work to perform. If no task is ready, this function
- * blocks until a coroutine becomes runnable (e.g., an IO operation completes or another
- * external awaitable resumes a coroutine).
+ * Returns std::noop_coroutine(): an await_suspend that returns it ends the suspension there, and
+ * control goes back to the resume() call that started the current chain of coroutines, normally
+ * the one in pool_t::run(), which then resumes the next ready coroutine. It neither blocks nor
+ * resumes a coroutine itself.
  * 
  * Usage:
  * - Typically used in external awaitables to continue processing library-managed coroutines.
  * 
- * @param pool The pool from which to fetch the next ready coroutine (obs: each valid state_t has
- * to have a valid pool_t*)
- * @return Coroutine handle for the next task to run
+ * @param pool The pool of the suspending coroutine (obs: each valid state_t has to have a valid
+ * pool_t*)
+ * @return std::noop_coroutine(), for the await_suspend to return
  */
 inline std::coroutine_handle<void> external_wait_next_task(pool_t *pool);
 
@@ -4252,6 +4255,11 @@
         return ready_tasks.size() > 0;
     }
 
+    /* The scheduler runs user code: modif callbacks, such as the SCHED callbacks of what
+    thread_sched() handed over, and on Windows the APCs of its alertable io wait. That code can end
+    any coroutine, so only run() calls this, between two resumes. An await_suspend must not: the
+    coroutine it suspends could be ended there, and MSVC writes the handle await_suspend returns
+    into that coroutine's frame. */
     state_t *next_task_state() {
         COLIB_DEBUG_TRACE_SCOPE("");
 #if COLIB_ENABLE_MULTITHREAD_SCHED
@@ -4278,11 +4286,6 @@
         return nullptr;
     }
 
-    handle<void> next_task() {
-        state_t *ns = next_task_state();
-        return ns ? ns->self : std::noop_coroutine();
-    }
-
     template <typename P>
     error_e wait_io(handle<P> h, const io_desc_t& io_desc) {
         /* add the file descriptor inside epoll and schedule the corutine for waiting on it */
@@ -4422,10 +4425,7 @@
             return std::noop_coroutine();   /* a modif resumes the caller */
         return caller_state->self;
     }
-    if (exit_err == ERROR_SUSPENDED)
-        return std::noop_coroutine();       /* a modif took over */
-
-    return yielding_task_state->pool->get_internal()->next_task();
+    return std::noop_coroutine();   /* no caller: pool_t::run() goes on (a modif may own it now) */
 }
 
 /* true: it has no caller, so it doesn't suspend at its end and the coroutine frees itself (nobody
@@ -4541,7 +4541,8 @@
 }
 
 inline handle<void> external_wait_next_task(pool_t *pool) {
-    return pool->get_internal()->next_task();
+    (void)pool;
+    return std::noop_coroutine();   /* suspended: pool_t::run() resumes the next task */
 }
 
 inline state_t *external_init_task(state_t *state, pool_t *pool) {
@@ -4588,9 +4589,7 @@
 
         pool->get_internal()->push_ready(state);
         triggered = true;
-        // TODO: is it required to call the modifs here if the returned coroutine is the same as
-        // this one or does c++ call resume either way?
-        return pool->get_internal()->next_task();
+        return std::noop_coroutine();   /* suspended: pool_t::run() resumes the next task */
     }
 
     void await_resume() {
@@ -4713,7 +4712,7 @@
         }
         state_access_t::set(state, STATE_WAITING_IO);
         triggered = true;
-        return pool->get_internal()->next_task();
+        return std::noop_coroutine();   /* suspended: pool_t::run() resumes the next task */
     }
 
     error_e await_resume() {
@@ -4889,8 +4888,6 @@
 
         state = &to_suspend.promise().state;
 
-        auto pool = sem->get_internal()->get_pool();
-
         do_leave_modifs(state);
         error_e err = do_wait_sem_modifs(state, sem);
         if (err != ERROR_OK) {
@@ -4907,7 +4904,7 @@
         if (sem->get_internal()->await_ready())
             return to_suspend;  /* signaled from its own callbacks: it takes the token now */
         sem->get_internal()->push_waiter(state);    /* only now: until here it was running */
-        return pool->get_internal()->next_task();
+        return std::noop_coroutine();   /* suspended: pool_t::run() resumes the next task */
     }
 
     sem_t::unlocker_t await_resume() {
```

## The regression test

`tests/018-038-reproduced_next_task_window.cpp` (a placeholder number, assigned at apply time). It
promotes repro/43 to the suite, with one case for each place the diff changes and one for each
form of the failure: a yield, a semaphore wait, an io wait (a sleep), a `co_yield` without a
caller, an external awaitable that queues the coroutine again and returns
`external_wait_next_task()`, a kill that runs the victim on from inside its own suspension (the
"driven" case), and a kill that runs it into an awaitable that holds it and returns
`external_wait_next_task()` (the "hold" case).

- **One thread.** The victim calls `thread_sched()` from the pool's own thread, as `004-006` does
  from a SCHED callback, so the pool takes T in at its next scheduling pass. That pass is inside the
  victim's `await_suspend` on the unfixed header and in `run()` on the fixed one; no helper thread
  and no atomics are needed to order it. The hold case needs no T: a controller coroutine gives the
  victim its token and kills it.
- **The end.** T's SCHED callback ends the victim with its kill, or with `destroy_state()` for the
  generator (a killer can't be attached to one; its holder destroys it, as in `018-020`). In the
  driven case it signals the victim's semaphore first, so the kill runs the victim on.
- **The checks.** The end ran while the victim was alive and returned `ERROR_OK`; the victim's frame
  was destroyed once (a marker parameter) and it never went past the suspension where it dies; the
  driven victims got their token; the holding awaitable was destroyed while it held; T ran; and no
  freed block was written.
- **The quarantine.** A plain MSVC build goes on silently after the write, so the test replaces the
  global `operator new` and `operator delete`, as `018-018` does: a freed block is poisoned with
  `0xdd` and kept, and after each case every kept block is checked. Under ASan
  (`__SANITIZE_ADDRESS__`) the replacement is left out, and ASan's own quarantine does that job.
- **A terminate handler** reports a `std::terminate()` (the unfixed header's end in the hold case)
  as `[FAILED]`, instead of leaving it to the system's error reporting.

On the two headers, built through the tests makefile (`repro/fix/43/noop/t-*`, `lx-*`):

| header  | compiler | build | result                                                             |
|---------|----------|-------|--------------------------------------------------------------------|
| base    | MSVC     | plain | the yield case:                                                    |
|         |          |       | `a freed block of 928 bytes was written at offset 208`,            |
|         |          |       | `[FAILED]`, exit -1                                                |
| base    | MSVC     | ASan  | `heap-use-after-free`, `WRITE of size 8` in `next_task()` under    |
|         |          |       | `yield_awaiter_t::await_suspend`, exit 1                           |
| base    | GCC      | plain | the first six cases pass (GCC doesn't write into the frame); the   |
|         |          |       | hold case: `std::terminate() in the hold case`, `[FAILED]`, exit 1 |
| base    | GCC      | ASan  | the same as plain                                                  |
| patched | MSVC     | plain | all seven cases end as expected, `[PASSED]`, exit 0                |
| patched | MSVC     | ASan  | the same                                                           |
| patched | GCC      | plain | the same                                                           |
| patched | GCC      | ASan  | the same                                                           |

The test stops at its first failing case, so each case was also built alone on the base header
(`repro/fix/43/noop/t-base-cases`, `t-base-asan-cases`). Each one fails, at its own place:

| case      | plain: the quarantine finds            | ASan: `WRITE of size 8` in `next_task()`  |
|           |                                        | under                                     |
|-----------|----------------------------------------|-------------------------------------------|
| yield     | a 928-byte block written at offset 208 | `yield_awaiter_t::await_suspend`          |
| sem       | a 928-byte block written at offset 264 | `sem_awaiter_t::await_suspend`            |
| io        | a 1136-byte block written at offset    | `io_awaiter_t::await_suspend`             |
|           | 640                                    |                                           |
| generator | a 352-byte block written at offset 200 | `cpp_yield_awaiter`                       |
| external  | a 928-byte block written at offset 360 | `external_wait_next_task`                 |
| driven    | a 928-byte block written at offset 416 | `sem_awaiter_t::await_suspend` (the kill  |
|           |                                        | ran the victim on inside it)              |
| hold      | (none)                                 | (no write) the same `std::terminate()`,   |
|           | `std::terminate() in the hold case`,   | exit 1                                    |
|           | exit 1                                 |                                           |

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#include <cstring>
#include <exception>

/* Test92 - Reproduced Bugs: the scheduler ran inside a suspending coroutine's await_suspend
================================================================================================= */

/* Was an open bug (repro/43): the yield, semaphore and io awaiters, a co_yield without a caller and
external_wait_next_task() queued the suspending coroutine, then ran the scheduler
(pool_internal_t::next_task()) inside its await_suspend. The scheduler runs user code, here the
SCHED callback of a coroutine handed over with thread_sched(). A kill from that callback found the
suspending coroutine in a colib queue and freed its frame, or, when its wait had already done its
work, ran it on to its next wait and freed it there. Either way the await_suspend still running on
that frame then wrote the next coroutine's handle into it (MSVC keeps the handle an await_suspend
returns in the coroutine's frame): an ASan heap-use-after-free. A kill that ran its target into an
external awaitable returning external_wait_next_task() ran the scheduler inside the kill itself,
and popped the kill's placeholder: std::terminate(). The awaiters now return
std::noop_coroutine(): the suspension ends first, and pool_t::run() runs the scheduler between two
resumes.

Each case runs on its own pool, and the victim carries a killer. In the first six, the victim hands
T over with thread_sched() from the pool's own thread, so the pool takes T in at its next
scheduling pass, then the victim suspends: a yield, a semaphore wait, an io wait (a sleep), a
co_yield without a caller, an external awaitable that queues it again and returns
external_wait_next_task(), or a semaphore wait whose semaphore T signals. T's SCHED callback ends
the victim: its kill, or destroy_state() for the generator, which a killer can't be attached to.
In the last case a controller signals the victim's semaphore and kills it, and the kill runs the
victim into an awaitable that holds it and returns external_wait_next_task(). The victim must die
once, at its suspension or at the next one for the two driven cases, and T must run after it. A
plain build doesn't notice a write into freed memory, so this test replaces the global operator
new and delete: a freed block is kept, poisoned, and checked after each case. Under ASan, ASan
does that check itself. */

static const char *test92_file = "018-038-reproduced_next_task_window.cpp";

/* ---------------------------------------------------------------- quarantine allocator */

#if defined(__SANITIZE_ADDRESS__)

static bool test92_quarantine_clean() { return true; }  /* ASan reports a use after free itself */

#else /* __SANITIZE_ADDRESS__ */

/* a block's header: it keeps the blocks 16-byte aligned */
struct test92_block_t {
    test92_block_t *next;           /* the freed block before it */
    size_t n;                       /* the size the user asked for */
};
static_assert(sizeof(test92_block_t) == 16, "the header keeps the blocks 16-byte aligned");

static test92_block_t *test92_freed = nullptr;  /* only this test's one thread allocates */

void *operator new(size_t n) {
    test92_block_t *b = (test92_block_t *)malloc(sizeof(test92_block_t) + n);
    if (!b)
        throw std::bad_alloc();
    b->n = n;
    return b + 1;
}

/* the block is poisoned and kept, never given back: a later write into it changes its bytes */
void operator delete(void *ptr) noexcept {
    if (!ptr)
        return;
    test92_block_t *b = (test92_block_t *)ptr - 1;
    memset(ptr, 0xdd, b->n);
    b->next = test92_freed;
    test92_freed = b;
}

void operator delete(void *ptr, size_t) noexcept {
    operator delete(ptr);
}

/* true when no freed block was written since it was freed */
static bool test92_quarantine_clean() {
    for (test92_block_t *b = test92_freed; b; b = b->next) {
        unsigned char *p = (unsigned char *)(b + 1);
        for (size_t i = 0; i < b->n; i++) {
            if (p[i] != 0xdd) {
                DBG("a freed block of %zu bytes was written at offset %zu", b->n, i);
                return false;
            }
        }
    }
    return true;
}

#endif /* __SANITIZE_ADDRESS__ */

/* ---------------------------------------------------------------- the test */

enum test92_way_e {
    TEST92_YIELD,       /* co::yield() */
    TEST92_SEM,         /* a semaphore nobody signals */
    TEST92_IO,          /* a sleep */
    TEST92_GENERATOR,   /* a co_yield without a caller */
    TEST92_EXTERNAL,    /* an awaitable that queues it again, then external_wait_next_task() */
    TEST92_DRIVEN,      /* a semaphore T signals before the kill: the kill runs it to a yield */
    TEST92_HOLD,        /* its kill runs it into a hold that returns external_wait_next_task() */
};

static const char *test92_way_name[] = {
    "yield", "sem", "io", "generator", "external", "driven", "hold"
};

static test92_way_e test92_way = TEST92_YIELD;
static co::pool_t *test92_pool = nullptr;
static co::sem_p test92_sem;
static co::task_t test92_t;             /* T: the victim hands it over, its SCHED ends the victim */
static int test92_destructed = 0;
static int test92_hold_cancelled = 0;   /* the holding awaitable was destroyed while it held */
static bool test92_got_token = false;   /* a driven victim went past its semaphore wait */
static bool test92_past = false;        /* the victim went past the suspension where it dies */
static bool test92_t_ran = false;
static bool test92_victim_alive = false;
static bool test92_ended_while_alive = false;   /* its end found the victim alive */
static co::error_e test92_end_ret = co::ERROR_GENERIC;

/* a parameter: its copy in the frame lives until the frame is freed (a local dies at co_return) */
struct test92_marker_t {
    bool live = false;
    test92_marker_t() {}
    test92_marker_t(test92_marker_t &&oth) : live(true) { oth.live = false; }
    ~test92_marker_t() {
        if (live) {
            test92_destructed++;
            test92_victim_alive = false;
        }
    }
};

/* queues the coroutine again at once, then lets the pool go on (Option 3 of the external
awaitables) */
struct test92_requeue_t {
    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        co::state_t *s = co::external_on_suspend(h);
        co::external_sched_resume(s);
        return co::external_wait_next_task(s->pool);
    }

    void await_resume() {}
};

/* holds the coroutine, then lets the pool go on (Option 3); destroyed while it holds, it cancels
what it started, here nothing */
struct test92_hold_t {
    bool holding = false;

    ~test92_hold_t() {
        if (holding)
            test92_hold_cancelled++;
    }

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        holding = true;
        co::state_t *s = co::external_on_suspend(h);
        return co::external_wait_next_task(s->pool);
    }

    void await_resume() { holding = false; }
};

static co::task_t test92_thread_task() {
    test92_t_ran = true;
    co_return 0;
}

static co::task_t test92_victim(test92_way_e way, test92_marker_t) {
    test92_victim_alive = true;
    if (way != TEST92_HOLD)
        test92_pool->thread_sched(test92_t);    /* taken in at the pool's next scheduling pass */
    if (way == TEST92_YIELD) {
        co_await co::yield();
    }
    else if (way == TEST92_SEM) {
        co_await test92_sem->wait();
    }
    else if (way == TEST92_IO) {
        co_await co::sleep_ms(2000);
    }
    else if (way == TEST92_EXTERNAL) {
        co_await test92_requeue_t{};
    }
    else {
        co_await test92_sem->wait();    /* signaled, then killed: the kill runs it on from here */
        test92_got_token = true;
        if (way == TEST92_DRIVEN)
            co_await co::yield();       /* its next wait, where the kill ends it */
        else
            co_await test92_hold_t{};   /* held: the kill ends it here */
    }
    test92_past = true;
    co_return 0;
}

static co::task_t test92_generator(test92_marker_t) {
    test92_victim_alive = true;
    test92_pool->thread_sched(test92_t);
    co_yield 1;                             /* no caller to go back to */
    test92_past = true;
    co_return 0;
}

/* the hold case's end: the victim waits on the semaphore, it gets its token, then its kill */
static co::task_t test92_controller(std::function<co::error_e(void)> kill_fn) {
    test92_sem->signal();                   /* the victim is queued with its token */
    test92_ended_while_alive = test92_victim_alive;
    test92_end_ret = kill_fn();             /* its wait did its work: the kill runs it on */
    co_return 0;
}

static int test92_case(test92_way_e way) {
    DBG("case: %s", test92_way_name[way]);
    test92_way = way;
    test92_destructed = 0;
    test92_hold_cancelled = 0;
    test92_got_token = false;
    test92_past = false;
    test92_t_ran = false;
    test92_victim_alive = false;
    test92_ended_while_alive = false;
    test92_end_ret = co::ERROR_GENERIC;

    auto pool = co::create_pool();
    test92_pool = pool.get();
    test92_sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    co::task_t victim;
    co::state_t *generator = nullptr;
    if (way == TEST92_GENERATOR) {
        victim = test92_generator(test92_marker_t{});
        generator = victim.get_state();
    }
    else {
        victim = co::add_modifs(pool.get(), test92_victim(way, test92_marker_t{}), killer_mods);
    }

    auto end = co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
            [&kill_fn, generator, way](co::state_t *) -> co::error_e {
                test92_ended_while_alive = test92_victim_alive;
                if (generator) {
                    co::destroy_state(generator);   /* its holder ends it */
                    test92_end_ret = co::ERROR_OK;
                    return co::ERROR_OK;
                }
                if (way == TEST92_DRIVEN)
                    test92_sem->signal();           /* the wait did its work: the kill runs it */
                test92_end_ret = kill_fn();
                return co::ERROR_OK;
            });
    if (way != TEST92_HOLD)
        test92_t = co::add_modifs(pool.get(), test92_thread_task(), co::modif_pack_t(1, end));

    pool->sched(COLIB_REGNAME(victim));
    if (way == TEST92_HOLD)
        pool->sched(test92_controller(kill_fn));
    ASSERT_FN(pool->run());
    test92_sem = nullptr;

    DBG("%s: end %s, alive at the end %d, destructed %d, token %d, past %d, cancelled %d, "
            "T ran %d", test92_way_name[way], co::dbg_enum(test92_end_ret).c_str(),
            (int)test92_ended_while_alive, test92_destructed, (int)test92_got_token,
            (int)test92_past, test92_hold_cancelled, (int)test92_t_ran);
    ASSERT_FN(CHK_BOOL(test92_ended_while_alive && test92_end_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test92_destructed == 1 && !test92_past));
    bool driven = way == TEST92_DRIVEN || way == TEST92_HOLD;
    ASSERT_FN(CHK_BOOL(test92_got_token == driven));
    ASSERT_FN(CHK_BOOL(test92_hold_cancelled == (way == TEST92_HOLD ? 1 : 0)));
    ASSERT_FN(CHK_BOOL(test92_t_ran == (way != TEST92_HOLD)));
    ASSERT_FN(CHK_BOOL(test92_quarantine_clean()));
    return 0;
}

int test92_next_task_window() {
    ASSERT_FN(test92_case(TEST92_YIELD));
    ASSERT_FN(test92_case(TEST92_SEM));
    ASSERT_FN(test92_case(TEST92_IO));
    ASSERT_FN(test92_case(TEST92_GENERATOR));
    ASSERT_FN(test92_case(TEST92_EXTERNAL));
    ASSERT_FN(test92_case(TEST92_DRIVEN));
    ASSERT_FN(test92_case(TEST92_HOLD));
    return 0;
}

int main() {
    /* the unfixed header ends the hold case in std::terminate(): a failure, not an error dialog */
    std::set_terminate([] {
        DBG("std::terminate() in the %s case", test92_way_name[test92_way]);
        print_test_result(test92_file, false);
        std::_Exit(1);
    });
    int ret = test92_next_task_window();
    print_test_result(test92_file, ret >= 0);
    return ret;
}
```

## How it was checked

The Windows runs: Windows 10, MSVC 19.43, built through the tests makefile on scratch copies under
`repro/fix/43/noop/`. The Linux runs: WSL Ubuntu, GCC 15.2, the epoll backend, through the same
makefile (it dispatches to `linux.makefile`). ASan means
`CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /I.. /fsanitize=address'` with MSVC (with `/FS` for
the parallel builds) and `-std=c++2a -O2 -g -fsanitize=address,undefined --param=asan-stack=0` with
GCC.

**The repro** (`repro/fix/43/noop/{base,patched,lx-base,lx-patched}/repro/`):

| header  | compiler | program             | exit | result                                        |
|---------|----------|---------------------|------|-----------------------------------------------|
| base    | MSVC     | 43-next_task_window | 1    | `heap-use-after-free`, `WRITE of size 8` in   |
|         |          |                     |      | `next_task()` under                           |
|         |          |                     |      | `yield_awaiter_t::await_suspend`              |
| base    | MSVC     | 43-sem-variant      | 1    | the same, under                               |
|         |          |                     |      | `sem_awaiter_t::await_suspend`                |
| patched | MSVC     | 43-next_task_window | 0    | `run() = 0`, `killed_in_sched = 1`,           |
|         |          |                     |      | `x_resumed_past_yield = 0`, `PASS`            |
| patched | MSVC     | 43-sem-variant      | 0    | `run()=0 kill_ret=0 destruct=1 x_past_wait=0` |
| base    | GCC      | both                | 0    | their pass lines: GCC doesn't write into the  |
|         |          |                     |      | frame                                         |
| patched | GCC      | both                | 0    | the same                                      |

**The regression test**: see its section above.

**The whole suite** (`tests/` as it is, plus the regression test where the table says so):

| header  | build      | files                                      | result                      |
|---------|------------|--------------------------------------------|-----------------------------|
| patched | MSVC plain | 91, `tests/` as it is                      | `[PASSED: 91]/[FAILED: 0]`  |
| patched | MSVC ASan  | 91, `tests/` as it is                      | `[PASSED: 91]/[FAILED: 0]`  |
| patched | MSVC plain | 92, with the test's first version (5       | `[PASSED: 92]/[FAILED: 0]`  |
|         |            | cases)                                     |                             |
| patched | MSVC ASan  | 92, with the test's first version (5       | `[PASSED: 92]/[FAILED: 0]`  |
|         |            | cases)                                     |                             |
| final   | MSVC plain | 92, with the test (7 cases)                | `[PASSED: 92]/[FAILED: 0]`  |
| final   | MSVC ASan  | 92, with the test (7 cases)                | `[PASSED: 92]/[FAILED: 0]`  |
| base    | GCC -O3    | 91: no `018-029`, the test's first version | `[PASSED: 91]/[FAILED: 0]`  |
| patched | GCC -O3    | the same 91                                | `[PASSED: 91]/[FAILED: 0]`  |

`patched` and `final` have the same code (`final` differs in comments only); `base` is the
working tree's header without this diff.

- The parallel builds (`make -j`) hit `C1041` on the shared `vc140.pdb` even with `/FS`; `make`'s
  run phase then rebuilt those targets one by one, so every count above is of built and run files.
- One pass of the 92-file suite, run while the Linux suite built in parallel, failed
  `003-002-sleep_duration` in both builds: its sleeps woke 22 to 65 ms late, against a margin of
  20 ms. On a quiet machine it passed 5 runs of 5 on each header, plain, and 3 of 3 with ASan, and
  the next pass of the whole suite passed.
- `018-029-reproduced_winevent_handle_leak` doesn't build on Linux, on either header: its watchdog
  function is defined only under `COLIB_OS_WINDOWS`, and `main()` calls it unconditionally. It is a
  test bug, not this fix's, so the Linux suite ran without it.

**The review**: a workflow of three reviewers (the other resumers, the remaining windows, the run
loop's behavior), four agents on the pending fixes, and a skeptic over every finding. Their
programs and logs are under `repro/fix/43/noop/wf-*`, cited where they are used above.

**NOT-CHECKED.**

- The kqueue backend: not built here. This diff touches no backend code.
- Clang: not installed here.
- The suite with GCC ran with the regression test's first version (five cases); its final version
  ran alone with GCC, plain and with ASan, on both headers.
- The socket and named-pipe tests of the pending fixes (FIX_09's, FIX_12's) were built during the
  review, not run, since the suite ran at the same time on the same ports. Their logic was checked
  with socket-free stand-ins where the interaction needed it.

## Interactions

The pending fixes whose text names the code this diff touches, each checked against it on scratch
copies (`repro/fix/43/noop/wf-inter-*`). Every pair applies in either order to the same header,
except FIX_27, which shares one line with this diff.

Three of them open a route into this bug's window on their own, and this diff closes each one:

- **FIX_19, ENTER after `external_sched_resume()`.** No line is touched by both. FIX_19 runs the
  ENTER callbacks of a coroutine queued by `external_sched_resume()` at its pop in
  `next_task_state()`. Without this diff that pop also runs inside a suspending coroutine's
  `await_suspend`, so FIX_19 alone opens a route that needs one thread and no
  `COLIB_ENABLE_MULTITHREAD_SCHED`: X queues F with `external_sched_resume()` and yields, F's ENTER
  callback at the pop kills X, and X is freed under its own yield. Confirmed under ASan on base +
  FIX_19 (`WRITE of size 8` in `next_task()` under `yield_awaiter_t::await_suspend`); clean on this
  diff + FIX_19. So this diff goes first. FIX_19 needs re-basing either way (two of its hunks meet
  FIX_47's lines, a third edits a block gone since 9a13a9a). Its test passes on this diff + FIX_19,
  plain and ASan, but its scenario C describes a pop inside another coroutine's
  `external_wait_next_task()`, which no longer happens: the pop is `run()`'s. Its assertions stay;
  its comments change.
- **FIX_16, a refused handover is destroyed.** No line is touched by both. FIX_16 makes
  `_take_thread_tasks()` destroy a handover that a SCHED callback refuses, which runs its EXIT
  callbacks inside the scheduler. Without this diff, an EXIT callback that kills the yielding X frees
  it under its own yield, confirmed under ASan on base + FIX_16; clean on this diff + FIX_16. So this
  diff goes first, or with it (FIX_16 itself wants FIX_08 first or with it). With both, open
  question 2 grows: `external_has_next_task()` would run those EXIT callbacks too. Both tests pass on
  the combined header, plain and ASan, FIX_16's also with `COLIB_ENABLE_MULTITHREAD_SCHED`.
- **FIX_12, a pipe that was already connected.** No line is touched by both. Its new branch in the
  IOCP `add_waiter()` queues X from inside `io_awaiter_t::await_suspend()`, which then takes the
  tail this diff rewrites, so `run()` resumes X one return later. By reading, FIX_12 alone in a
  `COLIB_ENABLE_MULTITHREAD_SCHED` build opens a route into this bug (a kill from a SCHED callback
  finds that io finished and drives X inside its own `await_suspend`); this diff closes it. One
  sentence of FIX_12's md names `next_task()` and gets reworded. Its test uses named pipes and was
  built, not run.

The others:

- **FIX_27, a wait that crosses pools.** One line is shared: FIX_27's direction B rewrites
  `auto pool = sem->get_internal()->get_pool();` in `sem_awaiter_t::await_suspend()`, and this diff
  deletes it. This diff alone closes FIX_27's first half (a wait on pool B's semaphore ran pool B's
  coroutines inside `pool_a->run()`) and leaves the second (`_awake_one()` queues the waiter on the
  semaphore's pool). FIX_27's test shows it: on the base it fails at half 1, on this diff alone at
  half 2, and on both it passes, plain and ASan. With this diff first, FIX_27's hunk 4 drops (the
  rest applies with offsets: +9 -8 instead of +10 -9); with FIX_27 first, this diff's
  `sem_awaiter_t` hunk deletes FIX_27's version of the line. Direction A's guard keeps compiling
  (it uses `sem_internal_t::get_pool()`, which this diff leaves), but its hunk needs re-basing.
- **FIX_09, `run()` returns `RUN_ERRORED`.** No line is touched by both. They compose, and this
  diff makes FIX_09 matter more: with `run()` the only caller of `next_task_state()`, its no-task
  exit is where every failed io poll lands. Without FIX_09, a failure that the base absorbed with
  `run()`'s second poll now ends the run reported as `RUN_OK`; the IOCP and epoll backends retry
  their transient failures, so none reaches that exit here today. FIX_09 should land before this
  diff or with it. Its test binds a socket, so the review ran a socket-free stand-in with the same
  asserts: it fails on the base and on this diff alone (`RUN_OK`), and passes with FIX_09 on either
  (`RUN_ERRORED`), plain and ASan. FIX_09's prose about `next_task()` goes stale.
- **FIX_07, a woken waiter holds its semaphore.** No changed line in common, either order. A
  semaphore cleared by a SCHED callback inside X's wait is a route of this bug that FIX_07 doesn't
  close and this diff does (the cleared-semaphore row above). The review also found, apart from this
  diff, that FIX_07's `await_resume()` takes an empty hold on FIX_02's path where the waiter is
  signaled from its own callbacks (`018-034` aborts on base + FIX_07); FIX_07 needs re-basing on
  FIX_02 either way.
- **FIX_08, a park that a later error overrode.** No line is touched by both, either order. FIX_08
  picks between an awaiter's park and refusal branches, which this diff leaves as they are; its test
  passes on the combined header, plain and ASan. Its diff needs re-basing on the current
  `do_generic_modifs()` either way.
- **FIX_14, a `co_yield` doesn't complete a future.** Independent, either order: FIX_14's guard
  relies on nothing this diff changes in `cpp_yield_awaiter()`. Both tests pass on the combined
  header, plain and ASan.
- **FIX_36, the identity of an unwait.** Unrelated, either order. This diff's test happens to check
  FIX_36's assert on an engine path: its io case unwaits through `close_wait()` under
  `COLIB_ENABLE_DEBUG_CHECKS`, and a header with that assert inverted aborts there.

The docs that describe the old behavior, to update when the diff is applied: `docs/02_api.md` (the
`external_has_next_task()` and `external_wait_next_task()` entries, and the "Task Initialization"
block if open question 4 changes it), `docs/03_execution_model.md` (a yield and a semaphore wait
transfer to `next_task()`), `docs/understanding.md` (a "blocking fetch" for driving the loop by
hand, and the paragraph on where a finished coroutine goes, which names `next_task()`),
`docs/TODO.md` item 8 (its self-yield question goes with the TODO it quotes), and `README.md`'s note
on compiling (the same as the header's). `tests/CLAUDE.md` says that `001-001` overflows under
GCC's ASan, which it no longer does (long runs of calls still do); it is a CLAUDE.md file, so that
sentence changes only on the user's word.

## Open questions

1. **`external_wait_next_task()`'s contract.** Its docs, and those of `external_has_next_task()`,
   promised that it blocks until a coroutine is ready and returns that coroutine, and
   `docs/understanding.md` describes it as a blocking fetch for driving the pool's loop by hand.
   With this diff it returns `std::noop_coroutine()` at once. Returned from an `await_suspend`, its
   documented use, it does what Option 3 says: the pool goes on with another coroutine. Called
   anywhere else, for example in a loop of the user's that resumes what it returns, it now gives
   nothing to run. The choices:
   - (a) keep the name and the new docs, as in the diff;
   - (b) rename it after what it now does, or deprecate it in favor of returning
     `std::noop_coroutine()` directly;
   - (c) keep the old contract for calls made outside an `await_suspend` (then `next_task()`
     stays for it), and document that it must not be returned from one, which takes Option 3 away.

   No test in `tests/` calls it; FIX_19's test does (see Interactions).
2. **`external_has_next_task()` keeps the window open.** `has_next_task_state()` calls
   `_take_thread_tasks()`, so the SCHED callbacks of the coroutines handed over with
   `thread_sched()` run wherever the user calls it. Two failures, both on the base and the patched
   header alike:
   - inside an `await_suspend`: an awaitable that calls `external_on_suspend()`,
     `external_sched_resume()`, then `external_has_next_task()` is freed in its own `await_suspend`
     by a SCHED callback's kill, a heap-use-after-free under ASan
     (`repro/fix/43/noop/wf-review-B-windows/`, rerun in the main session); held
     (`STATE_EXTERNAL`) or still running, the kill throws instead, and a throw from a callback ends
     the program;
   - inside a SCHED callback: the nested pass takes the same handover twice, and the program ends in
     `std::terminate()` (`wf-review-A-resumers/a7`).

   The diff leaves this function as it is. The choices:
   - (a) leave it, and say in its doc that it runs the SCHED callbacks of handed-over coroutines, so
     it must not be called from an `await_suspend` once the coroutine is queued again, or from a
     SCHED callback;
   - (b) let it answer from the queues alone (a ready coroutine, or a handover pending) without
     taking anything in, the way this diff takes the scheduler out of the awaiters.
3. **Resumes from outside `run()`.** User code that resumes a coroutine itself now gets control
   back at that coroutine's next colib wait. The review ran it with an APC that runs inside the
   pool's alertable wait (`wf-review-A-resumers/a1`, a sleeping coroutine keeps the pool waiting):
   - the APC resumes a held coroutine (`external_on_resume()`, then `resume()`), which yields. On
     the base the yield ran the scheduler inside the APC, and the coroutine finished there. On the
     patched header the yield returns to the APC, and the coroutine waits in the ready queue until
     the next io completion (2.6 s in the run), or for good when no io ever completes;
   - the APC queues the held coroutine with `external_sched_resume()`: it waits for the next io
     completion on both headers, before this fix too.

   Only wakeups through the ready queue wait (a yield, a signal, `external_sched_resume()`, a
   `sched()`); a coroutine whose next wait is io wakes on its own completion. The cause is in the
   IOCP `io_pool_t::handle_ready()`: after an APC (`WAIT_IO_COMPLETION`) it goes back to waiting
   without looking at the ready queue again. Looking there, and returning when something is ready,
   would cover both cases; it is a separate change, not built or run here.
4. **The "Task Initialization" doc of `external_init_task()`.** It names "resuming the coroutine
   engine from inside a callback" as the use, and its example resumes a continuation, then destroys
   it as soon as `resume()` returns. On the base, the continuation's colib wait ran the scheduler
   inside the callback, so `resume()` came back only when the continuation reached its stop
   awaitable, if nothing else was ready. On the patched header `resume()` comes back at the first
   colib wait, and the callback destroys a coroutine that is still waiting: with a yield it is
   silently dropped from the ready queue; with a sleep in a call chain, the inner frame later returns
   into the freed continuation, a heap-use-after-free (`wf-review-A-resumers/a2`). The base already
   broke the same way whenever another coroutine was ready (heap corruption, `0xC0000374`). The
   choices: drop the sentence and the example; or rewrite the example so that `pool_t::run()` takes
   the continuation through its waits and the callback destroys it only after its stop. The diff
   leaves the block as it is; `docs/02_api.md` mirrors it.
5. **`sem_internal_t::get_pool()`** has no caller left. FIX_27's direction B removes it, and its
   direction A's guard uses it, so the diff leaves it for that ruling.
