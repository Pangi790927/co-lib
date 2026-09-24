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
| 2  | Force Stop / Killing    | `002-001-flowctrl_force_stop.cpp`, `002-002-flowctrl_create_killer.cpp`, `002-003` .. `002-013` (killer redesign)   |
| 3  | Timing                  | `003-001-sleep.cpp`, `003-002-sleep_duration.cpp`, `003-003-sleep_create_timeo.cpp`, `003-004-sleep_timer_pool_limit.cpp`   |
| 4  | Pool lifecycle          | `004-001-pool_clear.cpp`, `004-002-pool_get_internal_handle.cpp`, `004-003` .. `004-006` (thread_sched)                     |
| 5  | I/O                     | `005-001-io.cpp`, `005-002-io_wait_event.cpp`, `005-003-io_stop_fd.cpp`, `005-004-io_stop_handle.cpp`, `005-005-io_stop_io.cpp` |
| 6  | Debugging               | `006-001-dbg_trace.cpp`                                                                                         |
| 7  | Futures                 | `007-001-futures.cpp`                                                                                           |
| 8  | wait_all                | `008-001-wait_all.cpp`                                                                                          |
| 9  | Yielding / generators   | `009-001-yielding.cpp`                                                                                          |
| 10 | Exceptions              | `010-001-exceptions.cpp`                                                                                       |
| 11 | Modifications           | `011-001-modifs.cpp`, `011-002-modifs_await.cpp`, `011-003-modifs_lifecycle.cpp`, `011-006` .. `011-009`          |
| 12 | Coroutine introspection | `012-001-introspection_get_pool_get_state.cpp`, `012-002-introspection_state.cpp`                             |
| 13 | External Awaitables     | *(no test file yet - see todo.md)*                                                                          |
| 14 | Allocator               | *(no test file yet - see todo.md)*                                                                          |
| 15 | Configuration Macros    | *(no test file yet - see todo.md)*                                                                          |
| 16 | Stress & Edge Cases     | *(no test file yet - see todo.md)*                                                                          |
| 17 | Integration             | *(no test file yet - see todo.md)*                                                                          |
| 18 | Reproduced Bugs         | `018-001` .. `018-023` |

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
| `makefile`                                  | Build  | Thin dispatcher - includes windows.makefile or linux.makefile based on $(OS)      | Complete     |
| `windows.makefile`                          | Build  | Windows build (cl, pinned to cmd.exe shell so it needs no extra PATH setup)       | Complete     |
| `linux.makefile`                            | Build  | Linux/Unix build (g++); also owns the unix/unix_kqueue targets                    | Complete     |
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
| `002-003-flowctrl_killer_finished.cpp`          | Test   | a target whose wait already completed finishes inside the kill (ERROR_FINISHED)   | Complete     |
| `002-004-flowctrl_killer_self_kill.cpp`         | Test   | a coroutine can't kill itself: the kill throws kill_self_t and kills nothing       | Complete     |
| `002-005-flowctrl_killer_caller_turn.cpp`       | Test   | a killed child's caller resumes in the child's place in the ready queue           | Complete     |
| `002-006-flowctrl_killer_driven_yield.cpp`      | Test   | a scheduled root that co_yields while the kill resumes it: std::terminate(), nothing else ran inside the kill | Complete |
| `002-007-flowctrl_killer_parked_unwound.cpp`    | Test   | a chain parked by the kill that resumed it; the other killer finds nothing left    | Complete     |
| `002-008-flowctrl_killer_generator_yield.cpp` | Test | a killer attached to a generator: its root's first co_yield calls std::terminate() | Complete |
| `002-009-flowctrl_killer_generator_driven.cpp` | Test | a generator that co_yields while the kill resumes it: its co_yield calls std::terminate() | Complete |
| `002-010-flowctrl_killer_generator_first_run.cpp` | Test | a generator killed before its first co_yield is killed like any coroutine, no terminate | Complete |
| `002-011-flowctrl_killer_generator_later_run.cpp` | Test | a generator with a killer never gets a later run to be killed in: its first co_yield terminates | Complete |
| `002-012-flowctrl_killer_generator_silent.cpp` | Test | a generator with a killer is never left idle for a kill to find: its co_yield terminates | Complete |
| `002-013-flowctrl_killer_get_err.cpp` | Test | a killed callee returns the default value and task<T>::get_err() gives the kill's error | Complete (13) |
| `003-001-sleep.cpp`                             | Test   | Sleep functions test                                                              | Complete     |
| `003-002-sleep_duration.cpp`                    | Test   | sleep(chrono::duration) with measured-elapsed-time test                           | Complete     |
| `003-003-sleep_create_timeo.cpp`                | Test   | create_timeo() completes-in-time/times-out test                                   | Complete     |
| `003-004-sleep_timer_pool_limit.cpp`            | Test   | COLIB_MAX_TIMER_POOL_SIZE unbounded-concurrency test                              | Complete     |
| `004-001-pool_clear.cpp`                        | Test   | Clear/destruction order test                                                      | Complete     |
| `004-002-pool_get_internal_handle.cpp`          | Test   | pool_t::get_internal_handle() test                                                | Complete     |
| `004-003-pool_thread_sched.cpp` | Test | with COLIB_ENABLE_MULTITHREAD_SCHED, a coroutine thread_sched()'d from another thread runs on the pool's thread | Complete (12) |
| `004-004-pool_thread_sched_debug_checks.cpp` | Test | the same with the debug checks: a thread_sched()'d coroutine gets its SCHED before its first ENTER | Complete (12) |
| `004-005-pool_thread_sched_many.cpp` | Test | 8 threads thread_sched() 1600 coroutines concurrently: each runs once, on the pool's thread | Complete (12) |
| `004-006-pool_thread_sched_from_sched_cbk.cpp` | Test | a SCHED callback of a thread_sched()'d coroutine calls thread_sched() on the same pool: no double lock, both run | Complete (14) |
| `005-001-io.cpp`                                | Test   | IO operations test (platform-specific)                                            | Complete (3) |
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
| `011-006-modifs_force_suspend.cpp`             | Test   | ERROR_SUSPENDED parks (LEAVE, WAIT, UNWAIT), the modif owns it; a refused YIELD    | Complete     |
| `011-007-modifs_close_order.cpp`               | Test   | closing callbacks run in reverse order, running and waiting never overlap         | Complete     |
| `011-008-modifs_yield_pair.cpp`                | Test   | co::yield() is a paired wait: YIELD/UNYIELD, refused, and closed by a kill        | Complete     |
| `011-009-modifs_return.cpp`                    | Test   | RETURN on co_yield (alive, no EXIT) and co_return; a killer follows CALL/RETURN    | Complete     |
| `012-001-introspection_get_pool_get_state.cpp` | Test   | get_pool/get_state test                                                           | Complete     |
| `012-002-introspection_state.cpp`              | Test   | state_t::get_state() follows a coroutine; a queued coroutine destroyed leaves the queue | Complete |
| `018-001-reproduced_modif_helpers_self_target.cpp` | Test | no-arg add_modifs()/rm_modifs()/task_modifs() operate on the caller's own state, not a throwaway helper coroutine's | Complete |
| `018-002-reproduced_call_modif_failure_default.cpp` | Test | task<T>::await_resume() after a failed CALL modif: default-construct instead of std::bad_variant_access; move (not copy) the return value | Complete |
| `018-003-reproduced_semaphore_signal_all_negative.cpp` | Test | sem_t::signal_all() wakes every waiter even when val started negative | Complete |
| `018-004-reproduced_future_exception_propagation.cpp` | Test | create_future() forwards an exception from the wrapped task instead of crashing | Complete |
| `018-005-reproduced_killer_after_completion.cpp` | Test | create_killer()'s kill_fn() after the target already completed naturally is a clean no-op | Complete |
| `018-006-reproduced_killer_reentrancy.cpp` | Test | create_killer()'s kill_fn() called reentrantly (from a destructor of a frame it's tearing down) corrupts kstate->call_stack | Complete |
| `018-007-reproduced_signal_zero_boundary.cpp` | Test | sem_t::signal(0) is a no-op whatever the counter is (val == 0, val < 0, val > 0) | Complete |
| `018-008-reproduced_unlocker_spurious_signal.cpp` | Test | sem_t::unlocker_t must not signal when the wait() it came from was aborted by a WAIT_SEM_CBK modif | Complete |
| `018-009-reproduced_unlocker_fastpath_null.cpp` | Test | sem_t::unlocker_t from a legitimate fast-path acquire (await_ready() itself resolved) must still be a real, usable unlocker | Complete |
| `018-010-reproduced_allocator_deallocate_uaf.cpp` | Test | allocator_t<T>::deallocate() must not use-after-free when a modif_p outlives the pool it was created from | Complete |
| `018-011-reproduced_call_modif_failure_double_enter.cpp` | Test | caller must not be ENTER-modif'd twice when a callee's CALL modif vetoes the call | Complete |
| `018-012-reproduced_sleep_zero_hang.cpp` | Test | sleep(0)/sleep_us(0) must resolve immediately, not hang - self-watchdogged (alarm/SIGALRM) since the failure mode is a literal hang | Complete |
| `018-013-reproduced_signal_after_pool_clear.cpp` | Test | a sem_t that outlived its pool - cleared, or destroyed - must answer signal/signal_all/try_dec/clear with an error, not dereference its invalidated internals | Complete |
| `018-014-reproduced_killer_outlives_pool.cpp` | Test | a killer and its modif pack held past their pool must not free into it - the kill state, its shared_ptr count and the semaphore waiter handle it kept | Complete |
| `018-015-reproduced_timeo_read_drops_bytes.cpp` | Test | create_timeo(co::read(...)) on Windows must not drop bytes a read already moved (it lost 273/1000 before the killer redesign) | Complete |
| `018-016-reproduced_killer_drops_sem_token.cpp` | Test | a kill must not throw away a semaphore token already given to its target | Complete |
| `018-017-reproduced_killer_call_veto_stale_stack.cpp` | Test | a CALL modif vetoing a call must not leave the destroyed callee on a killer's call stack (a later kill would read the freed frame) - `018-015`/`018-016` are reserved by the killer redesign | Complete |
| `018-018-reproduced_sem_freed_by_its_waiter.cpp` | Test | a semaphore whose last owner is its own waiter must survive its clear() destroying that waiter (poisoning operator new/delete makes the use-after-free crash) | Complete (7) |
| `018-019-reproduced_generator_not_freed.cpp` | Test | a generator that co_yielded must be freed once it co_returns (a by-value parameter's copy lives in the frame) | Complete (8) |
| `018-020-reproduced_destroy_yielded_generator.cpp` | Test | destroying a generator that co_yielded must destroy only it (with its EXIT), not the caller it yielded to | Complete (9) |
| `018-021-reproduced_killed_generator_stale_value.cpp` | Test | was #11 (a killed generator's caller got the previous yielded value): now its first co_yield terminates | Complete (10) |
| `018-022-reproduced_killer_freed_generator_terminates.cpp` | Test | was #12 (a kill after the holder freed the yielded generator terminated): now its co_yield terminates first | Complete (10) |
| `018-023-reproduced_timeo_read_race_forced.cpp` | Test | 018-015's race with its order forced: the timer kills a reader whose IOCP read already took the byte; the byte comes back from that create_timeo (HEAD lost it 6/6) | Complete |

Status notes:
1. `011-003-modifs_lifecycle.cpp`: covers all 7 remaining `modif_e` types and `CO_MODIF_INHERIT_ON_CALL`.
   `CO_MODIF_INHERIT_ON_SCHED` (now `011-004`) and standalone `task_modifs`/`add_modifs`/`rm_modifs`
   coverage (now `011-005` for the explicit-target overloads, `018-001` for the no-arg self-target ones)
   were the remaining gaps here - both closed.
2. `018-011-reproduced_call_modif_failure_double_enter.cpp`: see `BUGS.md` #5 - confirmed defect,
   failing by design until `colib.h` was fixed (it now passes, see note 11). It doesn't assert, it `abort()`s (SIGABRT, exit 134):
   the double `ENTER` trips `dbg_check_modif_enter`'s "entered twice" check. Confirmed on Linux
   2026-08-17; the previous "unconfirmed" status was an artifact of the Windows dev box losing
   native hard-crash output, not of the bug being hard to reproduce.
3. `018-001-reproduced_modif_helpers_self_target.cpp` and `011-004-modifs_inherit_on_sched.cpp`: both
   had been `Complete` (correct test logic) while silently failing to *build* on Linux (g++ 11-13) -
   `co_await add_modifs(modif_pack_t{mod})` ICEs gcc (see `tests/CLAUDE.md`'s toolchain note); the
   Windows dev box never surfaced this since MSVC doesn't hit it. Fixed 2026-08-17 by hoisting the
   pack into a named local before the `co_await` in both files - both build and pass now. `Complete`
   was accurate about coverage but not about "actually compiles on every platform this suite targets"
   - worth remembering that gap can exist silently for any test authored on one platform only.
4. `018-012-reproduced_sleep_zero_hang.cpp`: was `BUGS.md` #3, "not yet reproduced" since the prior
   dev box couldn't exercise `COLIB_OS_LINUX`. Confirmed for real 2026-08-17 on Linux: a scratch
   build against the pre-fix `colib.h` genuinely hung `pool->run()` forever on `sleep_us(0)` (no
   crash, no assertion, just a hang - `timeout 5` was needed to kill it). The fix (already applied
   to `colib.h` by the time this test was written) short-circuits `sleep()` to `co_return ERROR_OK`
   immediately when the requested duration is 0, before either platform's timer backend is touched -
   confirmed to resolve in ~2ms post-fix. Entry removed from `BUGS.md` since it's fixed.
5. `005-001-io.cpp`: had been marked `Complete` on the unverified assumption that its failure was a
   pre-existing, environment-only limitation (no `WSAStartup()`/no raw-disk permissions) not worth
   investigating. That assumption was wrong, and cost real time before it got checked properly.
   Three separate, real bugs - all in this test file, none in `colib.h` - were found and fixed:
   - `main()` never called `WSAStartup()` (lost when this file was extracted from the old root
     `tests.cpp`, which did call it). Without it, every socket call failed immediately, which
     incidentally also masked the next bug below by preventing anything from running long enough to
     hit it.
   - `test8_io_pipe()`'s `pipe_client` was a *capturing* lambda-coroutine, scheduled fire-and-forget
     via `co::sched()`. When a lambda is also a coroutine, the coroutine frame stores a pointer back
     to the closure object as an implicit `this` - it does not copy captured members into its own
     frame, regardless of by-value vs by-reference capture. Since `test8_io_pipe()` (where the
     closure lived) could finish and be destroyed before the independently-scheduled `pipe_client`
     coroutine resumed, this was a genuine dangling-pointer crash - confirmed reliably reproducing
     (100% across multiple spaced-out runs once a port-reuse confound in earlier rapid-fire testing
     was controlled for) and confirmed fixed by passing the needed data as real coroutine
     *parameters* (which the standard does guarantee get copied into the coroutine's own frame)
     instead of captures. `test8_io_connect_accept()`'s `server` lambda had the identical pattern
     (capturing `client_conn` by reference) - fixed the same way, by capturing by value instead
     (didn't independently reproduce a crash, but was an identical latent bug).
   - The pipe name literal was missing a backslash (`"\\.\\pipe\\..."` → runtime string
     `\.\pipe\...`, one leading backslash; the correct named-pipe namespace prefix needs two:
     `\\.\pipe\...`), so `CreateNamedPipeA`/`CreateFileA` both genuinely failed with
     `ERROR_INVALID_NAME`. This went undetected because the file's `CHK_PTR()` checks (a plain
     truthiness check) don't catch `INVALID_HANDLE_VALUE` - the sentinel `CreateFile`-family
     functions actually return on failure - since it's non-null. Fixed the name literal, and added
     a proper `CHK_HANDLE()` macro to `tests_common.h`, used everywhere this file checks a `HANDLE`.
   With all three fixed, `005-001-io.cpp` passes deterministically (5/5 runs, properly spaced to
   avoid port-reuse contamination between runs). One remaining, known-environmental failure:
   `test8_io_device()` can't open `\\.\PhysicalDrive0` (`ERROR_FILE_NOT_FOUND` - this sandbox
   doesn't expose that device) - now correctly detected and reported (thanks to `CHK_HANDLE`)
   rather than silently continuing with an invalid handle; it doesn't fail the overall test since
   nothing asserts on `test8_io_device_cnt` at the end.
6. `018-017-reproduced_killer_call_veto_stale_stack.cpp`: see `BUGS.md` #6 - confirmed defect,
   failing by design until `colib.h` was fixed (it now passes, see note 11). It crashes (access violation, exit 139 under Git
   Bash) rather than asserting: the kill reads the vetoed callee's freed frame. Confirmed on Windows
   2026-09-24, on `colib.h` both before and after the killer redesign. `018-015` and
   `018-016` are left free for the redesign's own reproduced bugs.
7. `018-018-reproduced_sem_freed_by_its_waiter.cpp`: was `BUGS.md` #8, a heap use-after-free found
   with AddressSanitizer: a semaphore whose last owner was its own waiter got freed in the middle of
   its `clear()`. A plain build hides it, so the test poisons freed memory through its own global
   `operator new`/`delete`, which made the bug segfault. Fixed 2026-09-24 (the semaphore list change):
   `clear()` closes and detaches its waiters before destroying any of them. The test now passes,
   also under ASan, and stays as the regression check.
8. `018-019-reproduced_generator_not_freed.cpp`: was `BUGS.md` #9, a leak introduced during the
   killer rework: after a `co_yield`, `err` stayed `ERROR_YIELDED`, so a generator that later
   `co_return`ed was never destroyed. `cpp_yield_awaiter_t::await_resume()` resets `err` again; the
   test failed before that and passes now.
9. `018-020-reproduced_destroy_yielded_generator.cpp`: was `BUGS.md` #10: after a `co_yield` the
   generator still pointed at the caller it yielded to, so `destroy_state()` on it walked into that
   caller. `task<T>::await_resume()` now clears a yielded callee's `caller_state` (it has no parent
   until it is awaited again); the test failed before and passes now.
10. The killer's generator rule: a killer attached to a generator terminates the program at its
    root's first `co_yield` (`002-008`, `002-009`, `002-006`); killed before it, it is killed like
    any coroutine (`002-010`). Two earlier versions of the rule (a kill of an already yielded
    generator terminating) left states behind that produced `BUGS.md` #11 (a killed generator's
    caller got the previous yielded value) and #12 (a kill after the holder freed the generator
    terminated); with the terminate at the yield those states can't exist. Their tests
    (`018-021`, `018-022`), like `002-011`/`002-012`, now require the terminate at the first
    `co_yield`, so they fail if the rule is ever loosened.
