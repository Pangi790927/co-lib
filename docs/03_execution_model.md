# 03. Execution Model: Call, Sched, and the Scheduler

`01_introduction.md` and `02_api.md` cover what a task/pool/semaphore *is* and what you can call on
each. This chapter is about what actually happens underneath those calls: the two different ways one
coroutine can start another (`co_await` a task vs. `sched`-ing it), what the pool's scheduler loop
does with the coroutines it's handed, and where semaphores fit into that picture. Line references are
against `colib.h` as of commit `fd519a6` - see `progress.md`.

---

## Call vs. sched: two different relationships

Every coroutine that starts running on a pool got there one of two ways, and the library treats them
as genuinely different relationships, not two spellings of the same thing.

### Call - `co_await some_task`

This is `task<T>`'s own awaiter (`task<T>::await_suspend`, colib.h ~2664-2686). Calling is a *nested*
relationship: the callee is logically part of the caller's own stack of work.

What happens, in order:

1. `do_leave_modifs(&caller...)` fires - the caller is about to stop running.
2. The callee's `state_t::caller_state` is set to point at the caller's `state_t`. This is the link
   that makes call a real stack: it's what lets a killer unwind back through nested calls (see a
   later chapter), and it's what `await_resume()` uses to know who to resume.
3. `CO_MODIF_INHERIT_ON_CALL`-flagged modifications are inherited from the caller into the callee's
   own modif table (`inherit_modifs`, colib.h ~2462-2487).
