# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this directory is

This is **the** test suite for **colib.h**, a single-header C++20 coroutine library that lives one
level up at `../colib.h` (~7000 lines, epoll/IOCP/kqueue-based async I/O, semaphores, timers, a custom
allocator, and a "modifications" callback system for coroutine lifecycle events). It started as an
exploratory area written while learning/testing the library, alongside a separate, older `../tests.cpp`
in the parent repo. Once this directory grew into the more thorough, better-organized suite of the two,
`../tests.cpp` was retired (its coverage was a strict subset of what's here) and this directory became
the library's one and only test suite.

Test files are numbered standalone `.cpp` programs at the top of this directory (`001-001-*.cpp`,
`012-001-*.cpp`, etc.), each self-contained with its own `main()`. `tests_common.h` provides shared
assertion macros and helpers used by all of them. The `MAJOR` number is a topic category, not a
sequence — see the Categories table in `progress.md` for what each number means.

## Build & run

The build is split by OS: the top-level `makefile` is a thin dispatcher (`include windows.makefile`
or `include linux.makefile` based on `$(OS)`) - `windows.makefile` uses `cl` and is pinned to
`cmd.exe` as its shell so it works with zero extra `PATH` setup beyond a Developer Command Prompt
(no Git-for-Windows/MSYS `usr/bin` required); `linux.makefile` uses `g++` and also owns the
Unix-only `unix`/`unix_kqueue` targets, which don't exist on the Windows side since they don't apply
there. You never need to `include`/invoke those two directly - always just run `make` and let the
dispatcher pick the right one.

The `all`/`unix`/`unix_kqueue` targets (running the whole suite) hand off to `run_tests.py`
(`python`/`python3` must be on PATH) rather than looping in the makefile itself - cmd.exe's
setlocal/delayed-expansion/errorlevel handling turned out to be too unreliable to correctly track a
pass/fail flag across a `for` loop in a single recipe line. Building/running a single test target
(`make 005-003-io_stop_fd.bin`) doesn't need Python at all.

Build everything and run all tests (each `.cpp` file compiles to its own executable; every test runs
regardless of earlier failures - a failing test doesn't stop the rest, and the overall pass/fail is
reported via the final exit code):

```bash
make            # Linux/Windows default target
make unix       # adds -DCOLIB_OS_UNIX=true, links -lkqueue
make unix_kqueue
make clean
```

To build/run a single test, use the file's basename plus the binary extension (`.bin` on
Linux/Unix/macOS, `.exe` on Windows) as the make target, or compile directly:

```bash
make 005-003-io_stop_fd.bin
./005-003-io_stop_fd.bin

# or, equivalent to what the makefile does:
g++ -std=c++2a -O3 -g -Wno-format-security -I.. 005-003-io_stop_fd.cpp -o 005-003-io_stop_fd.bin
./005-003-io_stop_fd.bin
```

Test binaries are always named `<test>.bin` or `<test>.exe` (never extensionless) so they're easy to
`.gitignore` — see the root `.gitignore`'s `*.bin`/`*.exe` rules.

There is no separate lint step — this is header-only C++, correctness is judged by compiling and
running each test binary.

## Conventions for tests in this directory

- Include order is always `../colib.h` (optionally preceded by `#define COLIB_*` config macros) then
  `"tests_common.h"`.
- Enable `COLIB_ENABLE_DEBUG_NAMES` (and register coroutines with `COLIB_REGNAME(...)` when scheduling)
  to get readable names in debug output.
- Use `DBG(fmt, ...)` for logging, `ASSERT_FN(expr)` inside plain functions and `ASSERT_COFN(expr)`
  inside coroutines — both treat a negative `int` result as failure, print file/line/errno, and
  `return`/`co_return` the error code.
- Use `FnScope` for RAII-style cleanup (e.g. closing fds) when a test can exit through multiple paths.
- Each file's `main()` calls its own test function(s), then `print_test_result(filename, ret >= 0)`,
  and returns the result.
- Platform-specific tests (fd/socket-based I/O) are guarded with `#if COLIB_OS_LINUX || COLIB_OS_UNIX`
  vs Windows-specific APIs (`ConnectEx`, `IOCP`, etc.), with a not-supported fallback branch.
- New test files should follow the existing `<NNN>-<MMM>-<short_description>.cpp` naming scheme (NNN =
  topic/category index, MMM = variant within that topic, **both zero-padded to 3 digits** - `001`,
  `018`, not `1`, `18` - so plain lexicographic sort matches numeric order) so `make`'s wildcard-based
  target discovery keeps working.

## Key colib.h concepts to know before writing tests

