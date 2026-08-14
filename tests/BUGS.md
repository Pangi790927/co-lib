# colib.h - Observed Bugs

Bugs in `../colib.h` found while writing tests for it in this directory. Each entry: what's broken,
where, how it was found, and whether/how it blocks a test. Entries are removed once fixed — this file
only tracks currently-open bugs.

---

## 1. `CO_MODIF_WAIT_IO_CBK`'s doc comment has the LEAVE/WAIT ordering backwards

- **Where:** `../colib.h`, the `modif_e` enum doc comment for `CO_MODIF_WAIT_IO_CBK`: *"This is
  called when a corutine is waiting for an IO (after the leave cbk)."* — claims `LEAVE` fires before
  `WAIT_IO`.
- **Actual behavior:** the reverse. `io_awaiter_t::await_suspend` (colib.h ~4188-4200) calls
  `do_wait_io_modifs()` first, registers the fd with the pool, and only then calls
  `do_leave_modifs()`. `sem_awaiter_t::await_suspend` (colib.h ~4364-4369) does the identical thing
  for `WAIT_SEM`/`LEAVE` (its doc comment just says "similar to wait_io", inheriting the same
  mismatch). On resume, the order is `ENTER` then `UNWAIT_IO`/`UNWAIT_SEM` (colib.h ~4204-4206,
  ~4375-4377) — consistent with (mirrors) the suspend-side order, just reversed.
- **Impact:** doc-only — doesn't affect correctness of code that doesn't rely on the documented
  ordering, but is actively misleading for anyone writing a modification that assumes LEAVE fires
  before WAIT_IO/WAIT_SEM.
- **How found:** while designing `11-3-modifs_lifecycle.cpp` (todo.md Category 11, remaining
  `modif_e` types), 2026-07-27.
- **Test status:** `11-3-modifs_lifecycle.cpp` asserts the actual (verified) order:
  `WAIT_SEM, LEAVE, ENTER, UNWAIT_SEM, WAIT_IO, LEAVE, ENTER, UNWAIT_IO, LEAVE, EXIT`.

---

## 2. No-arg `add_modifs()`/`rm_modifs()`/`task_modifs()` crash if scheduled instead of co_awaited

- **Where:** `../colib.h` ~5645, ~5694, ~5707 - all three read/write
  `(co_await get_state())->caller_state->modif_table`. `caller_state` is only ever set in
  `task<T>::await_suspend()` (colib.h ~2655), i.e. only when a coroutine is directly `co_await`-ed
  by another. `pool_t::sched()`/`co::sched()` never set it.
- **Actual behavior:** `pool->sched(co::add_modifs(mods))` (or the same for `rm_modifs`/
  `task_modifs`) instead of `co_await co::add_modifs(mods)` leaves `caller_state == nullptr`, and
  the dereference is an immediate access violation (confirmed - repro crashes with exit code
  `0xC0000005`).
- **Impact:** only misuse - every documented/intended call site `co_await`s these (see colib.h's own
  doc comment: `@return **Coroutine** that resolvs to: the adding of the modifiers`, i.e. it must be
  awaited). But nothing stops a caller from scheduling them like any other `task_t`, and the failure
  mode is a hard crash with no diagnostic instead of a controlled error.
- **How found:** while reviewing the caller_state fix for the no-arg modif helpers (see
  `18-1-reproduced_modif_helpers_self_target.cpp`), 2026-08-14.
- **Test status:** `18-6-reproduced_sched_no_arg_modif_helper.cpp` reproduces this - it currently
  fails (crashes the process) by design, per the reproduce-before-fix workflow described in
  `progress.md`'s Category 18 note and the root `CLAUDE.md`. It'll keep failing until the fix below
  lands; once it does, this entry gets removed and that test file becomes the regression check.
- **Fix not yet applied** - candidates: a debug-mode assert/check on `caller_state` (matching the
  `COLIB_DEBUG_CHECK_*` pattern already used for CALL/SCHED/LEAVE elsewhere in colib.h), or a
  no-op-with-logged-warning fallback when `caller_state` is null.
