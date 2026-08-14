# colib.h Testing - Progress

## Overview
Status of the test files in this directory: individual, modular `.cpp` tests for the **colib.h**
single-header C++20 coroutines library. This is now the library's sole test suite - it started as an
extraction/restructuring of an older single-file `../tests.cpp`, which has since been retired (its
coverage was a strict subset of what ended up here).

---

## Categories

Test files are named `MAJOR-MINOR-description.cpp`, where MAJOR and MINOR are both zero-padded to 3
digits (`001`, not `1`) so a plain lexicographic sort - which is what `ls`, file pickers, and the
makefile's `$(wildcard *.cpp)` all use - matches true numeric order. MAJOR is a topic category
(below); MINOR is a sub-test within that category. Category numbers are shared with `todo.md` — a
category listed there with "no test file yet" will get its number's first MAJOR once a test for it is
written.

| #  | Category                | Files                                                                                                       |
|----|-------------------------|-------------------------------------------------------------------------------------------------------------|
| 1  | Semaphores              | `001-001` .. `001-009`                                                                                              |
| 2  | Force Stop / Killing    | `002-001-flowctrl_force_stop.cpp`, `002-002-flowctrl_create_killer.cpp`                                             |
| 3  | Timing                  | `003-001-sleep.cpp`, `003-002-sleep_duration.cpp`, `003-003-sleep_create_timeo.cpp`, `003-004-sleep_timer_pool_limit.cpp`   |
| 4  | Pool lifecycle          | `004-001-pool_clear.cpp`, `004-002-pool_get_internal_handle.cpp`                                                    |
| 5  | I/O                     | `005-001-io.cpp`, `005-002-io_wait_event.cpp`, `005-003-io_stop_fd.cpp`, `005-004-io_stop_handle.cpp`, `005-005-io_stop_io.cpp` |
| 6  | Debugging               | `006-001-dbg_trace.cpp`                                                                                         |
| 7  | Futures                 | `007-001-futures.cpp`                                                                                           |
| 8  | wait_all                | `008-001-wait_all.cpp`                                                                                          |
| 9  | Yielding / generators   | `009-001-yielding.cpp`                                                                                          |
| 10 | Exceptions              | `010-001-exceptions.cpp`                                                                                       |
| 11 | Modifications           | `011-001-modifs.cpp`, `011-002-modifs_await.cpp`, `011-003-modifs_lifecycle.cpp`                                     |
| 12 | Coroutine introspection | `012-001-introspection_get_pool_get_state.cpp`                                                                 |
| 13 | External Awaitables     | *(no test file yet - see todo.md)*                                                                          |
| 14 | Allocator               | *(no test file yet - see todo.md)*                                                                          |
| 15 | Configuration Macros    | *(no test file yet - see todo.md)*                                                                          |
| 16 | Stress & Edge Cases     | *(no test file yet - see todo.md)*                                                                          |
| 17 | Integration             | *(no test file yet - see todo.md)*                                                                          |
| 18 | Reproduced Bugs         | `018-001` .. `018-005`                                                                                       |

