CLAUDE.md
=========

## Rules

1. Claude will **NEVER** modify **git state**. Examples: `git status` is allowed, `git add` never.

2. Claude will **not act** when asked "how" or "when" or a question in general; questions
should be **answered first**, and implementing comes when the user says so.

3. Claude should not make a change unless 95% sure it is **allowed** to do so.

4. Claude should not edit CLAUDE.md without an **express request by the user**, not only inferred
from the user's query. This is a **per-edit request**, so one time agreement is not a forever
mandate to modify the file.

5. C++ code (not the comments inside) is under a similar principle to rule 4: this time you can
ask the user for an edit (**ask permission**), but you are only allowed to write it with express
authorization (per edit request). You must **present** what you want to write.

6. **Tests are excepted** from rule 5, so you can write and run C++ code for tests. The tests will
stay in tests/ directory.

7. Rule 5 tells Claude to ask, not to try to implement workarounds for the missing C++ features.
Claude **should ask** the user for the **missing features** that it needs from C++ and if approved
by the user a proposal should be written by Claude and presented to the user.

8. **Any contradiction** with past messages or stance in general should be **explicitly resolved**.
Claude should present the contradiction to the user and ask him how to resolve it.

9. Don't duplicate code. **Use what is already there**; ask the user when something can be
simplified.

10. Don't write overly long functions. When a function can be split, **split it into components**.
Sure, if the function is a dispatcher, it is allowed to grow large, and that is ok, but a function
that does two separate things can be split into two functions instead.

## Writing style

How Claude writes in this repo — in comments and prose.
- Calm, **plain** prose.
- A description is a **proposition**, with a **subject** and a **predicate** — never a noun fragment
  — and it stands on its own, naming what the thing itself takes and does rather than leaning on the
  entry above or the file's internal vocabulary.
- When rewriting a description, **re-derive it from the code**: the old wording is a claim to
  verify, not a source of truth.
- On the flip side, current code behavior is not a design ruling. If something is unnatural to the
  new request, the user prefers to **fix the strange behaviour** rather than patch together things
  that don't fit.
- Comments on public functions should not contain internal behaviour; only the interface and the
  behaviour a caller can **observe or would care about** should be noted.
- A comment block stays attached to every public function; rewrite it tighter when touched,
  but the shape stays.
- 100 columns, everywhere — code and comments alike.
- Tables should be written **aligned**, even those in markdown files. Cells should wrap text inside
  them to fit.
- C++ comments are doxygen for all public-facing functions. (Python, Lua, etc. will emulate doxygen)

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
- **Test files** (`tests/*.cpp`) are also fair game to write directly, including the reproduce-first
  `18-N` bug tests described above - that's a case where writing the test *is* the review/checking
  work, not code-authoring on the user's behalf.

## INDEX

Above are the general rules. Here, instead, is the per-case documentation, on various subjects. This
is the only section of this file Claude may modify.

- `colib.h` is the library: a single-header C++20 coroutine library (async io over
  epoll/IOCP/kqueue, semaphores, timers, an allocator, modifs). The user owns its code (rule 5).
- `README.md` describes the usage and the public API of colib.h. Claude keeps it current.
- `tests/` holds the test suite, one standalone program per `.cpp`; its `CLAUDE.md` has the build
  commands and conventions. Claude owns the tests (rule 7) and keeps `BUGS.md` (the open bugs),
  `progress.md` and `todo.md` current without being asked.
- `tests/BUGS.md` lists the open bugs. A suspected bug gets its failing test (category 18) and its
  entry before the fix; the fix makes that test pass unchanged, and the entry is then removed.
- `docs/` holds the numbered-chapter user documentation; its `CLAUDE.md` has the conventions. Claude
  keeps it current; the user decides the chapter list.
- The root `.md` files other than this one explain one change each (a diff with its reasons);
  `review-colib.md` holds the user's review notes.
- `docs/ideas/` holds proposals and designs that are not decided or not started.
- `docs/old_drafts/` holds frozen old drafts, not updated. They go in with one commit and are
  deleted by the commit after it.


