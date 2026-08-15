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

**When a `+local` entry's uncommitted changes get committed, drop the `+local` and update the hash to
the new commit** - *if and only if* nothing relevant changed between the two commits beyond what was
already verified (i.e. the commit is exactly "the local changes landed," not new edits on top). This
turns the Commit column into a precise "last actually modified in commit X" marker rather than a
vague "verified around here" one, which is what makes tracking a chapter's accuracy across commit
history possible at all. If something *did* change beyond the verified local diff, that's a fresh
edit - re-verify and record normally (new commit +`local` again if still uncommitted, or the new
commit alone if not).

---

## Chapters

| # | Chapter | Status | Date | Commit | Source material | Notes |
|---|---------|--------|------|--------|------------------|-------|
| 01 | `01_introduction.md` - Introduction to coroutines & this library | done | 2026-08-15 | `f86175a` | The end of `README.md` - "Introduction" through "Config Macros", excluding the front matter ("On AI"/"Usage"/"Versions") and "Organization" - byte-identical to `colib.h`'s top `DOCUMENTATION` block over that same range | Fully-introductory chapter; content moved here 2026-08-15. Scope corrected same day: "Organization" moved above "Introduction" in `README.md` (repo meta, not library docs) and cut from this chapter; `README.md`'s front matter was never in scope. Re-verified against `colib.h`'s `DOCUMENTATION` block same day - found and fixed real drift (missing `COLIB_ENABLE_DEBUG_CHECKS`/`COLIB_LOG_FUNCTION` rows, stale `COLIB_ENABLE_DEBUG_TRACE_ALL` wording, a tab-vs-spaces table misalignment in `colib.h` itself) - all three (`colib.h`, `README.md`, this chapter) now diff byte-identical over the shared range. `README.md`/`colib.h` still hold their own copies - not yet repointed to this chapter (see `CLAUDE.md`'s "Keeping things in sync"). Later chapters go into detail on specific topics but don't shrink this one. |
| 02 | `02_api.md` - API Reference | done (first pass) | 2026-08-15 | `5dde5f2` | `colib.h`'s `HEADER` section (~line 376-2176): every public declaration + doc comment | Has an ongoing maintenance obligation - see `CLAUDE.md`. First pass covers every declaration in the header as of 2026-08-15; needs re-verification any time a signature or doc comment changes. |
| 03 | `03_execution_model.md` - Execution Model: call vs. sched, the scheduler, semaphores | done | 2026-08-15 | `5dde5f2` | `colib.h` implementation: task call/await machinery, `pool_t::sched`/`thread_sched`, the run loop, `sem_t`/`create_sem`/wait/signal | Requested by the user 2026-08-15: what actually happens on call vs. sched, what the scheduler does with ready coroutines, how semaphores fit into that flow. Verified line-by-line against the current implementation, including the caller_state->self-is-unconditional-on-a-real-caller point (double-checked mid-session per user pushback) and the pool_t::sched-vs-co_await-colib::sched modif-inheritance nuance. |

| 04 | `04_lifetimes.md` - Lifetimes: coroutine frames, killers, semaphores, the pool, modifications | done | 2026-08-15 | `49232f1` +local | `colib.h`: `destroy_state()`, `create_killer()`'s unwind, `sem_t`/`create_sem`/`invalidate_self`, `pool_t`'s ctor/dtor/`clear()`, `modif_t`/`modif_table_t`/`create_modif`, plus the fixed `tests/018-010` UAF as a worked cautionary example | Requested by the user 2026-08-15, scope agreed in two passes: coroutine-frame/killer/semaphore-clear/pool-teardown lifetimes first, then pool/semaphore/modification lifetimes added on top. Verified line-by-line, including a key finding: `sem_t` and (after the 018-010 fix) `modif_t` are deliberately allocated via the global allocator, not the pool's bucket allocator, specifically because both are designed to outlive any one pool - the identical reasoning applied twice, once by design and once as a bugfix. |

Further chapters to be planned with the user - not pre-listed here.
