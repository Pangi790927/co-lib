# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this directory is

Runnable example applications that pair 1:1 with `docs/06_*` tutorial chapters - **not** part of the
test suite in `tests/`. Where `tests/` proves `colib.h` behaves correctly (assertions, pass/fail,
`tests/BUGS.md`), `examples/` teaches how to *use* `colib.h` by building something real with it. No
`ASSERT_FN`/`ASSERT_COFN`, no `tests/BUGS.md` linkage, no `progress.md` category numbering - each
subdirectory is a small, self-contained, standalone program a reader could copy out of this repo and
build on its own.

Each numbered subdirectory (`06_0_chat/`, ...) corresponds to exactly one `docs/06_*_example_*.md`
chapter, which walks through that subdirectory's code. The chapter is the primary artifact for a
reader; the code here is what the chapter actually talks about - keep them in sync the same way
`docs/02_api.md` has an ongoing obligation to track `colib.h`'s declarations (see `docs/CLAUDE.md`).

## Conventions

- Self-contained: no dependency on `tests/` (not even `tests_common.h`) - a reader should be able to
  copy one `examples/06_N_*/` subdirectory out of this repo, keep its own copy of `colib.h` alongside
  it, and have it build. Shared helpers each example needs (error-checking macros, an `FnScope`-style
  RAII helper, protocol/parsing utilities) live in that example's own `*_common.h`, not a
  directory-wide shared header - duplication across examples is fine and preferred over coupling them
  together.
- Each subdirectory gets its own `makefile`/`windows.makefile`/`linux.makefile`, following the same
  OS-detecting-dispatcher shape as `tests/`'s (`make` picks the right one via `$(OS)`) - a reader
  should be able to `cd` into one example and run `make`, nothing more.
- Match `colib.h`'s and `tests/`'s existing conventions where they apply (doc-comment style,
  `COLIB_ENABLE_DEBUG_NAMES`, platform guards via `COLIB_OS_*`) so example code doesn't read as a
  different dialect from the library or its tests.
- Roles are the same as elsewhere in this repo (root `CLAUDE.md`): this directory's `.cpp`/`.h` files
  and the paired `docs/06_*` chapters are mine to write and keep current, same as `tests/*.cpp` and
  `docs/*.md` generally - `colib.h` itself is still the user's to write; an example only ever *uses*
  the public API, never modifies the library.
