# colib.h - Observed Bugs

Bugs in `../colib.h` found while writing tests for it in this directory. Each entry: what's broken,
where, how it was found, and whether/how it blocks a test. Entries are removed once fixed — this file
only tracks currently-open bugs. Entry numbers are stable: a removed entry leaves a gap rather than
renumbering the ones below it, since `docs/` cross-references them by number.

---

## 1. `CO_MODIF_WAIT_IO_CBK`'s doc comment has the LEAVE/WAIT ordering backwards

- **Where:** `../colib.h`, the `modif_e` enum doc comment for `CO_MODIF_WAIT_IO_CBK`: *"This is
  called when a corutine is waiting for an IO (after the leave cbk)."* — claims `LEAVE` fires before
  `WAIT_IO`.
- **Actual behavior:** the reverse. `io_awaiter_t::await_suspend` calls `do_wait_io_modifs()` first,
  registers the fd with the pool, and only then calls `do_leave_modifs()`.
  `sem_awaiter_t::await_suspend` does the identical thing for `WAIT_SEM`/`LEAVE` (its doc comment
  just says "similar to wait_io", inheriting the same mismatch). On resume, the order is `ENTER`
  then `UNWAIT_IO`/`UNWAIT_SEM` — consistent with (mirrors) the suspend-side order, just reversed.
- **Not just a stale doc comment — the internal design pseudocode near the end of the file (the
  `io_awaiter:` block of the draft "docs folder" notes, after `#endif /* COLIB_H */`) independently
  describes the *same* backwards order as the public doc comment (`do_leave_modifs` then
  `do_wait_io_modifs` on suspend; `do_unwait_io_modifs` then `do_entry_modifs` on resume). Two
  independent sources agreeing with each other and disagreeing with the code is what makes this
  worth keeping open instead of just fixing the doc text outright.
- **However, the current (real) order is load-bearing, not accidental:**
  - `dbg_check_modif_wait_io`/`unwait_io` (under `COLIB_ENABLE_DEBUG_CHECKS`) assert
    `entered && !left` at the moment `WAIT_IO`/`UNWAIT_IO` fire. `dbg_check_modif_leave` is
    the only place that sets `left = true`. Swapping to LEAVE-then-WAIT_IO would trip that assertion
    immediately (`left` already `true` from the just-prior `LEAVE`).
  - `create_killer`'s manual replay for a coroutine caught mid-wait when killed does
    `do_entry_modifs(); do_unwait_io_modifs(); do_leave_modifs();` — it re-`ENTER`s *before*
    `UNWAIT_IO` specifically because a parked coroutine currently sits at `(entered=false, left=true)`
    under the real ordering. Flipping the general order would flip this invariant too, and the
    replay would need to shrink to just `UNWAIT_IO; LEAVE` (a second `ENTER` there would trip the
    "entered twice" debug-check instead).
  - `io_awaiter_t::await_suspend` also only calls `do_leave_modifs()` *after* the OS-level
    `wait_io()` registration has succeeded — never before. If registration fails, the coroutine
    resumes synchronously without ever having "left". Flipping the order so `LEAVE` fires first
    would need a new failure path to walk that back (a synthesized `ENTER` to undo the speculative
    `LEAVE`) that doesn't exist today.
  - In short: reversing this to match the docs is a coordinated change across `io_awaiter_t`/
    `sem_awaiter_t`, `create_killer`'s replay, and the `dbg_check_modif_*` invariants (plus a new
    failure branch), not a one-line fix — see the session discussion for the full walkthrough.
- **Decision (2026-08-15):** not fixing right now either direction. If it happens, it'll be doc+code
  both moving to the LEAVE-before-WAIT_IO order (an internal-only change, no public API shift) -
  deliberately deferred, not forgotten.
- **Impact:** currently doc-only (the code is internally consistent with itself and with
  `create_killer`) - actively misleading for anyone writing a modification that assumes LEAVE fires
  before WAIT_IO/WAIT_SEM, but not a functional defect as things stand.
