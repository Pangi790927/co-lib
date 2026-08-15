# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working in this directory.

## What this directory is

This is the documentation for `colib.h`, replacing the informal patchwork of the top-of-file
`DOCUMENTATION` block in `colib.h` and the root `README.md`. Both of those still exist and still
get read (Doxygen runs off the in-header block; GitHub renders `README.md`), but this directory is
where documentation content actually gets authored and maintained going forward - see "Keeping
things in sync" below for how the three stay consistent.

The `TODO: changed my mind, we will have a docs folder...` comment near the end of `colib.h`
(the "docs folder" pseudocode appendix, past `/* The end */`) is what this replaces: that appendix
was a scratch spec for suspend/resume/kill semantics that ended up shipping in the header instead
of becoming real documentation. This directory is that unfinished intention, actually followed
through on.

This mirrors `tests/`'s directory structure and its `CLAUDE.md`/`progress.md` convention
deliberately - same idea, different subject matter (documenting behavior instead of verifying it).

## Layout

- `CLAUDE.md` - this file.
- `progress.md` - the chapter plan and each chapter's status (not started / in progress / done),
  same role as `tests/progress.md`'s Categories/Test Files tables.
- `TODO.md` - the `tests/BUGS.md` equivalent for open *design questions* and deferred work: inline
  `TODO` comments found in `colib.h`, and ideas raised in a documentation session that haven't been
  started. **Confirmed defects still go in `tests/BUGS.md`, not here** - that's the one place bugs are
  tracked regardless of what surfaced them; `TODO.md` cross-references a `BUGS.md` entry rather than
  duplicating it when a `TODO` comment turns out to describe a real defect.
- `understanding.md` - **not user-facing.** My own working notes: declarations copied out of
  `colib.h` next to my explanation of what they actually do, invariants I've had to reconstruct by
  reading the implementation, cross-references between parts of the system that aren't obvious from
  reading top to bottom. This exists so a future session (mine) can re-orient in this codebase
  quickly instead of re-deriving things like the modif ordering invariants from scratch every time.
  It's allowed to be rough, opinionated, and to say "I'm not sure why this is the way it is" - none
  of that is acceptable in a numbered chapter, all of it is fine here.