4. The callee's `CALL` modif fires. If any callback returns non-`ERROR_OK`, the call is aborted: the
   caller resumes immediately (`await_resume()` returns a default-constructed `T{}`, or throws
   `std::runtime_error` if `T` isn't default-constructible) and the callee's body never runs at all.
5. Otherwise, control transfers **directly into the callee** - `await_suspend` returns the callee's
   own coroutine handle, which the compiler treats as a tail call (a "symmetric transfer"). The
   callee starts running immediately, in the same sense a normal function call would, not by being
   queued and picked up later.

When the callee finishes (`co_return`) or yields (`co_yield`), the reverse happens
(`final_awaiter_cleanup`/`cpp_yield_awaiter`, colib.h ~4010-4048): the relevant modifs fire, and
control symmetric-transfers **straight back to `caller_state->self`** - again, no queueing, the
caller just continues exactly where it left off.

**`co_yield` vs. `co_return`:** both go through `caller_state`, symmetric-transferring back to the
caller the same way, but they differ in one important respect. `co_yield value` (`cpp_yield_awaiter`,
colib.h ~4010-4027) marks the callee's state with `err = ERROR_YIELDED` before transferring back, and
`task<T>::await_resume()` only destroys the callee's coroutine frame when `err != ERROR_YIELDED`
(colib.h ~2718-2719) - so a `co_yield`, unlike a `co_return`, leaves the callee's frame alive and
resumable. The value comes back to the caller exactly like a `co_return` value would, but
`co_await`-ing the *same* `task<T>` object again later re-enters `task<T>::await_suspend` from the
top - a fresh `CALL`, `ON_CALL` modif inheritance, all of it - which resumes the callee exactly where
its `co_yield` left off, not from the beginning. That's what lets one `task<T>` be awaited
repeatedly to pull a stream of values out of a single coroutine, one call/resume cycle per value,
until it optionally `co_return`s and gets destroyed for real.

**Consequence:** from the scheduler's point of view, a whole chain of nested calls - however deep -
is invisible. It happens entirely inside one `state->self.resume()` call from the pool's run loop
(see below). The scheduler only ever sees the outermost task that was actually scheduled.

### Sched - `pool->sched(task)` / `co_await colib::sched(task)`

Scheduling is the opposite relationship: *fire-and-forget*. The new task runs independently on the
same pool, with no return-value link and no shared call stack with whoever scheduled it.

Both forms end up in `pool_internal_t::sched()` (colib.h ~3745-3764):

1. The task is bound to the pool (`external_init_task`).
2. Any modifications passed explicitly (`{mod1, mod2}`) are attached.
3. `CO_MODIF_INHERIT_ON_SCHED`-flagged modifications are inherited from a *parent* modif table - see
   the nuance below, this isn't always available.
4. The `SCHED` modif fires.
5. The task is pushed onto the back of the pool's ready queue (`ready_tasks.push_back(state)`) - **not
   run yet.** It waits its turn like anything else already in the queue.

Notably, `state_t::caller_state` is never set here - it stays `nullptr` (its default), which is
exactly what makes sched fire-and-forget: there's no caller to symmetric-transfer back to when the
task finishes, and nothing to unwind if the scheduler itself gets killed.

**The parent-table nuance:** `CO_MODIF_INHERIT_ON_SCHED` inheritance needs a modif table to inherit
*from*, and which one you get depends on which form of `sched` you used:

- `pool->sched(task, v)` (the plain `pool_t` method, e.g. called from `main()`) has no calling
  coroutine to speak of, so `pool_t::sched` (colib.h ~4056-4059) passes `nullptr` as the parent table.
  `inherit_modifs` returns immediately on a `nullptr` parent (colib.h ~2464-2465) - nothing is
  inherited, only the modifs you passed explicitly apply.
- `co_await colib::sched(task, v)` (`sched_awaiter_t::await_suspend`, colib.h ~4195-4202) runs from
  *inside* a coroutine, and passes that coroutine's own `modif_table` as the parent - so
  `ON_SCHED`-flagged modifs attached to the scheduling coroutine **are** inherited into the new task.

Also worth noting: `sched_awaiter_t::await_suspend` returns `bool`, not a handle, and returns `false`
specifically - per the C++ coroutine spec that means "don't actually suspend." So
`co_await colib::sched(...)` doesn't pause the scheduling coroutine at all, not even for a queue
round-trip; it enqueues the new task and falls straight through to the next line.

If you actually want that round-trip - e.g. to give the just-scheduled task a chance to run before
continuing - follow the `sched` with `co_await colib::yield()` (`yield_awaiter_t`, colib.h
~4150-4181): it pushes the *current* coroutine onto the back of the ready queue and immediately
symmetric-transfers to whatever's next in line. Since the freshly sched'd task was pushed onto that
same queue first, it's ahead in FIFO order and gets to run before this coroutine's own turn comes
back around.

When a sched'd task finishes, `final_awaiter_cleanup` sees `caller_state == nullptr`, posts the task
for destruction, and returns `std::noop_coroutine()` - control falls back to the pool's own run loop,
not to anything in particular, since nothing was waiting on it directly. (`wait_all`/`create_future`
exist specifically to bridge this gap - `co_await`-able results without a `caller_state` link - see a
later chapter.)

### At a glance

| | Call (`co_await task`) | Sched (`pool->sched` / `co_await colib::sched`) |
|---|---|---|
| Relationship | Nested (caller "owns" the callee) | Independent, fire-and-forget |
| `caller_state` | Set to the caller | Stays `nullptr` |
| Starts running | Immediately (symmetric transfer) | Whenever the scheduler gets to it |
| Modif inheritance | `ON_CALL`-flagged | `ON_SCHED`-flagged (only if there *is* a scheduling coroutine - see above) |
| On finish | Transfers straight back to the caller | Falls back to the pool's run loop |
| Killing the origin | Tears down the callee too (same stack) | Doesn't touch it - no link |

---

## The scheduler: what `pool_t::run()` actually does

`pool_t::run()` just forwards to `pool_internal_t::run()` (colib.h ~3774-3814), which is a small
loop:

```cpp
while (true) {
    state = next_task_state();
    if (state == nullptr)
        return RUN_OK;          // (or whatever ret_val was set to - see below)

    state->self.resume();       // runs the task - and, via `call`, everything it nests inside itself

    if (posted_to_destroy) { ... }   // a finished, caller-less (sched'd) task gets destroyed here
    if (posted_exception)  { ... }   // an uncaught exception from a caller-less task rethrows here
    if (posted_stop)       { break; }  // force_stop() was called
}
```

Each iteration resumes exactly one coroutine handle and lets it run until *that* handle suspends for
real (blocks on I/O, waits on a semaphore, yields, or finishes) - which, because of symmetric
transfer, may involve running an entire chain of nested calls first. The loop doesn't know or care how
deep that chain was; it only sees the single `resume()` return.

`colib::yield()` (`yield_awaiter_t`, colib.h ~4150-4181) is what a call chain can use to opt out of
the current `resume()` early, from anywhere inside it. Whichever coroutine actually calls
`co_await colib::yield()` - however deeply nested it is under callers - only pushes *its own*
`state_t` onto the ready queue and symmetric-transfers onward to `next_task()`. Its callers
(`caller_state`, `caller_state->caller_state`, ...) are never touched by this - they're left exactly
as they were, suspended at their own `co_await`, and only get resumed later the normal way: once this
coroutine actually finishes or yields a value back through the call machinery described above. So a
`colib::yield()` deep in a chain doesn't hand control back to its immediate caller, and it doesn't
finish running the rest of that chain either - it drops out of the whole chain in one step and jumps
straight to whatever's next in the ready queue, which is very likely a completely unrelated task. The
chain isn't lost - it's been peeled apart: the yielding coroutine becomes its own standalone
ready-queue entry, decoupled for now from the callers waiting on it, and gets resumed directly (not by
re-entering via its caller) whenever the scheduler gets back around to it.

Worth being explicit about, since the names invite confusion: `colib::yield()` here is unrelated to
`co_yield`, the C++ keyword - `co_yield` is part of `call`'s machinery (see "Call" above, and
`cpp_yield_awaiter`), symmetric-transfers straight back to a specific caller, and is what "yields a
value back" means in the paragraph above. `colib::yield()` doesn't yield a value to anyone and doesn't
know who its caller is - it's a pure scheduling primitive. Same word, two unrelated mechanisms.