Non-obvious placements: `002-002-flowctrl_create_killer.cpp` groups with `force_stop` (both terminate tasks/pools)
rather than with `modifs`, even though it's implemented via `create_modif()` internally. `011-002-modifs_await.cpp`
groups with `modifs` (colib.h's own docs list `await()` under its "Modifications" function group) rather
than with flow control. `004-002-pool_get_internal_handle.cpp` groups with `004-001-pool_clear.cpp` under "Pool lifecycle"
rather than standing alone. `005-002`/`005-003`/`005-004` group with `005-001-io.cpp` under one "I/O" category rather
than a separate "I/O teardown" category.

**Category 18 is different from the rest:** every other category tests a *feature*; 18 tests a *bug in
colib.h*, fixed or not. The workflow is reproduce-first, not fix-first: the moment a suspected bug is
confirmed to actually reproduce, it gets a `18-N` test file *and* a `BUGS.md` entry, in the same pass -
before any fix exists. Until it's fixed, that test is *expected to fail* (an assertion failure, or,
for something like a crash/UAF, the process dying outright - either way `make all` stops there, which
is the point: a known, un-fixed bug should visibly block the suite, not sit invisible in a doc). Once
colib.h is fixed, the same file stays untouched (or has its assertions adjusted to match the actual
fix if the original ones were provisional) and starts passing - it becomes the regression test, and
the matching `BUGS.md` entry is removed. Each file's header comment always explains the bug, where it
lives, how it was confirmed to reproduce, and - once fixed - why the fix works. When one of these ever
starts failing again after having passed, it means colib.h regressed a specific, previously-fixed bug,
not just "some coverage broke." Not every reproduce-first investigation ends in a colib.h code change,
though: `018-006` (scheduling the no-arg `add_modifs`/`rm_modifs`/`task_modifs` instead of
`co_await`-ing them) turned out not to be a defect on investigation - the call is meaningless by
construction, not an edge case colib.h failed to handle - so the resolution was a `@warning` doc
comment on those three declarations in colib.h, not a runtime check, and the test was retired rather
than kept: asserting that a specific *undefined-behavior* crash keeps reproducing forever isn't a
meaningful regression guarantee the way asserting a *fixed* defect stays fixed is. This pattern (reproduce and commit the failing test before touching
the fix) is the general bug-handling workflow for this project, not just a `tests/` convention - see
the root `CLAUDE.md`.

---

## Test Files

| File                                        | Type   | Purpose                                                                           | Status       |
|---------------------------------------------|--------|-----------------------------------------------------------------------------------|--------------|
| `tests_common.h`                            | Header | Common test utilities, assertions, PASSED/FAIL output                             | Complete     |
| `makefile`                                  | Build  | Makefile for compiling and running all tests                                      | Complete     |
| `001-001-semaphore_ping_pong.cpp`               | Test   | Semaphore ping-pong test                                                          | Complete     |
| `001-002-semaphore_multi_wait.cpp`              | Test   | Semaphore multi-wait initialization test                                          | Complete     |
| `001-003-semaphore_protect.cpp`                 | Test   | Semaphore protection test                                                         | Complete     |
| `001-004-semaphore_multiple_waiters.cpp`        | Test   | Multiple waiters semaphore test                                                   | Complete     |
| `001-005-semaphore_try_dec.cpp`                 | Test   | Semaphore try_dec test                                                            | Complete     |
| `001-006-semaphore_signal_all.cpp`              | Test   | sem_t::signal_all() broadcast test                                                | Complete     |
| `001-007-semaphore_clear.cpp`                   | Test   | sem_t::clear() destructive-reset test                                             | Complete     |
| `001-008-semaphore_destruction.cpp`             | Test   | semaphore destruction with waiters test                                           | Complete     |
| `001-009-semaphore_signal_negative.cpp`         | Test   | sem_t::signal() negative increment test                                           | Complete     |
| `002-001-flowctrl_force_stop.cpp`               | Test   | Force stop test                                                                   | Complete     |
| `002-002-flowctrl_create_killer.cpp`            | Test   | create_killer() semaphore-waiter kill test                                        | Complete     |
| `003-001-sleep.cpp`                             | Test   | Sleep functions test                                                              | Complete     |
| `003-002-sleep_duration.cpp`                    | Test   | sleep(chrono::duration) with measured-elapsed-time test                           | Complete     |
| `003-003-sleep_create_timeo.cpp`                | Test   | create_timeo() completes-in-time/times-out test                                   | Complete     |
| `003-004-sleep_timer_pool_limit.cpp`            | Test   | COLIB_MAX_TIMER_POOL_SIZE unbounded-concurrency test                              | Complete     |
| `004-001-pool_clear.cpp`                        | Test   | Clear/destruction order test                                                      | Complete     |
| `004-002-pool_get_internal_handle.cpp`          | Test   | pool_t::get_internal_handle() test                                                | Complete     |
| `005-001-io.cpp`                                | Test   | IO operations test (platform-specific)                                            | Complete     |
| `005-002-io_wait_event.cpp`                     | Test   | wait_event() readable-pipe test (Linux)                                           | Complete     |
| `005-003-io_stop_fd.cpp`                        | Test   | stop_fd test (Linux/Unix)                                                         | Complete     |
| `005-004-io_stop_handle.cpp`                    | Test   | stop_handle test (Windows)                                                        | Complete     |
| `005-005-io_stop_io.cpp`                        | Test   | stop_io() registered/unregistered cancellation test (Linux)                       | Complete     |
| `006-001-dbg_trace.cpp`                         | Test   | Debug trace test                                                                  | Complete     |
| `007-001-futures.cpp`                           | Test   | Futures test                                                                      | Complete     |
| `008-001-wait_all.cpp`                          | Test   | wait_all test                                                                     | Complete     |
| `009-001-yielding.cpp`                          | Test   | Yielding test                                                                     | Complete     |
| `010-001-exceptions.cpp`                       | Test   | Exceptions test                                                                   | Complete     |
| `011-001-modifs.cpp`                           | Test   | CO_MODIF_CALL_CBK/SCHED_CBK test                                                  | Complete     |
| `011-002-modifs_await.cpp`                     | Test   | await() test                                                                      | Complete     |
| `011-003-modifs_lifecycle.cpp`                 | Test   | EXIT/LEAVE/ENTER/WAIT_IO/UNWAIT_IO/WAIT_SEM/UNWAIT_SEM + ON_CALL inheritance test | Complete (1) |
| `011-004-modifs_inherit_on_sched.cpp`          | Test   | CO_MODIF_INHERIT_ON_SCHED test                                                   | Complete     |
| `011-005-modifs_standalone_explicit.cpp`       | Test   | task_modifs(t)/add_modifs(pool,t,mods)/rm_modifs(t,mods) explicit-target test    | Complete     |
| `012-001-introspection_get_pool_get_state.cpp` | Test   | get_pool/get_state test                                                           | Complete     |
| `018-001-reproduced_modif_helpers_self_target.cpp` | Test | no-arg add_modifs()/rm_modifs()/task_modifs() operate on the caller's own state, not a throwaway helper coroutine's | Complete |
| `018-002-reproduced_call_modif_failure_default.cpp` | Test | task<T>::await_resume() after a failed CALL modif: default-construct instead of std::bad_variant_access; move (not copy) the return value | Complete |
| `018-003-reproduced_semaphore_signal_all_negative.cpp` | Test | sem_t::signal_all() wakes every waiter even when val started negative | Complete |
| `018-004-reproduced_future_exception_propagation.cpp` | Test | create_future() forwards an exception from the wrapped task instead of crashing | Complete |
| `018-005-reproduced_killer_after_completion.cpp` | Test | create_killer()'s kill_fn() after the target already completed naturally is a clean no-op | Complete |

