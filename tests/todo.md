# colib.h Testing - Remaining Work

Completed categories/items have been removed from this list — see `progress.md` for what the existing
test files cover and for the full category list. Category numbers here match test file MAJOR numbers
(see `progress.md`); categories with no number yet listed there don't have a test file at all.

---

### Category 5: I/O
(`5-1-io.cpp`, `5-2-io_wait_event.cpp`, `5-3-io_stop_fd.cpp`, `5-4-io_stop_handle.cpp`,
`5-5-io_stop_io.cpp` exist)
- [ ] Test `WSASend`/`WSARecv` variants (Windows-only; this environment is Linux, can write but not
      verify)

### Category 11: Modifications
(`11-1-modifs.cpp` covers `CO_MODIF_CALL_CBK`/`CO_MODIF_SCHED_CBK`; `11-2-modifs_await.cpp` covers
`co::await()`; `11-3-modifs_lifecycle.cpp` covers the remaining 7 `modif_e` types and exercises
`CO_MODIF_INHERIT_ON_CALL`)
- [ ] Test `CO_MODIF_INHERIT_ON_SCHED` specifically (only `ON_CALL` is exercised so far)
- [ ] Test `task_modifs()`, `add_modifs()`, `rm_modifs()` as standalone operations (removing a modif
      mid-flight, reading back a task's attached modifs)

### Category 13: External Awaitables (no test file yet)
- [ ] Test `external_init_task()`, `external_on_suspend()`, `external_on_resume()`,
      `external_sched_resume()`, `external_has_next_task()`, `external_wait_next_task()`

### Category 6: Debugging (extend `6-1-dbg_trace.cpp` or add files)
- [ ] Test `dbg_name()` with `COLIB_ENABLE_DEBUG_NAMES`
- [ ] Test `log_str()`, `dbg()`, `dbg_format()`
- [ ] Test `COLIB_ENABLE_DEBUG_CHECKS` assertions
- [ ] Test `COLIB_ENABLE_DEBUG_TRACE_ALL` tracing

### Category 14: Allocator (no test file yet)
- [ ] Test allocation/deallocation, bucket sizing, `COLIB_DISABLE_ALLOCATOR`, `COLIB_ALLOCATOR_SCALE`,
      `COLIB_ALLOCATOR_REPLACE`, memory leak detection

### Category 15: Configuration Macros (no test file yet)
- [ ] Test OS auto-detection (`COLIB_OS_LINUX`/`WINDOWS`/`UNIX`/`UNKNOWN`), `COLIB_ENABLE_MULTITHREAD_SCHED`,
      `COLIB_ENABLE_LOGGING`, `COLIB_LOG_FUNCTION`

### Category 16: Stress & Edge Cases (no test file yet)
- [ ] Max concurrent coroutines, deeply nested calls, rapid scheduling/cancellation, memory pressure,
      long-running coroutines, error recovery, thread safety under `COLIB_ENABLE_MULTITHREAD_SCHED`

### Category 17: Integration (no test file yet)
- [ ] Full application scenarios (network server/client, file I/O, mixed I/O+timing)

---

### Process / Meta
- [ ] Verify all tests compile and pass on Linux, Windows, and Unix — especially platform-specific
      ones (`5-1-io.cpp`, `5-3-io_stop_fd.cpp`, `5-4-io_stop_handle.cpp`)
- [ ] Decide: move these tests into the main repo (alongside `../tests.cpp`), or keep them in `tests/`?
- [ ] Add benchmark/stress tests for behavior under heavy load or many concurrent coroutines

## Notes
- Platform-specific tests should use appropriate `#ifdef` guards.
- Tests should cover both success and error paths, and edge/boundary conditions.