- **How found:** while designing `011-003-modifs_lifecycle.cpp` (todo.md Category 11, remaining
  `modif_e` types), 2026-07-27. Re-examined in depth 2026-08-15.
- **Test status:** `011-003-modifs_lifecycle.cpp` asserts the actual (verified) order:
  `WAIT_SEM, LEAVE, ENTER, UNWAIT_SEM, WAIT_IO, LEAVE, ENTER, UNWAIT_IO, LEAVE, EXIT`.

---

## 4. kqueue `io_pool_t::force_awake()` and `clear()` are unimplemented stubs

- **Where:** `../colib.h`, the `#if COLIB_OS_UNIX` kqueue-based `io_pool_t` (the backend used by
  `make unix`/`make unix_kqueue`).
  ```cpp
  error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
      /* TODO: figure it out, for this and for the others, maybe I can find a way not to use
      a map */
  }
  error_e clear() {}
  ```
- **Actual behavior:** both bodies are empty. `force_awake` is declared to return `error_e` but has
  no `return` on any path - undefined behavior if the return value is ever used (and it is: see
  `pool_internal_t::stop_io()`, which returns whatever `force_awake` returns). Functionally it also
  just does nothing: contrast with the working epoll implementation (the `COLIB_OS_LINUX`
  `io_pool_t::force_awake()`), which walks the fd's waiters, pushes the matching ones onto
  `ready_tasks` with `retcode`, and removes them from the wait set.
- **Impact:** `force_awake` is what `stop_io()` uses, which is what `create_killer`'s unwind path
  uses to yank a coroutine off of I/O-wait when it's being killed
  (`stop_io(*kstate->io_desc, ERROR_WAKEUP)`). On a kqueue build, killing or force-stopping a
  coroutine that's currently suspended waiting on I/O would silently do nothing - the coroutine just
  never wakes up. `clear()` (used by `io_pool_t::clear()`/pool teardown to force-awaken everything
  still waiting) has the same gap.
- **How found:** reviewing every `TODO`/stub in `colib.h` for open bug candidates, 2026-08-15.
- **Test status:** not yet reproduced - needs a kqueue-capable environment (BSD/macOS, or Linux with
  `libkqueue` via `make unix_kqueue`), not available on this Windows dev box.

---

## 5. (CONFIRMED) Caller gets `ENTER`-modif'd twice when a `CALL` modif vetoes a call

- **Where:** `../colib.h`, `task<T>::await_suspend()`/`await_resume()`.
  `await_suspend()` calls `do_leave_modifs(&caller...)`, then if `do_call_modifs(state)` (the
  callee's `CALL` modif) returns an error, it does `do_entry_modifs(&caller...); return caller;` -
  which resumes the caller directly at the same `co_await` point. But `task<T>::await_resume()`
  (the awaiter the caller's compiler-generated code calls next either way) unconditionally does
  `do_entry_modifs(h.promise().state.caller_state)` again - the same caller.
- **Consequence:** the caller is `ENTER`-modif'd twice with no intervening `LEAVE`, which is exactly
  what `dbg_check_modif_enter`'s "entered twice" assertion (under `COLIB_ENABLE_DEBUG_CHECKS`)
  exists to catch - `abort()` on trip.
- **How found:** reading `task<T>`'s call-failure path for `03_execution_model.md`, 2026-08-15.
- **Confirmed 2026-08-17** on Linux (g++ 13.1.0): `018-011-reproduced_call_modif_failure_double_enter
  .bin` aborts with `SIGABRT` (exit 134, core dumped). The earlier "unconfirmed" status was purely an
  artifact of the Windows dev box losing native hard-crash output between Bash/PowerShell - the abort
  is real and reproduces on the first run. Verified it is not a side effect of the `signal(0)` fix
  landed the same day: a build against the pre-fix `colib.h` aborts identically.
- **Test status:** `018-011-reproduced_call_modif_failure_double_enter.cpp` reproduces the bug and is
  **currently failing by design** (crashes rather than asserting) - it becomes the regression check
  once `await_resume()`'s unconditional `do_entry_modifs` is made conditional on the call having
  actually suspended.
