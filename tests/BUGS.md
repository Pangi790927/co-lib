# colib.h - Observed Bugs

Bugs in `../colib.h` found while writing tests for it in this directory. Each entry: what's broken,
where, how it was found, and whether/how it blocks a test. Entries are removed once fixed — this file
only tracks currently-open bugs.

---

## 1. `CO_MODIF_WAIT_IO_CBK`'s doc comment has the LEAVE/WAIT ordering backwards

- **Where:** `../colib.h`, the `modif_e` enum doc comment for `CO_MODIF_WAIT_IO_CBK`: *"This is
  called when a corutine is waiting for an IO (after the leave cbk)."* — claims `LEAVE` fires before
  `WAIT_IO`.
- **Actual behavior:** the reverse. `io_awaiter_t::await_suspend` (colib.h ~4243-4261) calls
  `do_wait_io_modifs()` first, registers the fd with the pool, and only then calls
  `do_leave_modifs()`. `sem_awaiter_t::await_suspend` does the identical thing for `WAIT_SEM`/`LEAVE`
  (its doc comment just says "similar to wait_io", inheriting the same mismatch). On resume, the
  order is `ENTER` then `UNWAIT_IO`/`UNWAIT_SEM` — consistent with (mirrors) the suspend-side order,
  just reversed.
- **Not just a stale doc comment — the internal design pseudocode near the end of the file (colib.h
  ~6614-6644, the draft "docs folder" notes) independently describes the *same* backwards order as
  the public doc comment (`do_leave_modifs` then `do_wait_io_modifs` on suspend; `do_unwait_io_modifs`
  then `do_entry_modifs` on resume). Two independent sources agreeing with each other and disagreeing
  with the code is what makes this worth keeping open instead of just fixing the doc text outright.
- **However, the current (real) order is load-bearing, not accidental:**
  - `dbg_check_modif_wait_io`/`unwait_io` (colib.h ~6414-6440, under `COLIB_ENABLE_DEBUG_CHECKS`)
    assert `entered && !left` at the moment `WAIT_IO`/`UNWAIT_IO` fire. `dbg_check_modif_leave` is
    the only place that sets `left = true`. Swapping to LEAVE-then-WAIT_IO would trip that assertion
    immediately (`left` already `true` from the just-prior `LEAVE`).
  - `create_killer`'s manual replay for a coroutine caught mid-wait when killed (colib.h ~5938-5943)
    does `do_entry_modifs(); do_unwait_io_modifs(); do_leave_modifs();` — it re-`ENTER`s *before*
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

## 2. `sem_t::signal(0)` vs its doc: which one is right is still an open question

