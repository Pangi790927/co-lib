# colib.h - Observed Bugs

Bugs in `../colib.h` found while writing tests for it in this directory. Each entry: what's broken,
where, how it was found, and whether/how it blocks a test. Entries are removed once fixed — this file
only tracks currently-open bugs. Entry numbers are stable: a removed entry leaves a gap rather than
renumbering the ones below it, since `docs/` cross-references them by number.

---

No open entries. Entry 4 (the kqueue `force_awake()`/`clear()` stubs) left on 2026-10-29: the
kqueue backend is not implemented, so that is unfinished work, tracked in `docs/TODO.md` #1,
not a bug.
