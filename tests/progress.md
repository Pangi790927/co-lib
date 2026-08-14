# colib.h Testing - Progress

## Overview
Status of the test files in this directory: individual, modular `.cpp` tests for the **colib.h**
single-header C++20 coroutines library (extracted/restructured out of the parent repo's `../tests.cpp`).

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

Non-obvious placements: `2-2-flowctrl_create_killer.cpp` groups with `force_stop` (both terminate tasks/pools)
rather than with `modifs`, even though it's implemented via `create_modif()` internally. `11-2-modifs_await.cpp`
groups with `modifs` (colib.h's own docs list `await()` under its "Modifications" function group) rather
than with flow control. `4-2-pool_get_internal_handle.cpp` groups with `4-1-pool_clear.cpp` under "Pool lifecycle"
rather than standing alone. `5-2`/`5-3`/`5-4` group with `5-1-io.cpp` under one "I/O" category rather
than a separate "I/O teardown" category.

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
| `12-1-introspection_get_pool_get_state.cpp` | Test   | get_pool/get_state test                                                           | Complete     |

Status notes:
1. `11-3-modifs_lifecycle.cpp`: covers all 7 remaining `modif_e` types and `CO_MODIF_INHERIT_ON_CALL`.
   `CO_MODIF_INHERIT_ON_SCHED` and standalone `task_modifs`/`add_modifs`/`rm_modifs` coverage are still
   untested — see `todo.md` Category 11.

**31 test files + 1 common header + 1 makefile = 33 files (31 complete, 0 stubs)**

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
