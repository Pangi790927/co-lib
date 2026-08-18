# colib.h - Open TODOs and Design Questions

The `tests/BUGS.md` equivalent for open work that isn't a defect: design questions left unresolved in
`../colib.h` (inline `TODO` comments, deferred decisions), plus ideas raised in this directory's own
sessions that haven't been started. Entries are removed once resolved (decided against, implemented,
or folded into a chapter/`BUGS.md` if it turns out to be a real bug) - this file only tracks
currently-open items, same convention as `tests/BUGS.md`.

Confirmed defects still go in `tests/BUGS.md`, not here, even if they started as a `TODO` comment in
the code - see the root `CLAUDE.md`'s reproduce-first workflow. Several entries below cross-reference a
`BUGS.md` entry instead of duplicating it for exactly that reason.

---

## 1. `COLIB_OS_UNIX`/kqueue backend is incomplete

- **Where:** `../colib.h`, the `#if COLIB_OS_UNIX` block. Config-macro table row is itself flagged
  (`COLIB_OS_UNIX [TODO]`).
- **What's open:** `io_pool_t::force_awake()`/`clear()` are empty stubs (already `tests/BUGS.md` #4 -
  see that entry for the concrete impact: killing/force-stopping a coroutine parked on I/O silently
  does nothing on a kqueue build). Two smaller pieces in the same area, not yet worth their own
  `BUGS.md` entries since nothing exercises them yet:
  - `force_awake`'s own inline note: the intended fix is tracked in a `std::map`
    the way epoll's backend does it, with an open "maybe I can find a way not to use a map" design
    question - i.e. even the fix's *shape* isn't settled, not just its absence.
  - `dbg_to_str(const io_desc_t&)` for the kqueue variant returns
    `"NOT_IMPLEMENTED_TO_STR"` unconditionally - debug/tracing output for a kqueue build is a stub too.
- **Written up in `docs/05_platforms.md`** (see `progress.md`) - states this plainly as "not yet done"
  rather than implying three symmetric backends. This `TODO.md` entry stays open until the kqueue
  backend itself is actually finished, not just documented as incomplete.

---

## 2. Whether ASIO could back a fourth `io_pool_t`/`timer_pool_t` engine

- **Not a colib.h TODO - raised in a documentation-session discussion, 2026-08-15.** The question:
  could `boost::asio`/standalone Asio serve as the engine under `COLIB_OS_UNKNOWN` +
  `COLIB_OS_UNKNOWN_IMPLEMENTATION`, the same extension point that already exists for a fully custom
  backend?
- **Assessment reached:** yes, as a real from-scratch adapter, not a shortcut. `io_desc_t` would need
  to hold whatever per-operation ASIO state an op needs (an ASIO I/O object + buffers, not a bare
  fd+events pair); `add_waiter` would issue the matching `async_*` call whose completion handler calls
  `push_ready`; `handle_ready()` would become `io_context::run_one()`/`poll_one()` (matching the
  existing "no-op if `ready_tasks` non-empty, block only when truly idle" contract - see
  `03_execution_model.md`); `force_awake`/`stop_io` map to `cancel()`; timers map onto
  `asio::steady_timer`. A real fourth peer to epoll/IOCP/kqueue, following their existing shape.
- **Worked into a full proposal in `docs/05_1_APENDIX_asio.md`** - fills in `colib.h`'s own
  commented-out `COLIB_OS_UNKNOWN` reference template function-by-function, with the key refinement
  that this backend should mirror IOCP's shape specifically (stashed-closure `io_desc_t`, `add_waiter`
  issues the op) since Asio's `async_*` is completion-based like IOCP, not readiness-based like epoll.
  Written for the user to compare against their own idea - **no code implemented, not decided.**
- **Not started** (as actual code in `colib.h`).

---

## 3. Allocator: collapse two customization points into one

- **Where:** `../colib.h`, the `TODO` directly above the `COLIB_ALLOCATOR_REPLACE` defines - see
  `understanding.md`'s Allocator section for the full writeup, already documented there in detail.
- **Summary:** `COLIB_ALLOCATOR_REPLACE_IMPL_1`/`_IMPL_2` are two disjoint raw-code-splice points that
  must be kept consistent with each other by hand; the agreed-on fix is one named backend type with a
  small fixed contract (construct + `alloc(size_t)` + `free(void*)`), expressible as a C++20 `concept`
  for a real compile error instead of a deep template failure. Also needs rethinking the
  malloc-fallback pointer-range check that currently assumes a single contiguous blob.
- **Not started.**

---

## 4. `next_task_state()`: deduplicate the `ready_tasks.size() > 0` guard across backends

- **Where:** `../colib.h`, `io_pool_t::handle_ready()` - duplicated identically in the epoll, IOCP, and
  kqueue backends (inline `TODO` added at the epoll site during this same documentation pass,
  2026-08-15).
