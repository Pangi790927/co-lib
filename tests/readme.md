# colib.h - What This Project Learned About It

**colib.h** (`../colib.h`) is a single-header C++20 coroutine library: `co_await`/`co_yield`/`co_return`
coroutines scheduled on a `pool_t`, with async I/O (epoll/IOCP/kqueue), semaphores, timers, a
lifecycle-callback ("modifications") system, and a custom allocator. This project builds standalone
tests for it (see `progress.md`/`todo.md`); this file captures the usage patterns and non-obvious
behavior those tests uncovered, so the next person doesn't have to re-derive them from scratch. For the
full API surface, colib.h's own inline docs are the primary source — this isn't a copy of that.

## Quick Start

The pattern every test in this directory follows:

```cpp
#include "../colib.h"
namespace co = colib;

co::task_t hello() {
    co_await co::sleep_ms(10);
    co_return 0;
}

int main() {
    auto pool = co::create_pool();  // pool_p, shared_ptr-managed
    pool->sched(hello());           // schedule a coroutine
    return pool->run();             // run the event loop; 0 == co::RUN_OK
}
```

## Core Concepts, with a Real Example for Each

- **`task<T>` / `task_t`** - coroutine return type. See `7-1-futures.cpp` (`co::task<T>` with a
  non-void result via `co::create_future`) and `8-1-wait_all.cpp` (`co::wait_all` over heterogeneous
  `task<T>`s, returning a `std::tuple`).
- **`pool_t`** - shared scheduler: ready queue, I/O pool, timer pool, allocator. Single-threaded by
  default (`COLIB_ENABLE_MULTITHREAD_SCHED` adds `thread_sched()` from other threads, still
  cooperative). See `4-1-pool_clear.cpp` for pool lifecycle/`clear()` semantics.
- **`state_t`** - per-coroutine internal state (error, pool pointer, modif table, caller-state chain,
  handle, exception, `user_ptr`). See `12-1-introspection_get_pool_get_state.cpp` — note `state_t::user_ptr` is
  per-coroutine data, distinct from any pool-level state.
- **`sem_t`** - counting semaphore, can start negative so multiple waiters queue up before the first
  signal. See `1-1` through `1-9` (ping-pong, multi-wait init, mutex-style protection, multiple
  waiters, try_dec, signal_all, clear, destruction-with-waiters, negative signal).
- **`modif_t`** - lifecycle callbacks (CALL/SCHED/EXIT/LEAVE/ENTER/WAIT_IO/UNWAIT_IO/WAIT_SEM/UNWAIT_SEM)
  with `ON_CALL`/`ON_SCHED` inheritance flags. Built-in modifications exercised: `dbg_create_tracer()`
  in `6-1-dbg_trace.cpp`. User-defined `create_modif()`: `11-1-modifs.cpp` covers `CO_MODIF_CALL_CBK`/
  `CO_MODIF_SCHED_CBK`; the remaining 7 modif types, inheritance flags, and
  `task_modifs`/`add_modifs`/`rm_modifs` aren't tested yet (see `todo.md` Category 11).
- **I/O pool** - epoll (Linux) / IOCP (Windows) / kqueue (UNIX). See `5-1-io.cpp` for the platform-
  specific connect/accept/read/write calls, and `5-3-io_stop_fd.cpp` / `5-4-io_stop_handle.cpp` for
  registered-vs-unregistered fd/handle teardown.
- **Timer pool** - OS timers behind `sleep_us/ms/s`. `COLIB_MAX_TIMER_POOL_SIZE` (default 64) is *not*
  a concurrency cap — it's the size of the reuse cache for freed timers; concurrent sleeps are
  unbounded (confirmed in `3-4-sleep_timer_pool_limit.cpp` by running 2x that many concurrently).
  See `3-1-sleep.cpp`.
- **Allocator** - 5-bucket custom allocator (32/64/128/512/2048 bytes, scaled by
  `COLIB_ALLOCATOR_SCALE`). Not exercised by any test yet (see `todo.md` Category 14).

## Non-Obvious Behavior (learned by actually running the tests)