11. `018-011` and `018-017`: were `BUGS.md` #5 (the caller got ENTER twice when a CALL modif refused
    the call) and #6 (the refused callee was freed without its EXIT, so a killer that saw its CALL
    kept pointing at it). Both from `task<T>::await_suspend()`'s refusal branch, which fired the
    caller's ENTER (`await_resume()` fires it too) and no EXIT: it now fires the callee's EXIT
    instead. Both tests passed without changes.
12. `004-003` .. `004-005`: `COLIB_ENABLE_MULTITHREAD_SCHED` didn't compile (was `BUGS.md` #7:
    `<mutex>`/`<atomic>` never included; `004-003` reproduced it as a build failure). With the
    includes, `004-004` (the debug checks on) found `BUGS.md` #14: a thread_sched()'d coroutine was
    moved into the ready queue without its SCHED, so its first ENTER aborted the debug checks. The
    pool now runs the SCHED callbacks when it takes those coroutines in, on its own thread. All
    three pass, also under ASan and repeated.
13. `002-013`: the kill sets its error inside the killed coroutine, but its caller's
    `task<T>::await_resume()` freed the frame without reading it, and `task<T>::get_err()` was a stub
    returning `ERROR_OK` (since 2024-10). `await_resume()` now keeps a callee's `err` in the awaited
    task when the callee ended without a value, and `get_err()` returns it. `create_timeo` uses it:
    its killer is on the task itself, and its internal coroutine alone decides the result from
    `get_err()` (the `decided`/`timer_firing` flags are gone). `003-003`, `018-005`, `018-006` and
    `018-015` check it, the last two also repeated.
