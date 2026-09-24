# Proposal: bring the docs up to the 2026-09-24 colib.h

Status: **proposal, nothing applied.** The user wants the software changes first; the docs follow
if these changes survive the next ones. Until then, treat every place listed here as stale. Found
by searching `README.md`, `docs/` and colib.h's own doc blocks for what changed on 2026-09-24.

## What changed in colib.h (the facts the docs must follow)

- **Killer:** tracks its chain with `root`/`top` pointers, not a `call_stack`; no `sig_kill`/
  `kill_state_t`. A kill of a finished target returns `ERROR_FINISHED`; a reentrant kill (from the
  kill's own EXIT/UNWAIT callbacks) `ERROR_GENERIC`; a kill from inside its target throws
  `kill_self_t`; a killer attached to a generator terminates the program at its root's first
  `co_yield`. A target whose wait already had its effect is resumed inside the kill ("driven") up to
  its next wait; its root's caller resumes in the target's turn. A killer can be re-attached.
- **Modifs:** `CO_MODIF_YIELD_CBK`/`CO_MODIF_UNYIELD_CBK` (`co::yield()`, `co::force_stop()`),
  `CO_MODIF_RETURN_CBK` (hands control back to the caller: `co_return` and `co_yield`, the latter
  with `err == ERROR_YIELDED`). `co_yield` fires RETURN, no EXIT; EXIT means "it dies". A callback
  answering `ERROR_SUSPENDED` owns the coroutine; there is no PARKED callback. Closing callbacks
  run in reverse. SCHED: any non-OK return means not queued, the modif owns it.
- **Errors:** `ERROR_FINISHED = 3`, `ERROR_SUSPENDED = 2`.
- **Tasks:** `task<T>::get_err()` returns the err of a callee that ended without a value (a kill:
  the await returned `T{}`). A refused call fires the callee's EXIT, and the caller gets one ENTER.
- **Semaphores:** intrusive waiter lists; `sem_waiter_handle_p`, `invalidate_self()` are gone.
- **Pool:** finished roots free themselves at their final suspend (no `post_to_destroy`, no
  `final_awaiter_cleanup`); no parked list. `COLIB_ENABLE_MULTITHREAD_SCHED` compiles, and
  `thread_sched()`'d coroutines get their SCHED.
- **`create_timeo`:** its killer is on the task itself; the result comes from the task's
  `get_err()`; `t` can't be a generator.

## Proposed changes, per file

| File                          | What is stale                                   | Proposed change                        |
|-------------------------------|-------------------------------------------------|----------------------------------------|
| `docs/02_api.md`              | `modif_e` lists 9 callbacks; no YIELD/UNYIELD/  | Re-derive the `modif_e` section from   |
|                               | RETURN; SCHED "ignored"; `sem_waiter_handle_*`, | the enum; drop the removed semaphore   |
|                               | `invalidate_self`; `create_killer` quotes the   | types; quote the current killer doc    |
|                               | old "nothing to kill" warning and `sig_kill`    | (with its `-- false` marks); add       |
|                               |                                                 | `get_err()`, `kill_self_t`,            |
|                               |                                                 | `ERROR_FINISHED`/`ERROR_SUSPENDED`     |
| `docs/03_execution_model.md`  | `co_yield` described through EXIT and           | Describe `co_yield` as RETURN without  |
|                               | `final_awaiter_cleanup`; `posted_to_destroy`    | EXIT; roots freed at final suspend     |
| `docs/04_lifetimes.md`        | `create_killer()` section built on `call_stack`,| Rewrite the killer section around      |
|                               | `sig_kill`, `kill_state_t`; `invalidate_self`;  | root/top, the drive and the generator  |
|                               | `post_to_destroy`; `destroy_state` shape        | rule; semaphore lifetime without       |
|                               |                                                 | `invalidate_self`; frames without      |
|                               |                                                 | `post_to_destroy`                      |
| `docs/understanding.md`       | `call_stack`, `post_to_destroy`,                | Same facts as above, briefly           |
|                               | `final_awaiter_cleanup`, old `create_timeo`     |                                        |
| `README.md`                   | `co_yield` passages (4)                         | Check them against RETURN/EXIT; likely |
|                               |                                                 | only wording                           |
| colib.h, top `DOCUMENTATION`  | "Modifs" lists suspend/resume, call/sched, io   | Add yield/unyield, return, exit        |
|                               | and semaphore waits only                        |                                        |
| colib.h, pseudo-code at the   | `create_killer()` with `kstate.call_stack` and  | Rewrite `create_killer()`,             |
| end                           | `sig_kill`; `create_timeo()` with the old       | `create_timeo()` and the yield         |
|                               | decision; `yield_await` fires EXIT              | pseudo-code, or mark them stale        |
| `tests/readme.md`             | not checked                                     | Check against the list above           |

Also stale by the same changes: the `sig_killer` remark in `create_killer`'s doc block ("one kills
all (sig_killer installed in all)").