### The ready queue

The pool holds one queue of runnable coroutines, `ready_tasks` - a plain FIFO deque
(`std::deque<state_t*>`, colib.h ~3953). Three things feed into it:

- **`sched`** - `pool_internal_t::sched()` pushes the new task onto the back.
- **I/O completions** - when a coroutine's awaited fd/handle becomes ready (or a timer fires), the
  I/O backend pushes that coroutine's state onto the back too (see "Where I/O fits in" below).
- **Semaphore signals** - `sem_t::signal()` moves a waiter from the semaphore's own wait list back
  onto this same ready queue (see "Semaphores" below).

`next_task_state()` (colib.h ~3870-3899) is what actually pops from it: `ready_tasks.pop_front()` if
anything's there. Combined with `push_back` on the producing side, that makes the ready queue plain
FIFO - tasks run in the order they became runnable, not any kind of priority order. (There is a
`push_ready_front()`, colib.h ~3832-3834, used by `force_stop()` to jump its own continuation to the
front of the line - the one deliberate exception to FIFO ordering.)

### Where I/O and timers fit in

Before checking the ready queue, `next_task_state()` calls `io_pool.handle_ready()`. This is where the
platform-specific wait actually happens (`epoll_wait`/`GetQueuedCompletionStatusEx`/kqueue) - but only
when there's a reason to: if `ready_tasks` already has entries, `handle_ready()` is a no-op (colib.h
~2868-2876, the epoll backend) - no point checking the OS if there's already work queued. It's *only*
when the ready queue is empty and there's at least one pending I/O wait that the pool actually blocks
(`epoll_wait(..., -1)` - infinite timeout) until something becomes ready. Timers ride the same path:
`sleep*()` registers an OS timer (`timerfd` on Linux, `SetWaitableTimer` on Windows) as just another
I/O wait, so a pending sleep is exactly what keeps this blocking wait alive instead of returning
immediately.

This is also the answer to "why doesn't `run()` busy-loop": if there's nothing ready *and* nothing
pending on I/O/timers either, `handle_ready()` returns immediately doing nothing, `ready_tasks` stays
empty, and `next_task_state()` returns `nullptr` - which is exactly the "we're done" case below.