14. `004-006`: the #14 fix ran the SCHED callbacks of thread_sched()'d coroutines while holding the
    handover's `std::mutex`, so a SCHED callback calling `thread_sched()` locked it twice on the
    pool's thread (MSVC aborts, 0xC0000409). Found while answering a question on
    `_take_thread_tasks()`; the test was written and seen failing before the fix, without a
    `BUGS.md` entry (found and fixed the same hour). The lock is now a `std::recursive_mutex` and the
    handover loop goes by index, so what a callback hands over is taken in the same pass. Measured:
    a recursive and a plain mutex cost the same here (MSVC and g++, uncontended and with 1-4
    producer threads).
15. GCC and symmetric transfer (found 2026-09-25, the first Linux ASan run, g++ 15.2 under WSL):
    colib's coroutine switches stay on a bounded stack only when the compiler turns them into jumps.
    GCC does that only with `-foptimize-sibling-calls` (on from `-O2`); at `-O0`/`-O1`/`-Og`, and
    with ASan's stack instrumentation, `001-001`'s million switches overflow the stack. The user
    ruled it a requirement, not a bug (no trampoline mode): it is documented in `README.md` and at
    the top of colib.h's documentation block. The reproduce test that forced it
    (`018-024`, `#pragma GCC optimize ("no-optimize-sibling-calls")`) was retired: it could only fail.