- `0N_<name>.md` - numbered chapters, the actual user-facing documentation. Two digits
  (`01`, `02`, ... - this library isn't getting to 100 chapters), padded so plain lexicographic sort
  matches reading order, same reasoning as `tests/`'s `NNN-MMM-*.cpp` numbering. `01_introduction.md`
  is the fully-introductory chapter, `02_api.md` is the API reference, and the rest (to be planned
  with the user, see `progress.md`) go into detail on specific topics beyond what the intro/API
  chapters cover. `01_introduction.md`'s own scope is narrower than "the whole README" - see
  "Keeping things in sync" below.

## The chapters vs. `understanding.md` vs. `TODO.md` vs. `BUGS.md`

Four different places knowledge about `colib.h` can end up, and they're not interchangeable:

- **A chapter** (`0N_*.md`) is for the user: correct, current, readable, no hedging. If something in
  the code is unclear enough that the chapter would need to hedge, that's a sign the question
  belongs in `understanding.md` (to work out) or `tests/BUGS.md` (if it turns out to be a real
  defect), not a sign to hedge in the chapter itself.
- **`understanding.md`** is for me: a place to think out loud about how something works before it's
  well enough understood to write the user-facing version, and a place to keep the "why" that
  doesn't belong in a reference doc but is exactly what's needed to avoid re-investigating the same
  thing next session (the WAIT_IO/LEAVE ordering investigation in `tests/BUGS.md` #1 is a concrete
  example of the kind of thing that belongs here in full, and only as a one-line pointer in a
  chapter, if at all).
- **`TODO.md`** is for open design questions and deferred work that aren't defects - `colib.h`'s own
  inline `TODO` comments, and ideas raised in a documentation session (like whether ASIO could back a
  custom `COLIB_OS_UNKNOWN` engine) that haven't been started. Not user-facing either; more scratch
  than `understanding.md` in one sense (it's a flat list, not prose explaining how something works)
  but with a narrower job: tracking *that* something is unresolved and *where*, not working out how
  the resolved system behaves.
- **`tests/BUGS.md`** is for defects - anything documenting work turns up that looks like a real bug
  in `colib.h` (not just an unclear comment) still gets logged there, per the root `CLAUDE.md`'s
  reproduce-first workflow, not here. Documenting `colib.h` honestly sometimes means writing down
  that a function's real behavior doesn't match its own doc comment; when that's a genuine defect
  question rather than a wording fix, it's a `BUGS.md` entry, and the chapter should describe the
  real (verified) behavior in the meantime, not the aspirational one.

## Keeping things in sync

`01_introduction.md` and `02_api.md` are the two chapters with a direct source-of-truth
relationship to something outside this directory:

- **`01_introduction.md`** is a verbatim mirror of the *end* of `README.md` - "Introduction" through
  "Config Macros" (Introduction, Library Layout, Task, Pool, Semaphores, IO Pool, Allocator, Timers,
  Modifs, Debugging, Config Macros). It deliberately excludes `README.md`'s own front matter
  ("On AI", "Usage", "Versions") and "Organization" - those are repo-level meta, not library
  documentation, and `README.md`'s "Organization" section lives above "Introduction" precisely so
  the boundary is visually obvious. This is also exactly where `colib.h`'s top `DOCUMENTATION`
  block starts and ends (it opens directly with "Introduction", same reasoning) - **all three
  copies (`colib.h`'s block, `README.md`'s range, this chapter) are meant to be byte-identical
  over that shared range**, and a change to any one of them (content or formatting, including
  ASCII-table column alignment - `colib.h` had a tab-vs-spaces misalignment in this block, found and
  fixed 2026-08-15) should be propagated to the other two in the same pass, not left to drift. This
  chapter is the intended eventual source of truth; `README.md` and `colib.h`'s copy aren't yet
  repointed to it (still hand-kept-in-sync copies - see the note at the top of `01_introduction.md`).
  Later chapters going into detail on a topic already covered here (e.g. a deeper Modifs chapter)
  don't shrink `01_introduction.md` - it stays the full introductory mirror regardless; detail
  chapters are additive, not a replacement for it.
- **`02_api.md`** mirrors `colib.h`'s `HEADER` section: every public declaration plus its doc
  comment, organized the same way the header organizes them (Pool & Sched, Externals,
  Modifications, Timing, Flow Control, platform-specific I/O, Debug Interfaces). This one has an
  ongoing maintenance obligation the others don't: **whenever a change to `colib.h` touches a
  declaration's signature or doc comment, update the matching entry in `02_api.md` in the same
  pass** - don't let it drift the way `README.md` and the in-header doc block already have (see the
  code-review discussion that prompted this directory: the kqueue backend being a stub was invisible
  from the documented structure precisely because nothing forced the docs to admit it).

## Workflow

Same shape as `tests/`'s: get the list of work out first, then do it one item at a time.

1. **Plan before writing.** When starting a new chapter (or a substantial revision of one), figure
   out its scope and add/update its row in `progress.md` before writing prose.
2. **One chapter at a time, verified against the code before being marked done.** Don't draft
   multiple chapters ahead of confirming the previous one is actually accurate - "accurate" here
   means re-reading the relevant `colib.h` implementation, not relying on memory of it from an
   earlier session or from `understanding.md` notes that might themselves be stale.
3. **Write `understanding.md` notes as you go, not as an afterthought.** If figuring out a chapter
   requires reconstructing an invariant or tracing an interaction across multiple structs (the kind
   of investigation the WAIT_IO/LEAVE ordering took), write that down in `understanding.md` while
   it's fresh, whether or not it ends up explained in the chapter itself.
4. **A real defect found while documenting goes in `tests/BUGS.md`, not here** - see above. **An open
   design question or deferred-work `TODO` found in `colib.h` goes in `TODO.md`** - add it there in
   the same pass it's found, not as an afterthought either.
5. **Update `progress.md`'s status for a chapter in the same pass it's finished or revised** - don't
   let it drift from what's actually been written and verified. Same for `TODO.md`: remove an entry
   the same pass it gets resolved (decided against, implemented, or reclassified as a real `BUGS.md`
   defect) - don't leave stale entries for things that are no longer actually open.

## Roles & boundaries

Same as the root `CLAUDE.md`: `colib.h` logic is the user's to write, comments in it are fair game
for me to add/edit directly, and this directory - like `tests/*.md` - is mine to own and keep
current without being asked each time. Nothing in this directory should ever require touching git
state; see the root `CLAUDE.md` for that boundary in full.