- **`signal()` doesn't suspend the signaler.** `1-2-semaphore_multi_wait.cpp` has two coroutines each
  call `sem->signal()` 50 times in a tight loop with no intervening await — the single waiter's counter
  only updates once it's actually scheduled, i.e. `signal()` is fire-and-forget, not a handoff.
- **`pool->stopval` doesn't auto-reset.** `2-1-flowctrl_force_stop.cpp`: each `force_stop(i)` call sets
  `stopval = i` for that `run()` invocation, but if you don't reset it to 0 yourself, the *next*
  `run()` call (even the final one returning `RUN_OK`) still reports the last-set value.
- **Destruction order on `pool->clear()` is deterministic, not incidental.** `4-1-pool_clear.cpp`: coroutines
  parked on I/O/timers are torn down before semaphore waiters; among semaphore waiters, FIFO queue
  order holds; nested calls unwind callee-before-caller; the coroutine that itself called
  `force_stop()` (already back in the ready queue) is destroyed last of all.
- **`create_future()` is created before the task runs, not after.** `7-1-futures.cpp`'s working pattern
  is `t = task(); f = create_future(pool, t); co_await sched(t); result = co_await f;` — future first,
  then schedule, then await. Not verified whether creating the future after scheduling would also
  work; the only tested order is future-before-schedule.
- **Uncaught exceptions cross `co_await` and `pool->run()` boundaries like normal C++ exceptions.**
  `10-1-exceptions.cpp` throws several levels deep through nested `co_await` chains, with RAII
  (`FnScope`) destructors firing in the expected order, and the exception is still catchable with an
  ordinary `try`/`catch` around `pool->run()`.

## Configuration Macros

| Macro                          | Type | Default | Description                                                      |
|--------------------------------|------|---------|------------------------------------------------------------------|
| COLIB_OS_LINUX                 | BOOL | auto    | Use Linux implementation                                         |
| COLIB_OS_WINDOWS               | BOOL | auto    | Use Windows implementation                                       |
| COLIB_OS_UNIX                  | BOOL | auto    | Use UNIX implementation                                          |
| COLIB_OS_UNKNOWN               | BOOL | false   | Use custom implementation                                        |
| COLIB_MAX_TIMER_POOL_SIZE      | INT  | 64      | Size of the reuse cache for freed timers (not a concurrency cap) |
| COLIB_MAX_FAST_FD_CACHE        | INT  | 1024    | Max FD cache size (Linux)                                        |
| COLIB_ENABLE_MULTITHREAD_SCHED | BOOL | false   | Enable multi-thread scheduling                                   |
| COLIB_ENABLE_LOGGING           | BOOL | true    | Enable logging                                                   |
| COLIB_ENABLE_DEBUG_CHECKS      | BOOL | false   | Enable debug assertions                                          |
| COLIB_ENABLE_DEBUG_TRACE_ALL   | BOOL | false   | Enable full tracing                                              |
| COLIB_ENABLE_DEBUG_NAMES       | BOOL | false   | Enable debug names                                               |
| COLIB_DISABLE_ALLOCATOR        | BOOL | false   | Disable custom allocator                                         |
| COLIB_ALLOCATOR_SCALE          | INT  | 16      | Allocator bucket scaling                                         |
| COLIB_ALLOCATOR_REPLACE        | BOOL | false   | Replace allocator                                                |
| COLIB_WIN_ENABLE_SLEEP_AWAKE   | BOOL | false   | Windows timer flag                                               |
| COLIB_LOG_FUNCTION             | CODE | printf  | Custom log function                                              |

## Existing Tests

This directory is the library's test suite - one self-contained `.cpp` file per test, named
`MAJOR-MINOR-description.cpp` and grouped into topic categories (semaphores, flow control, timing,
I/O, modifications, and more - see `progress.md`'s Categories table). It grew out of, and has fully
superseded, an older single-file `../tests.cpp` (retired once this directory's coverage matched and
then exceeded it).

For build commands and testing conventions in this directory, see `CLAUDE.md`. For which features each
test file here actually covers (and which are still stubs), see `progress.md` and `todo.md`.

## References
- Library version: 0.1.0
- License: MIT
- Author: Andrei Pangratie
- Repository: co-lib (this directory is `co-lib/tests/`; tests include the header via `../colib.h`)