- **`pool_t`** (`co::create_pool()`) — shared scheduler/state for a group of coroutines: ready queue,
  I/O pool (epoll/IOCP/kqueue), timer pool, allocator. Single-threaded by default;
  `COLIB_ENABLE_MULTITHREAD_SCHED` enables `thread_sched()` from other threads (still cooperative, no
  parallel coroutine execution). Test pattern: `create_pool()` → `pool->sched(task)` → `pool->run()`.
- **`task<T>` / `task_t`** — coroutine return type wrapping a `std::coroutine_handle`.
- **`sem_t`** (`co::create_sem(pool, val)`) — counting semaphore; can start negative for multi-waiter
  setups. `wait()`, `signal(inc)`, `signal_all()`, `try_dec()`, `clear(val)`.
- **`modif_t`** — callbacks hooked to lifecycle events (`CALL`, `SCHED`, `EXIT`, `LEAVE`, `ENTER`,
  `WAIT_IO`/`UNWAIT_IO`, `WAIT_SEM`/`UNWAIT_SEM`), with inheritance flags controlling whether a
  modification propagates to awaited (`ON_CALL`) or scheduled (`ON_SCHED`) coroutines.
- **`error_e`** — `ERROR_OK=0`, `ERROR_YIELDED=1`, `ERROR_GENERIC=-1`, `ERROR_TIMEO=-2`,
  `ERROR_WAKEUP=-3`, `ERROR_USER=-4`, `ERROR_DEPEND=-5`. Negative values are the failure convention
  `ASSERT_FN`/`ASSERT_COFN` check for.
- Relevant `#define`-based config macros (must be set before including `colib.h`): `COLIB_OS_*`,
  `COLIB_ENABLE_DEBUG_NAMES`, `COLIB_ENABLE_DEBUG_TRACE_ALL`, `COLIB_ENABLE_LOGGING`,
  `COLIB_ENABLE_MULTITHREAD_SCHED`, `COLIB_ALLOCATOR_SCALE`, `COLIB_DISABLE_ALLOCATOR`.

For the full API surface (I/O ops, timers, futures, external-awaitable hooks) see `readme.md` in this
directory, which summarizes the public API extracted from `colib.h`'s inline docs.

## Working docs in this directory

`progress.md` lists what each test file actually covers and documents the category numbering scheme;
`todo.md` tracks remaining/uncovered work, organized by the same category numbers; `BUGS.md` logs
currently-open bugs found in `../colib.h` while writing these tests; `design/architecture.md` and
`notes/initial_observations.md` hold design-pattern notes not duplicated elsewhere. These are
session notes, not authoritative specs — treat `../colib.h` itself as ground truth when they disagree.

## Workflow for adding a new test

- **One test at a time, verified before moving on.** For each single item from `todo.md`: read the
  relevant `colib.h` section (don't rely on memory of the API), write exactly one test file, compile
  just that file, run it, and confirm it actually passes — before touching the next item. Don't write
  or compile a batch of untested files ahead of verification.
- **If a test fails because of a colib.h bug (not a bug in the test):** stop. Log it in `BUGS.md`
  (what's broken, where, how it was found, whether it blocks the test) and mark the corresponding
  `todo.md` item blocked, rather than silently working around the bug in the test. Once a bug is
  fixed, remove its entry from `BUGS.md` entirely — don't keep a "FIXED" note; that file only tracks
  currently-open bugs.
- **A suspected colib.h bug (not one found via the above) gets a `18-N` test file the moment it's
  confirmed to reproduce — before any fix exists.** This is the project's general bug workflow, not
  just a `tests/` convention — see the root `CLAUDE.md`. The test is expected to fail (an assertion,
  or the process crashing outright for something like a UAF) until colib.h is actually fixed; commit
  it failing anyway, alongside a `BUGS.md` entry. Once the fix lands, the same file starts passing and
  becomes the permanent regression check — see `progress.md`'s Category 18 note for the full rationale.
- **Category in both the filename and the header comment.** Every test file's name embeds its
  category (e.g. `001-005-semaphore_try_dec.cpp`, not `001-005-sem_try_dec.cpp`), and its
  `/* TestN - Category: detail` header comment leads with the same category word (see `progress.md`'s
  Categories table for the current list and short tags: Semaphores, Flow Control, Timing, Pool, IO,
  Debugging, Modifs, Introspection, ...). When a genuinely new category needs a number not yet in that
  table, add it there rather than reusing an unrelated one.
- After a test passes, update `todo.md` (remove/check off the item) and `progress.md` (add the file's
  row to the Test Files table, update the category's file list and the total count) in the same pass
  — don't let those drift from what's actually been verified.
