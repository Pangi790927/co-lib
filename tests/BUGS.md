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
