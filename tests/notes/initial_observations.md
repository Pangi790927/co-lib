# Initial Observations - colib.h Analysis

Component descriptions, API details, and per-component test ideas originally drafted here have moved
to (or were superseded by) `readme.md` (API reference) and `todo.md` (per-feature test checklist) —
not duplicated below. This file keeps only what isn't captured elsewhere: testing challenges specific
to this library, and the resolution of early open questions.

## General Observations (2026-07-13)
- Header-only, ~6867 lines, zero external deps beyond std C++20 + OS headers, 20+ config macros,
  cross-platform (Linux/Windows/UNIX/custom).
- Public API is well-documented inline (Doxygen-style); implementation is the bulk of the file.

## Potential Challenges

1. **Platform-Specific Code** - tests need `#ifdef` guards; some tests only make sense on one OS.
2. **Async I/O Testing** - requires real sockets/files; network tests need port availability; no mock
   I/O layer exists in the library.
3. **Timing Tests** - sleep precision depends on hardware/OS load; assertions need tolerance, not
   exact millisecond checks.
4. **Memory Testing** - hard to verify allocator/leak behavior from C++ alone; would need
   valgrind/ASan since the custom allocator's internals aren't observable from the public API.
5. **Concurrency Testing** - `COLIB_ENABLE_MULTITHREAD_SCHED` introduces race conditions that are hard
   to reproduce deterministically; would need repeated/stress runs.

## Open Questions - Resolved

These were open when this file was first written; here's what actually happened:

1. **Test framework**: existing hand-rolled pattern (`tests_common.h`'s `ASSERT_FN`/`ASSERT_COFN`,
   `DBG`, `FnScope`) was used, not Google Test/Catch2.
2. **Test location**: tests live flat in `tests/` as `<major>-<minor>-<name>.cpp`, not under `tests/tests/`.
3. **Build integration**: a separate `tests/makefile` builds/runs all of them (see `CLAUDE.md`).
4. **Debug configuration**: every test file sets `COLIB_ENABLE_DEBUG_NAMES true`; `COLIB_ENABLE_LOGGING`
   is left at its default.
5. **Error handling convention**: `ASSERT_FN`/`ASSERT_COFN` treat a **negative `int` result** as
   failure (not "result != `ERROR_OK`" as originally speculated here — `ERROR_YIELDED == 1` is
   non-OK but not a failure).

*Last Updated: 2026-07-26*
