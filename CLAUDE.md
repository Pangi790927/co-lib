# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repo is

**colib.h** is a single-header C++20 coroutine library (~7000 lines): epoll/IOCP/kqueue-based async
I/O, semaphores, timers, a custom allocator, and a "modifications" callback system for coroutine
lifecycle events. `README.md` covers usage and the public API; `tests/` is the library's test suite
(build/run commands and conventions in `tests/CLAUDE.md`).

## Bug workflow: reproduce first, fix second

When a suspected bug in `colib.h` is found or reported (not just a gap in test coverage - an actual
suspected *defect*), the test for it gets written and committed **before** the fix, not after:

1. Confirm the bug actually reproduces (write a minimal repro, run it, observe the wrong behavior).
2. Turn that repro into a proper test file in `tests/` (category 18, "Reproduced Bugs" - see
   `tests/progress.md`'s Category 18 note) *immediately*, in the same pass as step 1. It's expected to
   fail at this point - an assertion failure, or the process crashing outright for something like a
   UAF/double-free. That's fine; commit it failing anyway, alongside a `tests/BUGS.md` entry
   describing the bug.
3. Fix `colib.h`.
4. The same test file now passes, without needing to be rewritten (adjust its assertions only if the
   actual fix behaves differently than originally expected). It stays permanently as the regression
   check, and the `BUGS.md` entry gets removed.

Why: the test file is *how the bug gets caught the first time*, not just how a fix gets verified after
the fact. Writing it after the fix would only prove the fix works today - it wouldn't prove the test
would have caught the bug in the first place, and it loses the record of what the actual failure mode
looked like (assertion vs. crash, which line, under what conditions). A currently-failing `18-N` test
in the suite is a deliberate, visible marker of open work, not something to work around or skip.

This same reproduce-first pattern applies broadly, not just to `colib.h` internals - any time work in
this repo turns up a suspected bug (in `colib.h`, in the test infrastructure, anywhere), default to
writing the failing test before writing the fix.

## Roles & boundaries

- **Never touch git state. Ever.** No `add`, `commit`, `push`, `stash`, `checkout`/`restore`,
  `reset`, `branch`, `merge`, `rebase`, `tag`, or `remote` — none of it, under any circumstances, even
  if it looks safe/reversible/helpful in the moment. Read-only inspection (`status`, `diff`, `log`,
  `show`) is fine when needed to understand context. If something requires a git state change (e.g.
  comparing against a previous version of a file), find another way - e.g. a scratch copy outside the
  repo - never git itself.
- **`colib.h` logic is the user's to write.** The default flow is: the user may optionally ask for a
  suggestion first, the user writes the actual code, and the job here is to check it - read it
  closely for errors, bugs, wrong logic, edge cases, anything off - not to author changes to it.
  Don't edit `colib.h` logic unless explicitly asked to implement something specific; unprompted, the
  right output of looking at `colib.h` is findings (a review), not a diff.
  **Comments in colib.h are the one exception** - any comment text, not just `/*! @fn ... */`-style
  doc blocks (inline comments, `@warning`s, clarifying notes near tricky logic, etc.) is fair game to
  add or edit directly. Not a license to touch the code those comments describe. Every edit already
  goes through the tool-approval prompt before it lands and shows up in `git diff` afterward, so
  there's no need to separately narrate "here's what I changed" unless it's non-obvious - the user
  sees the real diff at approval time either way.
- **Documentation and `.md` files are generally this repo's job for Claude to own** - `tests/BUGS.md`,
  `tests/progress.md`, `tests/todo.md`, `README.md`, `CLAUDE.md` files, etc. Keep them current as
  understanding of the code changes, without being asked each time.
- **Test files** (`tests/*.cpp`) are also fair game to write directly, including the reproduce-first
  `18-N` bug tests described above - that's a case where writing the test *is* the review/checking
  work, not code-authoring on the user's behalf.

## Testing

See `tests/CLAUDE.md` for build commands (`make`, `make <target>.exe`/`.bin`, `make clean`), file
naming/category conventions, and assertion macros. Always build through the makefile rather than
invoking the compiler directly - it's not just a convenience wrapper, it's picked up real
platform-specific bugs in its own flag handling before (see its git history).

**Don't run `make clean` (or a full `make all`) as a reflex between every change.** `make`'s own
incremental rebuild is enough - `tests/windows.makefile`/`tests/linux.makefile`'s per-test rule
lists `../colib.h` and `tests_common.h` as prerequisites (fixed 2026-08-14 specifically so this
would be safe), so editing
colib.h correctly invalidates every test binary and a plain `make <target>.exe` rebuilds only what's
actually stale. Reach for `make clean` only when something's actually gone wrong (a build looks
inexplicably stale) - not as routine hygiene, and not "to be thorough" after every fix. One caveat:
if colib.h gets edited and `make` gets invoked within the same wall-clock second, the staleness check
can miss it (observed once - GNU Make on this setup appears to compare at second, not sub-second,
resolution) - not a concern in normal back-and-forth, but worth knowing if build+edit ever get
scripted in tight succession.