- **Where:** `../colib.h`, `sem_t::signal()`'s doc comment says *"If increment is 0 and the internal
  counter is less then or equal to 0 then it will awake all the waiters, else it does nothing."* The
  implementation (`sem_internal_t::signal()`) only checks `inc == 0 && val < 0` - misses `val == 0`,
  the ordinary resting state for a semaphore with waiters queued through normal `wait()` calls (which
  never touch `val` when it's not `> 0`).
- **Not yet classified as a code bug or a doc bug** - deliberately left unresolved (see the `TODO:
  BUG:` left directly on `signal()`'s doc comment). Fixing the code to match the doc would change
  observable behavior for `signal(0)` at `val == 0`; since nothing in this repo currently calls
  `signal(0)` at all, that risk is specifically about consumers outside this repo that aren't visible
  from here. Auditing existing callers before deciding which side to change.
- **Test status:** `018-007-reproduced_signal_zero_boundary.cpp` asserts the currently-documented
  behavior (wakes all waiters) and fails against the current implementation - left failing
  deliberately as a visible marker while this is unresolved, not because a fix is imminent. Once a
  side is chosen: either the implementation gets fixed and this test starts passing as-is, or the
  doc/test both get updated to describe the narrower, `val < 0`-only contract on purpose.

---

## 3. `sleep(0)` / any zero-duration timer hangs forever on Linux

- **Where:** `../colib.h`, `timer_pool_t::set_timer()` (~colib.h:3150-3162, the `COLIB_OS_LINUX`
  `timerfd`-based implementation). Already flagged by an inline `TODO` on `sleep()`'s declaration
  (colib.h:1492): `/* TODO sleep(0) hangs forever on Linux < <<< < < < << < < << < */`.
- **Actual behavior:** for a 0-duration request, the built `itimerspec` ends up with
  `it_value = {0, 0}`, which is passed straight to `timerfd_settime()`. Per `timerfd_settime(2)`:
  *"Setting the it_value member of new_value to zero disarms the timer"* - it does not mean "fire
  immediately." The `io_awaiter_t` then suspends waiting on a timer fd that will never signal, so
  the coroutine never resumes.
- **Platform-inconsistent, not a documented sentinel:** nothing in `sleep()`'s doc describes 0 as
  special, and the Windows backend (`SetWaitableTimer`, colib.h ~3589-3603) does the opposite - a
  0 due-time there is an absolute FILETIME timestamp in the deep past, so it fires immediately. So
  `sleep(0)` currently succeeds instantly on Windows and hangs forever on Linux.
- **Impact:** real, platform-specific hang - any caller (test or user code) that passes a
  computed/zero duration to `sleep()`/`sleep_us()`/`sleep_ms()`/`sleep_s()`/`create_timeo()` on Linux
  deadlocks that coroutine.
- **How found:** already present as an inline `TODO` comment on `sleep()`'s declaration; formally
  logged here 2026-08-15 after confirming via `timerfd_settime`'s documented semantics that it's a
  real defect (not intentional) and cross-checking the Windows path disagrees with it.
- **Test status:** not yet reproduced - this Windows dev box can't exercise the `COLIB_OS_LINUX`
  path directly (no WSL distro with `g++`/`timerfd` available, only the `docker-desktop` WSL
  distro). Needs a Linux environment to write the `18-N` reproduce-first test.

---

## 4. kqueue `io_pool_t::force_awake()` and `clear()` are unimplemented stubs

- **Where:** `../colib.h`, the `#if COLIB_OS_UNIX` kqueue-based `io_pool_t` (colib.h ~2725-2798,
  the backend used by `make unix`/`make unix_kqueue`).
  ```cpp
  error_e force_awake(const io_desc_t& io_desc, error_e retcode) {
      /* TODO: figure it out, for this and for the others, maybe I can find a way not to use
      a map */
  }
  error_e clear() {}
  ```
- **Actual behavior:** both bodies are empty. `force_awake` is declared to return `error_e` but has
  no `return` on any path - undefined behavior if the return value is ever used (and it is: see
  `pool_internal_t::stop_io()`, colib.h ~3895-3898, which returns whatever `force_awake` returns).
  Functionally it also just does nothing: contrast with the working epoll implementation
  (colib.h ~2975-2994), which walks the fd's waiters, pushes the matching ones onto `ready_tasks`
  with `retcode`, and removes them from the wait set.
- **Impact:** `force_awake` is what `stop_io()` uses, which is what `create_killer`'s unwind path
  uses to yank a coroutine off of I/O-wait when it's being killed (colib.h ~5924, `stop_io(*kstate->
  io_desc, ERROR_WAKEUP)`). On a kqueue build, killing or force-stopping a coroutine that's
  currently suspended waiting on I/O would silently do nothing - the coroutine just never wakes up.
  `clear()` (used by `io_pool_t::clear()`/pool teardown to force-awaken everything still waiting) has
  the same gap.
- **How found:** reviewing every `TODO`/stub in `colib.h` for open bug candidates, 2026-08-15.
- **Test status:** not yet reproduced - needs a kqueue-capable environment (BSD/macOS, or Linux with
  `libkqueue` via `make unix_kqueue`), not available on this Windows dev box.

---

## 5. (POSSIBLE, unconfirmed) Caller may get `ENTER`-modif'd twice when a `CALL` modif vetoes a call

- **Where:** `../colib.h`, `task<T>::await_suspend()`/`await_resume()` (~2662-2722).
  `await_suspend()` calls `do_leave_modifs(&caller...)`, then if `do_call_modifs(state)` (the
  callee's `CALL` modif) returns an error, it does `do_entry_modifs(&caller...); return caller;` -
  which resumes the caller directly at the same `co_await` point. But `task<T>::await_resume()`
  (the awaiter the caller's compiler-generated code calls next either way) unconditionally does
  `do_entry_modifs(h.promise().state.caller_state)` again - the same caller.
- **Suspected consequence:** the caller would be `ENTER`-modif'd twice with no intervening `LEAVE`,
  which is exactly what `dbg_check_modif_enter`'s "entered twice" assertion (colib.h ~6423, under
  `COLIB_ENABLE_DEBUG_CHECKS`) exists to catch - `abort()` on trip. Traced by hand while writing
  `docs/03_execution_model.md`'s call-vs-sched section; not yet confirmed by actually observing the
  abort - a `.exe` built clean (`018-011-reproduced_call_modif_failure_double_enter.cpp`,
  `COLIB_ENABLE_DEBUG_CHECKS true`) but running it in this session's shell didn't yield readable
  output before this got parked (native hard-crash output getting lost between Bash/PowerShell in
  this environment, not evidence either way on its own).
- **How found:** reading `task<T>`'s call-failure path for `03_execution_model.md`, 2026-08-15.
- **Test status:** `018-011-reproduced_call_modif_failure_double_enter.cpp` written and builds
  clean; **not yet run to confirm the abort actually happens** - needs a re-attempt in a shell that
  reliably surfaces a hard native-crash exit code/output before this can be treated as a confirmed
  bug (or ruled out).
