# colib.h Testing - Progress

## Overview
Status of the test files in this directory: individual, modular `.cpp` tests for the **colib.h**
single-header C++20 coroutines library. This is now the library's sole test suite - it started as an
extraction/restructuring of an older single-file `../tests.cpp`, which has since been retired (its
coverage was a strict subset of what ended up here).

---

## Categories

Test files are named `MAJOR-MINOR-description.cpp`. MAJOR is a topic category (below); MINOR is a
sub-test within that category. Category numbers are shared with `todo.md` — a category listed there
with "no test file yet" will get its number's first MAJOR once a test for it is written.

| #  | Category                | Files                                                                                                       |
|----|-------------------------|-------------------------------------------------------------------------------------------------------------|
| 1  | Semaphores              | `1-1` .. `1-9`                                                                                              |
| 2  | Force Stop / Killing    | `2-1-flowctrl_force_stop.cpp`, `2-2-flowctrl_create_killer.cpp`                                             |
| 3  | Timing                  | `3-1-sleep.cpp`, `3-2-sleep_duration.cpp`, `3-3-sleep_create_timeo.cpp`, `3-4-sleep_timer_pool_limit.cpp`   |
| 4  | Pool lifecycle          | `4-1-pool_clear.cpp`, `4-2-pool_get_internal_handle.cpp`                                                    |
| 5  | I/O                     | `5-1-io.cpp`, `5-2-io_wait_event.cpp`, `5-3-io_stop_fd.cpp`, `5-4-io_stop_handle.cpp`, `5-5-io_stop_io.cpp` |
| 6  | Debugging               | `6-1-dbg_trace.cpp`                                                                                         |
| 7  | Futures                 | `7-1-futures.cpp`                                                                                           |
| 8  | wait_all                | `8-1-wait_all.cpp`                                                                                          |
| 9  | Yielding / generators   | `9-1-yielding.cpp`                                                                                          |
| 10 | Exceptions              | `10-1-exceptions.cpp`                                                                                       |
| 11 | Modifications           | `11-1-modifs.cpp`, `11-2-modifs_await.cpp`, `11-3-modifs_lifecycle.cpp`                                     |
| 12 | Coroutine introspection | `12-1-introspection_get_pool_get_state.cpp`                                                                 |
| 13 | External Awaitables     | *(no test file yet - see todo.md)*                                                                          |
| 14 | Allocator               | *(no test file yet - see todo.md)*                                                                          |
| 15 | Configuration Macros    | *(no test file yet - see todo.md)*                                                                          |
| 16 | Stress & Edge Cases     | *(no test file yet - see todo.md)*                                                                          |
| 17 | Integration             | *(no test file yet - see todo.md)*                                                                          |
| 18 | Reproduced Bugs         | `18-1` .. `18-6` (`18-6` currently failing - open bug, see `BUGS.md` #2)                                     |

Non-obvious placements: `2-2-flowctrl_create_killer.cpp` groups with `force_stop` (both terminate tasks/pools)
rather than with `modifs`, even though it's implemented via `create_modif()` internally. `11-2-modifs_await.cpp`
groups with `modifs` (colib.h's own docs list `await()` under its "Modifications" function group) rather
than with flow control. `4-2-pool_get_internal_handle.cpp` groups with `4-1-pool_clear.cpp` under "Pool lifecycle"
rather than standing alone. `5-2`/`5-3`/`5-4` group with `5-1-io.cpp` under one "I/O" category rather
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
not just "some coverage broke." This pattern (reproduce and commit the failing test before touching
the fix) is the general bug-handling workflow for this project, not just a `tests/` convention - see
the root `CLAUDE.md`.

---

## Test Files

| File                                        | Type   | Purpose                                                                           | Status       |
|---------------------------------------------|--------|-----------------------------------------------------------------------------------|--------------|
| `tests_common.h`                            | Header | Common test utilities, assertions, PASSED/FAIL output                             | Complete     |
| `makefile`                                  | Build  | Makefile for compiling and running all tests                                      | Complete     |
| `1-1-semaphore_ping_pong.cpp`               | Test   | Semaphore ping-pong test                                                          | Complete     |
| `1-2-semaphore_multi_wait.cpp`              | Test   | Semaphore multi-wait initialization test                                          | Complete     |
| `1-3-semaphore_protect.cpp`                 | Test   | Semaphore protection test                                                         | Complete     |
| `1-4-semaphore_multiple_waiters.cpp`        | Test   | Multiple waiters semaphore test                                                   | Complete     |
| `1-5-semaphore_try_dec.cpp`                 | Test   | Semaphore try_dec test                                                            | Complete     |
| `1-6-semaphore_signal_all.cpp`              | Test   | sem_t::signal_all() broadcast test                                                | Complete     |
| `1-7-semaphore_clear.cpp`                   | Test   | sem_t::clear() destructive-reset test                                             | Complete     |
| `1-8-semaphore_destruction.cpp`             | Test   | semaphore destruction with waiters test                                           | Complete     |
| `1-9-semaphore_signal_negative.cpp`         | Test   | sem_t::signal() negative increment test                                           | Complete     |
| `2-1-flowctrl_force_stop.cpp`               | Test   | Force stop test                                                                   | Complete     |
| `2-2-flowctrl_create_killer.cpp`            | Test   | create_killer() semaphore-waiter kill test                                        | Complete     |
| `3-1-sleep.cpp`                             | Test   | Sleep functions test                                                              | Complete     |
| `3-2-sleep_duration.cpp`                    | Test   | sleep(chrono::duration) with measured-elapsed-time test                           | Complete     |
| `3-3-sleep_create_timeo.cpp`                | Test   | create_timeo() completes-in-time/times-out test                                   | Complete     |
| `3-4-sleep_timer_pool_limit.cpp`            | Test   | COLIB_MAX_TIMER_POOL_SIZE unbounded-concurrency test                              | Complete     |
| `4-1-pool_clear.cpp`                        | Test   | Clear/destruction order test                                                      | Complete     |
| `4-2-pool_get_internal_handle.cpp`          | Test   | pool_t::get_internal_handle() test                                                | Complete     |
| `5-1-io.cpp`                                | Test   | IO operations test (platform-specific)                                            | Complete     |
| `5-2-io_wait_event.cpp`                     | Test   | wait_event() readable-pipe test (Linux)                                           | Complete     |
| `5-3-io_stop_fd.cpp`                        | Test   | stop_fd test (Linux/Unix)                                                         | Complete     |
| `5-4-io_stop_handle.cpp`                    | Test   | stop_handle test (Windows)                                                        | Complete     |
| `5-5-io_stop_io.cpp`                        | Test   | stop_io() registered/unregistered cancellation test (Linux)                       | Complete     |
| `6-1-dbg_trace.cpp`                         | Test   | Debug trace test                                                                  | Complete     |
| `7-1-futures.cpp`                           | Test   | Futures test                                                                      | Complete     |
| `8-1-wait_all.cpp`                          | Test   | wait_all test                                                                     | Complete     |
| `9-1-yielding.cpp`                          | Test   | Yielding test                                                                     | Complete     |
| `10-1-exceptions.cpp`                       | Test   | Exceptions test                                                                   | Complete     |
| `11-1-modifs.cpp`                           | Test   | CO_MODIF_CALL_CBK/SCHED_CBK test                                                  | Complete     |
| `11-2-modifs_await.cpp`                     | Test   | await() test                                                                      | Complete     |
| `11-3-modifs_lifecycle.cpp`                 | Test   | EXIT/LEAVE/ENTER/WAIT_IO/UNWAIT_IO/WAIT_SEM/UNWAIT_SEM + ON_CALL inheritance test | Complete (1) |
| `11-4-modifs_inherit_on_sched.cpp`          | Test   | CO_MODIF_INHERIT_ON_SCHED test                                                   | Complete     |
| `11-5-modifs_standalone_explicit.cpp`       | Test   | task_modifs(t)/add_modifs(pool,t,mods)/rm_modifs(t,mods) explicit-target test    | Complete     |
| `12-1-introspection_get_pool_get_state.cpp` | Test   | get_pool/get_state test                                                           | Complete     |
| `18-1-reproduced_modif_helpers_self_target.cpp` | Test | no-arg add_modifs()/rm_modifs()/task_modifs() operate on the caller's own state, not a throwaway helper coroutine's | Complete |
| `18-2-reproduced_call_modif_failure_default.cpp` | Test | task<T>::await_resume() after a failed CALL modif: default-construct instead of std::bad_variant_access; move (not copy) the return value | Complete |
| `18-3-reproduced_semaphore_signal_all_negative.cpp` | Test | sem_t::signal_all() wakes every waiter even when val started negative | Complete |
| `18-4-reproduced_future_exception_propagation.cpp` | Test | create_future() forwards an exception from the wrapped task instead of crashing | Complete |
| `18-5-reproduced_killer_after_completion.cpp` | Test | create_killer()'s kill_fn() after the target already completed naturally is a clean no-op | Complete |
| `18-6-reproduced_sched_no_arg_modif_helper.cpp` | Test | scheduling (vs. co_await-ing) a no-arg modif helper crashes on a null caller_state | **Failing (2)** |

Status notes:
1. `11-3-modifs_lifecycle.cpp`: covers all 7 remaining `modif_e` types and `CO_MODIF_INHERIT_ON_CALL`.
   `CO_MODIF_INHERIT_ON_SCHED` (now `11-4`) and standalone `task_modifs`/`add_modifs`/`rm_modifs`
   coverage (now `11-5` for the explicit-target overloads, `18-1` for the no-arg self-target ones)
   were the remaining gaps here - both closed.
2. `18-6-reproduced_sched_no_arg_modif_helper.cpp`: reproduces `BUGS.md` #2, not yet fixed - this one
   is *expected* to fail (it crashes the process) until colib.h is fixed. See the Category 18 note
   above for why it's committed in this state rather than waiting for the fix.

**39 test files + 1 common header + 1 makefile = 41 files (38 complete, 1 failing/open-bug, 0 stubs)**

For remaining/uncovered features (not yet a test file at all), see `todo.md`.

---

## Test Organization

### Test Naming Convention
Tests follow the format: `MAJOR-MINOR-DESCRIPTION.cpp`
- **MAJOR**: category number (see Categories table above)
- **MINOR**: sub-test number within that category
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