- **Agreed direction ("canon" per the same session):** move the check up into the one caller,
  `pool_internal_t::next_task_state()`, so it's written once instead of duplicated per-backend - see
  the session discussion for the reasoning (also closes off one way a half-finished backend, like
  kqueue's, can omit or misimplement the guard).
- **Not started** - comment-only marker in place; the actual code move is `colib.h` logic, the user's
  to make.

---

## 5. `pool_internal_t::run()`'s `ret_val = RUN_ABORTED` default isn't wired to real error collection

- **Where:** `../colib.h`, inside `pool_internal_t::run()`'s main loop: `ret_val = RUN_ABORTED;
  /* TODO: use this to somehow collect epoll and internal errors */`.
- **What's open:** the default is set defensively every iteration but nothing currently gathers a more
  specific error into it beyond what `next_task_state()` already sets on I/O failure
  (`RUN_ERRORED`) - the TODO reads as "this placeholder should eventually carry more diagnostic detail
  about *why* a run aborted," not yet designed.
- **Not started.**

---

## 6. Batching ready-task detection across `io_awaiter_t` backends

- **Where:** `../colib.h`, `pool_internal_t::has_next_task_state()`: "figure out if we want
  to maybe cache all the coroutines that wouldn't block, or more precisely add another function inside
  all io_awaiters(kqueue(still wip), epoll, iocp) to get all ready tasks and move them inside
  ready_tasks."
- **What's open:** a possible performance path where a backend could report *every* currently-ready
  event in one call instead of the current one-at-a-time-via-`handle_ready()` shape. No design
  proposed yet.
- **Not started.**

---

## 7. `external_init_task()`: what "kind" of spawn is an externally-driven task?

- **Where:** `../colib.h`, `external_init_task(task<T>, pool_t*)`: "what kind of spawn does this
  have? sched, call or maybe a new one, external?"
- **What's open:** tasks driven in via the `external_*` escape hatch (see `understanding.md`'s
  Externals section) don't cleanly fit the call-vs-sched dichotomy `03_execution_model.md` documents -
  they have no `caller_state` (like sched) but also weren't scheduled through the normal ready-queue
  path. Whether that needs its own tracked "origin kind" (e.g. for killers, which currently only
  understand call/sched) is an open design question, not yet decided.
- **Not started.**

---

## 8. `yield_awaiter_t`: is a modif replay needed on the self-yield case?

- **Where:** `../colib.h`, inside `colib::yield()`'s `await_suspend` (see
  `03_execution_model.md`'s `colib::yield()` section for the mechanism). A commented-out draft fix is
  left in place:
  ```cpp
  // TODO: is it required to call the modifs here if the returned coroutine is the same as
  // this one or does c++ call resume either way?
  // auto ret = pool->get_internal()->next_task();
  // if (ret == h)
  // 	do_entry_modifs(ret);
  ```
- **What's open:** when a coroutine calls `colib::yield()` and it happens to be the *only* ready task
  (so `next_task()` immediately returns the same coroutine it just pushed), does `ENTER` still need to
  fire by hand, or does the compiler's own resume machinery make it a no-op either way? Unresolved -
  the draft fix above was written but never enabled.
- **Not started.**

---

## 9. `wait_all()`: missing `COLIB_REGNAME`, and whether `sched` needs to become a `task`

- **Where:** `../colib.h`, `wait_all()`: "Maybe we want a COLIB_REGNAME here? This needs to
  be determined, either way, it would require the sched to be transformed into a task?"
- **What's open:** minor - debug-naming completeness for tasks spawned inside `wait_all`, blocked on a
  small refactor question about `sched`'s own return type. Low priority.
- **Not started.**

---

## 10. `COLIB_ENABLE_DEBUG_CHECKS` doesn't yet validate parameters, only internal state

- **Where:** `../colib.h`'s top `DOCUMENTATION` block, the Debugging section: "For additional checks
  on parameters(TODO) and internal state, enable COLIB_ENABLE_DEBUG_CHECKS..."
- **What's open:** the `(TODO)` tag flags that parameter validation (as opposed to the internal
  `entered`/`left`/`io`/`sem` state-machine checks `dbg_check_modif_*` already does - see
  `04_lifetimes.md`/`understanding.md`) isn't implemented yet. No specific parameters identified.
- **Not started.**

---

## 11. Unclear/possibly-stray comment on `COLIB_REGNAME`'s doc block

- **Where:** `../colib.h`, the `/*! @def COLIB_REGNAME */` doc block: `* TODO: This library uses it
  internaly if COLIB_ENABLE_DEBUG_NAMES is true. *) */` - the trailing `*)` doesn't match anything
  else nearby and the sentence doesn't read as a complete thought.
- **Not a design question - flagging for the user to look at.** This reads like a leftover fragment
  from an earlier edit rather than an intentional note; not confident enough in what it was meant to
  say to rewrite it myself (comments are fair game to edit, but not to guess-and-rewrite when the
  original intent is this unclear).

---

## Cross-referenced elsewhere (not duplicated here)

- **Killer-from-killer reentrancy** (the inline `TODO` in `create_killer()`'s implementation) - stated
  plainly as undefined behavior in `04_lifetimes.md`'s `create_killer()` section. No separate entry
  needed.
- **The pseudocode appendix** (colib.h, past `/* The end */`) - already described in this
  directory's own `CLAUDE.md` as superseded by this `docs/` directory itself; not tracked line-by-line
  here. It still contains a handful of open questions embedded in the pseudocode (in its `io_awaiter`
  `await()`, `sem_awaiter` `resume()`, `force_awake()` (Windows `awake_data`), and `create_killer()`
  `sig_kill()` sections) that were never carried forward into real tracked items - worth
  mining if anyone revisits that appendix, but treated as dead scratch material until then, not a live
  TODO list.
