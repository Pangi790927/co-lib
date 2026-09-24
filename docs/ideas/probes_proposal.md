# Proposal: probes, as classed traces

Status: **proposal, nothing applied.** Suggested by the user on 2026-09-24, while forcing
`018-023`'s race with sleeps, and shaped with them the same evening: the probes are the trace
points, each with a class.

## The idea

colib.h already has 82 trace points (53 `COLIB_DEBUG_TRACE`, 29 `COLIB_DEBUG_TRACE_SCOPE`), today
all switched together by `COLIB_ENABLE_DEBUG_TRACE_ALL`. Each one gets a class (`pool`, `io`,
`timer`, `sem`, `killer`, `modif`, ...) and calls one function template per class:

- **By default** that function formats the line and sends it to the log and to the tester:
  `("pool", "next_state: %p", "next_state: 0x43453")`. That's enough to check orderings and to
  count events, and it gives users a tracer they can switch on per class.
- **When a test needs control**, it specializes the function for a class and gets the trace's
  arguments themselves, typed, instead of the formatted text: the `state_t *`, the `io_desc_t &`,
  the `error_e`. It can then act at that exact moment, or change an argument that is passed by
  reference.

Modifs hook a coroutine's lifecycle as public API. The traces see the engine: the io poll, a
completion being taken, a cancel racing a completion, the ready queue.

## Why: `018-023` today, and with a probe

Today `018-023` forces its order with timing. A coroutine blocks the pool for 120ms so that two
timers expire; the timeout is 40ms so the two timers land on different Windows ticks; a helper
blocks another 30ms so that the kernel completes the read. It takes about 150ms and depends on the
tick size. With the `pool` class specialized, the test sees `next_state` for the timeout's timer
coroutine: right then it sends the byte and waits until the reader's overlapped read has completed
(`HasOverlappedIoCompleted`). The kill then runs on a finished read, with no sleeps.

## Shape

```cpp
/* the classes; a user turns one on for logging with #define COLIB_TRACE_pool 1 */
enum trace_e : int32_t {
    TRACE_pool, TRACE_io, TRACE_timer, TRACE_sem, TRACE_killer, TRACE_modif, TRACE_COUNT,
};

#ifndef COLIB_TRACE_pool
# define COLIB_TRACE_pool 0
#endif
/* ... one per class */

/* the default: format once, log it if the class is on, hand it to the tester if one is set */
template <trace_e cls>
struct trace_fn {
    template <typename ...Args>
    static void fn(const char *cls_name, bool log, const char *fmt, Args&&... args) {
        dbg_trace_default(cls_name, log, fmt, std::forward<Args>(args)...);
    }
};

#define COLIB_DEBUG_TRACE(cls, fmt, ...) do { \
    if constexpr (COLIB_TRACE_##cls || COLIB_ENABLE_PROBES) \
        trace_fn<TRACE_##cls>::fn(#cls, COLIB_TRACE_##cls, fmt, ##__VA_ARGS__); \
} while (false)
```

- **A class template, not a function template.** One class has trace points with different
  arguments (`next_state: %p` takes a `state_t *`, another takes an `io_desc_t &`). A specialized
  `trace_fn<TRACE_pool>` keeps one member template `fn(Args&&...)`, and inside it the test
  dispatches on `fmt` or on the argument types (`if constexpr`). A function template would have to
  be specialized once per exact list of argument types.
- **Arguments by reference** where the trace passes a variable, so a specialization can change it
  (injection). A trace that passes an expression passes a value.
- **The tester sink** for the default: `set_trace_sink(fn)`, called with
  `(cls_name, fmt, formatted)`; by default it is unset, and the default only logs.

## One C++ rule the design has to respect

An explicit specialization must be declared before the first use that would instantiate the
template, in every translation unit ([temp.expl.spec]; "no diagnostic required", so breaking it
compiles and then misbehaves). colib.h calls `trace_fn<TRACE_pool>` inside its own inline code, so
a specialization written after `#include "colib.h"` comes too late. The way around it: colib.h
declares the primary template, then includes a user file if one is named, before any code that
traces:

```cpp
#ifdef COLIB_TRACE_SPECIALIZATIONS
# include COLIB_TRACE_SPECIALIZATIONS    /* e.g. "018-023-probes.h": trace_fn<TRACE_pool> etc. */
#endif
```

A test sets `#define COLIB_TRACE_SPECIALIZATIONS "its_probes.h"` and puts its specializations there.
Those can only use colib types declared before that point (declarations are enough for pointers
and references), and reach test state through globals.

## Rules

1. With every class off and probes off, the traces compile to nothing, as today.
2. The default never changes the flow; only a specialization can, through reference arguments.
3. A trace's `fmt` and its argument list are what the tests rely on: change them together with the
   tests.
4. Trace points exist where decisions happen, not only where they read well in a log: some would be
   added (for example `stop_io()`'s outcome: cancelled, or completed during the cancel).

## What it would give the tests

- **Orderings:** the races in `018-015`/`018-023`, a signal between a wake and a resume, a kill
  between a completion and its resume. These are forced at the exact moment instead of with sleeps,
  or checked as a recorded sequence of lines.
- **Failure paths:** a read that fails, a timer that can't be armed, a cancel that loses to a
  completion. These are injected through reference arguments instead of real resource exhaustion.
- **Invariants,** counted in one test: every issued io ends in exactly one completion or stop, and
  no coroutine is resumed twice.

## Open questions for the user

1. The class list: `pool`, `io`, `timer`, `sem`, `killer`, `modif`, `alloc`? Finer or coarser?
2. `COLIB_DEBUG_TRACE_SCOPE`: the same classes, with the scope's end as its own line?
3. Keep `COLIB_ENABLE_DEBUG_TRACE_ALL` as "all classes on", or drop it?
4. The specializations header: is an include hook inside colib.h acceptable, or should tests
   specialize some other way?
5. The first step: class the existing traces and add the sink, then rewrite `018-023` with a
   `pool` specialization as the proof?
