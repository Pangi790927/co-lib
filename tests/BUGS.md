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
- **How found:** while designing `011-003-modifs_lifecycle.cpp` (todo.md Category 11, remaining
  `modif_e` types), 2026-07-27.
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

## 3. `allocator_t<T>::deallocate()` use-after-frees if its pool was destroyed first

- **Where:** `../colib.h` ~3997, `allocator_t<T>::deallocate()` (its own TODO: *"modifs dereferenced
  by this will break if the pool dies, and since those are meant by the user, notok"*).
  `allocator_t<T>` holds a raw `pool_t*` captured at construction - not a `pool_p`/shared_ptr, so
  holding an `allocator_t<T>`-backed object does nothing to keep its pool alive.
- **Actual behavior:** `deallocate()` unconditionally dereferences that raw pointer
  (`pool->allocator_memory`) to decide whether the block being freed came from the pool's own
  bump/bucket allocator or was a plain `malloc()` fallback. A `modif_p` (returned by
  `create_modif()`, and explicitly meant to be held onto and reused across multiple
  `add_modifs()`/`rm_modifs()` calls - see `018-001`, `011-005`) can legitimately outlive the
  `pool_p` it was created from. If the pool is destroyed first and the `modif_p` is released
  afterward, `deallocate()` dereferences the already-destroyed `pool_t` through that stale raw
  pointer. Confirmed: `018-010-reproduced_allocator_deallocate_uaf.cpp` segfaults reliably on this
  build (unlike some other UB found in this codebase, e.g. the earlier `unlocker_t` null-`this`
  case, which happened not to fault - this one does, consistently).
- **Impact:** real, not hypothetical - any long-lived `modif_p` that outlives its pool (a pattern
  the API itself encourages by returning reusable `modif_p`/`modif_pack_t` values) crashes on
  release instead of cleaning up normally.
- **How found:** reviewing colib.h's own TODO on this line directly, 2026-08-14.
- **Test status:** `018-010-reproduced_allocator_deallocate_uaf.cpp` reproduces this - it currently
  fails (crashes the process) by design, per the reproduce-before-fix workflow described in
  `progress.md`'s Category 18 note and the root `CLAUDE.md`. It'll keep failing until colib.h is
  fixed; once it is, this entry gets removed and that test file becomes the regression check.
- **Fix not yet applied.**