**76 test files + 1 common header + 3 makefiles = 80 files (76 complete, 0 failing by design, 0 stubs)**

**Note on `create_modif()`'s public signature:** as of the `018-010` fix, `create_modif<Type>(flags,
cbk)` no longer takes a `pool` parameter (previously `create_modif<Type>(pool, flags, cbk)`) - a
deliberate, accepted breaking change (see `BUGS.md` history / commit history), since `modif_t` never
actually had pool-bound members and the old signature was rarely used directly. All test files
updated to the new 2-argument form.

For remaining/uncovered features (not yet a test file at all), see `todo.md`.

---

## Test Organization

### Test Naming Convention
Tests follow the format: `MAJOR-MINOR-DESCRIPTION.cpp`
- **MAJOR**: category number, zero-padded to 3 digits (`001`, `018`, ...) — see Categories table above
- **MINOR**: sub-test number within that category, also zero-padded to 3 digits (`001`, `002`, ...)
- **DESCRIPTION**: Brief descriptive name of the test

### Build System Features
`make` (dispatching to `windows.makefile` or `linux.makefile`) provides:
- **Individual compilation**: `make TEST_NAME` compiles a specific test
- **All tests**: `make all` compiles and runs all tests
- **Unix target**: `make unix` compiles with Unix flags and links with kqueue (`linux.makefile` only)
- **Clean**: `make clean` removes all executables

### Test Output
Each test prints **PASSED** (green) or **FAILED** (red) with the test filename.

For what's left to do (including process/meta items like platform verification), see `todo.md`.