Status notes:
1. `011-003-modifs_lifecycle.cpp`: covers all 7 remaining `modif_e` types and `CO_MODIF_INHERIT_ON_CALL`.
   `CO_MODIF_INHERIT_ON_SCHED` (now `011-004`) and standalone `task_modifs`/`add_modifs`/`rm_modifs`
   coverage (now `011-005` for the explicit-target overloads, `018-001` for the no-arg self-target ones)
   were the remaining gaps here - both closed.

**38 test files + 1 common header + 1 makefile = 40 files (38 complete, 0 stubs)**

For remaining/uncovered features (not yet a test file at all), see `todo.md`.

---

## Test Organization

### Test Naming Convention
Tests follow the format: `MAJOR-MINOR-DESCRIPTION.cpp`
- **MAJOR**: category number, zero-padded to 3 digits (`001`, `018`, ...) — see Categories table above
- **MINOR**: sub-test number within that category, also zero-padded to 3 digits (`001`, `002`, ...)
- **DESCRIPTION**: Brief descriptive name of the test

### Build System Features
The `makefile` provides:
- **Individual compilation**: `make TEST_NAME` compiles a specific test
- **All tests**: `make all` compiles and runs all tests
- **Unix target**: `make unix` compiles with Unix flags and links with kqueue
- **Clean**: `make clean` removes all executables

### Test Output
Each test prints **PASSED** (green) or **FAILED** (red) with the test filename.

For what's left to do (including process/meta items like platform verification), see `todo.md`.