### Termination

`next_task_state()` returns `nullptr` when there's nothing left to hand the loop - `ret_val` was
already set to `RUN_OK` on that path. `run()` can also end by hitting `RUN_ERRORED` (the I/O pool
itself failed) or `RUN_STOPPED` (`force_stop()` was called, which sets `posted_stop` and jumps its
own continuation to the front of the ready queue so it runs *next*). A stopped pool can be resumed
with another `run()` call - nothing about stopping tears the pool down.

---

## Semaphores: coordinating without call or sched

A semaphore is a third, orthogonal way for coroutines to interact - it doesn't *start* anything, it
parks and un-parks coroutines that are already running independently (usually sched'd, or called from
different branches of unrelated call chains).

**`wait()`** (`sem_awaiter_t`, colib.h ~4423-4482): if the counter is already positive, it decrements
and returns immediately without suspending at all (`await_ready()`). Otherwise, the waiting
coroutine's state is pushed onto the semaphore's *own* wait list (`push_waiter`, colib.h ~4369-4376) -
a structure entirely separate from the pool's ready queue - and control falls back to
`pool->get_internal()->next_task()`, i.e. straight to whatever the scheduler picks up next. A parked
waiter isn't on the ready queue at all; the scheduler has no idea it exists until something signals
the semaphore.

**`signal(inc)`** (`sem_internal_t::signal`, colib.h ~4323-4336): adjusts the counter, then, as long
as the counter is still positive and there are waiters, moves them from the semaphore's wait list back
onto the pool's ready queue (`push_ready`) one at a time. This is the same `push_ready` that `sched`
uses - **signaling doesn't resume a waiter immediately**, symmetric-transfer style; it just makes it
runnable again, and the scheduler gets to it on its next iteration through `next_task_state()`, after
whatever's currently running actually suspends.

**Wake order is FIFO.** The wait list is a deque; `push_waiter` inserts at the front and `signal`'s
internal `_awake_one()` (colib.h ~4384-4389) takes from the back - so the longest-waiting coroutine is
always the next one signaled awake, regardless of how many are queued.

**`clear(val)`** is the exception to "signal re-enqueues": it doesn't wake waiters onto the ready
queue at all, it destroys every currently-waiting coroutine outright (their stacks unwind through
destructors, they never resume past `co_await wait()`) and resets the counter - used when a semaphore
itself is going away (its own destructor calls `clear(0)`) or the pool is tearing down.

---

## Putting it together

```cpp
colib::sem_p mutex = colib::create_sem(pool, 1);

colib::task_t worker(int id) {
    std::lock_guard guard(co_await mutex->wait());  // wait(): may park on the sem's own wait list,
                                                      // not the ready queue - if it does, this
                                                      // coroutine simply isn't runnable again until
                                                      // signal() moves it back onto the ready queue
    std::cout << "worker " << id << " in critical section" << std::endl;
    co_await colib::sleep_ms(10);                    // an ordinary I/O-backed wait: parks on the
                                                      // pool's timer/io wait set, not the ready queue
                                                      // either, until the timer fires
    co_return 0;
} // guard's destructor -> unlocker_t::unlock() -> mutex->signal() -> next waiter (if any) is
  // pushed onto the ready queue, FIFO; it does NOT run right here

colib::task_t co_main() {
    for (int i = 0; i < 3; i++)
        co_await colib::sched(worker(i));  // fire-and-forget: each worker() call enqueues, this
                                            // coroutine keeps running without suspending at all
    co_return 0;
}
```

Three sched'd `worker` tasks compete for one mutex. Each `co_await mutex->wait()` is a *call* in the
generic "co_await something" sense but has nothing to do with `task<T>`'s call machinery - it's a
plain awaiter that either resolves immediately or parks the coroutine off the ready queue entirely
until a `signal()` puts it back on. None of this - the mutex wait/signal, the sleep - involves a
`caller_state` link the way calling another `task<T>` would; each `worker` is independent, sched'd
once and then coordinating purely through the semaphore.
