# colib.h - Observed Bugs

Bugs in `../colib.h` found while writing tests for it in this directory. Each entry: what's broken,
where, how it was found, and whether/how it blocks a test. Entries are removed once fixed — this file
only tracks currently-open bugs. Entry numbers are stable: a removed entry leaves a gap rather than
renumbering the ones below it, since `docs/` cross-references them by number.

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

## 17. (CONFIRMED) `stop_io()` on a request whose completion was already taken queues it twice

- **Where:** `../colib.h`, the Windows `io_pool_t::force_awake()`, when it is given the request's own
  `io_desc_t` (with its `io_data_t`).
- **What happens:** if the request's completion packet was already taken (its coroutine is in the
  ready queue, not resumed yet), `CancelIoEx` finds nothing to cancel and `GetOverlappedResult`
  says the request is done, so `force_awake()` takes it as "completed during the cancel" and calls
  `awake_io()` again: the coroutine is pushed to the ready queue a second time. Since the intrusive
  queues, `state_list_t`'s guard against a coroutine being in two queues calls `std::terminate()`
  (before them it would have been resumed twice). The killer never gets here (it only stops a
  coroutine still `STATE_WAITING_IO`), neither does `stop_handle()` (a taken request is no longer
  registered under its handle); a user's own request built on the public `create_io_desc()` +
  `io_awaiter_t` and stopped through `pool_t::stop_io(desc)` does.
- **How found:** listed in `REDESIGN_KILLER.md`'s open points (2026-09-23), reproduced 2026-09-25.
- **Not fixed yet, on purpose** (the user's call). Same root as #16: an `io_data_t` doesn't know
  whether its completion was already delivered.
- **Test status:** `018-025-reproduced_stop_io_after_completion.cpp` reproduces it (order forced: the
  pool takes both completions together, the stopping coroutine runs first) and is **currently
  failing by design** (a terminate handler reports it).
