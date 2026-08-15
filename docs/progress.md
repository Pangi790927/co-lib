# colib.h Documentation - Progress

Tracks each chapter's status. Mirrors the role of `tests/progress.md`'s Categories/Test Files
tables - see `CLAUDE.md` in this directory for the workflow. The chapter list itself is planned by
the user, not invented here ahead of time - this table only lists chapters that have actually been
requested/started.

Status values: **not started**, **in progress**, **done** (written and verified against the current
`colib.h`), **stale** (written, but `colib.h` has since changed underneath it - needs a re-pass).

**Date** and **Commit** record when a chapter was last checked against `colib.h` and what state
`colib.h` was in at the time: the base commit hash it was built on, plus a `+local` marker if
`colib.h` had uncommitted changes on top of that commit at verification time (it usually does -
`colib.h` is under active edit). This is what makes "stale" a checkable claim instead of a guess: if
`colib.h` has moved past the recorded commit/state, the chapter needs a re-pass.

---

## Chapters

| # | Chapter | Status | Date | Commit | Source material | Notes |
|---|---------|--------|------|--------|------------------|-------|
| 01 | `01_introduction.md` - Introduction to coroutines & this library | done | 2026-08-15 | `41fa8ac` +local | Most of `README.md` (Usage through Organization, not just the "Introduction" section), near-duplicated by `colib.h`'s top `DOCUMENTATION` block | Fully-introductory chapter; content moved here 2026-08-15. `README.md`/`colib.h` still hold their own copies - not yet repointed to this chapter (see `CLAUDE.md`'s "Keeping things in sync"). Later chapters go into detail on specific topics but don't shrink this one. |
| 02 | `02_api.md` - API Reference | done (first pass) | 2026-08-15 | `41fa8ac` +local | `colib.h`'s `HEADER` section (~line 376-2176): every public declaration + doc comment | Has an ongoing maintenance obligation - see `CLAUDE.md`. First pass covers every declaration in the header as of 2026-08-15; needs re-verification any time a signature or doc comment changes. |
| 03 | `03_execution_model.md` - Execution Model: call vs. sched, the scheduler, semaphores | done | 2026-08-15 | `41fa8ac` +local | `colib.h` implementation: task call/await machinery, `pool_t::sched`/`thread_sched`, the run loop, `sem_t`/`create_sem`/wait/signal | Requested by the user 2026-08-15: what actually happens on call vs. sched, what the scheduler does with ready coroutines, how semaphores fit into that flow. Verified line-by-line against the current implementation, including the caller_state->self-is-unconditional-on-a-real-caller point (double-checked mid-session per user pushback) and the pool_t::sched-vs-co_await-colib::sched modif-inheritance nuance. |

Further chapters to be planned with the user - not pre-listed here.
