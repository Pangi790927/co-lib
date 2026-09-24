# Killer work: state of bugs and TODOs

State on 2026-09-24. This covers the killer redesign and everything found while reviewing it.
Nothing in `colib.h` has been changed. The work lives in these files:

| File | What it is |
|---|---|
| `REDESIGN_KILLER.md` | The original proposal. **Out of date** wherever review changed it (see "Docs" below). |
| `killer_diff.md` | The reviewed `colib.h` diff for the killer (section 3), plus the `co_yield` fix (section 4). |
| `reversed_modifs.md` | The step after it. Part 1 reverses the order of the closing callbacks; part 2 fixes the order between kinds of callback (`BUGS.md` #1). |
| `state_queues.md` | The step after that: `state_e` in `state_t`, intrusive queues, and no pimpl pointer in `pool_t`/`sem_t`. |
| `combined_diff.md` | **The one to apply:** the four steps stitched into one `colib.h` diff plus one tests diff, with the killer simplified on top of the state (it reads `get_state()`, and `sched()`'s ENTER moves to the first run). |
| `tests/BUGS.md`, `tests/progress.md` | Open, confirmed bugs, and the test inventory. |

---

## 1. Decisions waiting on you

1. ~~**A second exception in the same resume.**~~ **Decided: it terminates.** It's in
   `combined_diff.md`, and a probe confirms it: before, the second exception was silently lost.
2. **Variant A or B of the lazy drop.** The diff is B. A no longer fits the new dispatch rule, so
   B stands unless you say otherwise.
3. **The caller's place in the queue: a placeholder or your "task before" anchor.** The diff uses
   a placeholder. The target's slot stays in the ready queue while the kill runs, and the caller
   takes it. Your idea (remember the task before it, and repair that when it's removed) gives the
   same order but needs the repair. Say if you want it instead.
4. **The fix for `BUGS.md` #6** (below). It depends on "close only the ones that opened". Also: do
   you want that rule now or in `docs/TODO.md` (`reversed_modifs.md`, "What's still left")?

---

## 2. Killer lifetime problems: what's solved and what isn't

**Solved by `killer_diff.md`** (each one is tested unless marked otherwise):

| Problem | How |
|---|---|
| A Windows read that completed before the kill loses its bytes: the completion was already dequeued (case A), or the cancel came too late (case B) | the target is resumed up to its next wait (`018-015`) |
| A semaphore token taken but never received | same (`018-016`) |
| `create_timeo`'s `exec_coro` and `timer_coro` killing each other, which becomes a self-kill once there's a drive | one decision point, `decided`/`timer_firing` (`003-003`, `018-015`) |
| `create_timeo` hangs when the timer's sleep fails | the sleep-error path signals `sem` too (not tested) |
| An aborted wait or a failed `wait_io()` leaves the killer's `io_desc` pointing at a dead awaiter | every WAIT gets its UNWAIT (not tested directly) |
| An EXIT callback returning an error before the killer's makes `unwind()` destroy the same frame twice | callbacks whose return is ignored always all run (not tested directly) |
| A destroyed frame whose `OVERLAPPED` the kernel may still write to, or whose completion packet arrives later | `force_awake` always waits for the cancel to settle (likely the unexplained access violation, **not confirmed**) |
| The parent of a killed child run from inside the kill | the parent takes the child's turn in the ready queue (`002-005`) |
| The scheduler running inside the kill when a driven root `co_yield`s | section 4 (the probe; no suite test yet) |
| Two roots finishing in one resume leak one frame (single destroy slot) | the posted destroy is a list |
| Self-kill | throws `kill_deferred_t`, dies at the next wait (`002-004`) |
| A coroutine destroyed while it's still in the ready queue: the pool later pops the freed frame (segfaults today) | `state_queues.md`: `~state_t()` unlinks it (`012-002`) |

**Not solved:**

| Problem | State |
|---|---|
| **A vetoed call leaves a freed frame on the killer's call stack; the next kill reads it** | **Confirmed, crashes today and after the redesign.** `BUGS.md` #6, `018-017`. It has the same shape as the July heap-corruption theory (a stale `state_t*` in `call_stack`). Whether your July crash went through this path is unknown. |
| A SCHED modif refusing a task after the killer's SCHED callback pushed it | the frame stays on the killer's stack and is never run. A kill destroys it (external-awaitable path), so it's probably harmless, but **not reproduced** |
| A target suspended in an external awaitable without `external_on_suspend` | it looks like it's executing, so the kill throws `kill_deferred_t`. Inside `create_timeo` that escapes `timer_coro` (`killer_diff.md` R4) |
| A target resumed through `external_sched_resume` | treated as having completed nothing: dropped |
| `COLIB_ENABLE_MULTITHREAD_SCHED` | `is_ready()` doesn't see `ready_thread_tasks`. A kill from another thread isn't covered |
| A kill called from outside `run()` | the posted work (destroy, lazy drop) waits for the next `run()` step |

So: every killer lifetime problem found so far is solved except the vetoed-call one (#6) and the
external-awaitable edge cases. The July crash should be re-checked on your side (in `mint_api`)
with the fixed build.

---

## 3. Bugs

### Confirmed, open (in `tests/BUGS.md`)

| # | Bug | Test | Fixed by |
|---|---|---|---|
| 1 | WAIT/LEAVE (and UNWAIT/ENTER) callback order crosses, the doc says otherwise | `011-003` asserts today's order | `reversed_modifs.md` part 2 |
| 4 | kqueue `force_awake()`/`clear()` are stubs | - | nothing yet |
| 5 | a vetoed call ENTERs the caller twice | `018-011` (failing) | nothing yet |
| 6 | a vetoed call leaves a dangling frame on a killer's stack | `018-017` (failing) | nothing yet, see decision 4 |
| 7 | `COLIB_ENABLE_MULTITHREAD_SCHED` doesn't compile (`<mutex>`/`<atomic>` never included) | none yet: it would break the whole build | two includes |

### Confirmed, not yet in `tests/BUGS.md` (they land with the redesign, reproduce-first)

| Bug | Test | State |
|---|---|---|
| `create_timeo(co::read(...))` drops bytes on Windows | `018-015` (in `REDESIGN_KILLER.md` §9.1) | fails today (273 of 1000 bytes arrived) |
| A kill drops a semaphore token already given | `018-016` (§9.2) | its last line uses `ERROR_FINISHED`, so it doesn't compile today. Drop that line first (R2) |
| A driven root that `co_yield`s runs the scheduler inside the kill | a probe only | appears only once the drive exists, and section 4 fixes it. Turn the probe into a test when landing |

### Suspected, not reproduced

- **The access violation** in one run of the original `018-015` reproduction. Leading guess: a late
  IOCP packet for a freed `io_data_t` (`ERROR_NOT_FOUND` without a wait). `killer_diff.md` removes
  that path. Check by running `018-015` under a debugger on today's `colib.h`.
- **Windows `handle_ready_events()`** sets `ERROR_OK` for every dequeued packet, even a failed
  one: a failed request reads as success with 0 bytes.
- **Linux `force_awake()`** wakes every waiter on the fd whose mask matches, not only the one being
  stopped (the others get a spurious `ERROR_WAKEUP`).
- **A `stop_io()` on a request whose completion was already dequeued** can queue its coroutine
  twice. The killer never does this, because it checks the ready queue first.
- **`003-002-sleep_duration` failed once:** `sleep(150ms)` measured 149 ms while four builds ran in
  parallel, and it passed 5 of 5 reruns. Either a 1 ms early timer or clock granularity; not
  investigated.

---

## 4. TODO, in landing order

### Step 1: reproduce first (tests only, no `colib.h` change)

- [ ] Add `018-015` as it is, and `018-016` without its last line. Add `BUGS.md` entries for both,
      and commit them failing.

### Step 2: `killer_diff.md` sections 3 and 4

- [ ] Apply the `colib.h` diff, sections 3 and 4.
- [ ] Update the existing tests: `002-002` and `018-005` expect `ERROR_FINISHED`, and `002-004`
      catches `kill_deferred_t`.
- [ ] Add the new tests: `002-003`, `002-004`, `002-005`, `011-006`, and the `co_yield` probe as
      a real test.
- [ ] Remove the `018-015`/`018-016` entries from `BUGS.md`.
- [ ] Settle decision 1 (the second exception) and add its test.
- [ ] Write a test for `co::stop_io` landing on an already-completed read: the data is delivered,
      and `stop_io` returns `ERROR_FINISHED`.
- [ ] Add R9: a `CO_MODIF_WAIT_YIELD_CBK` entry in `dbg_create_tracer`.
- [ ] Settle R4: `timer_coro` catches `kill_deferred_t`, or the doc names that exception.

### Step 3: `reversed_modifs.md`

- [ ] Part 1, plus `011-007`.
- [ ] Part 2, plus the `011-003`/`011-006`/`011-007` edits. Remove `BUGS.md` #1 and the "Flagged"
      note in `docs/02_api.md`.

### Step 4: `state_queues.md`

- [ ] Apply the diff, and add `012-002`.
- [ ] Build on Linux (epoll) and with kqueue: those backends aren't compiled on this machine.
- [ ] Follow-ups:
      - the killer reads `get_state()` instead of inferring from `entered`/`io_desc`/`sem`/`is_ready()`;
      - the debug checks become checked transitions on `state`, instead of their own map.

### Step 5: the open bugs

- [ ] `BUGS.md` #6 (decision 4), then #5. Both are on the vetoed-call path in
      `task<T>::await_suspend`/`await_resume`, so they're worth fixing together.
- [ ] `BUGS.md` #7: add the two includes, then its test.

### Docs, once the code lands

- [ ] `REDESIGN_KILLER.md` is out of date. Either mark it as superseded by `killer_diff.md`, or
      bring its names and rules up to date:
      - `ERROR_SUSPENDED`, `kill_deferred_t` (inherits from `std::exception`);
      - no `keep_completed`;
      - the dispatch rule (an error wins, the callbacks whose return is ignored all run);
      - the caller's turn;
      - the call-suspension ownership rule.
- [ ] Update the pseudocode at the end of `colib.h` for `create_killer`/`create_timeo`, the
      awaiters' park paths, and `force_awake`.
- [ ] `docs/04_lifetimes.md`, `create_killer()` section:
      - the "executing" case, `ERROR_FINISHED`, and the reentrancy rules;
      - resuming the target inside the kill, and the caller's turn;
      - R5: the second promise holds at the `co_await`, not for `create_timeo`'s caller.
- [ ] `docs/02_api.md`: `create_killer`, `create_timeo`, `error_e`, the modif section,
      `kill_deferred_t`, `CO_MODIF_WAIT_YIELD_CBK`, `ERROR_SUSPENDED`, and `co::stop_io`'s
      `ERROR_FINISHED`.
- [ ] `docs/03_execution_model.md`: the kill may resume the target before it destroys it.
- [ ] `docs/understanding.md`: the Timing and Flow Control summaries.
- [ ] `docs/TODO.md`: resolve "Killer-from-killer reentrancy", and add the WAIT/UNWAIT pairing
      rule if it's deferred.
- [ ] `tests/CLAUDE.md`: the modif type list and the `error_e` list (the two new codes are
      positive).
- [ ] `colib.h`: `wait_all`'s doc says "sig_killer installed in all", which isn't true. That's
      independent of the redesign.
