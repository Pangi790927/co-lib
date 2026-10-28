# FIX_DOC.md — the rules for writing a fix

A fix is one `FIX_NNN-<short_name>.md` file per change to `colib.h`. It is written for the user to
rule on and WAITS AT THE REPO ROOT; nothing in it is applied to `colib.h` until the user says so.
When the user says so, the diff is applied, the regression test lands in `tests/`, and the fix
file MOVES INTO THIS DIRECTORY (tests/fixes/) with an apply stamp — this directory is the archive
of applied fixes, nothing else. The file outlives the change — it is the record of what was
broken, why the fix is shaped the way it is, and what proves it.

## The shape of a fix file

The sections, in this order:

1. **Title and status.** `Proposal: <short name>` and a status line: `[WAITING FOR THE USER'S
   WORD]` while it waits, `[APPLIED <date> on the user's word]` once introduced (with the apply's
   verification appended under it).
2. **The bug.** A self-contained explanation of what was broken: the mechanism (the named
   functions and the exact wrong step), the trigger (the call sequence that fires it), and the
   confirmed consequence (the observed failure and how it was confirmed — an ASan report, a
   deterministic wrong value, a hang). A reader who has seen none of the underlying work must
   understand the bug from this section alone; pointers to the evidence logs are extra, not a
   substitute.
3. **Where the fix lands.** A table: the `colib.h` region, what changes there, and the
   adds/deletions count.
4. **The change.** The full unified diff against the tree it was built on, with a short reasoning
   paragraph per hunk. The code follows `colib.h`'s writing style: calm plain prose, comments as
   propositions, doxygen on public functions, 100 columns, flags as bitfields.
5. **The regression test.** The test that would catch this bug (or a similar one) in the future,
   in full, shaped to the `tests/` conventions (numbered name, category word in the name and the
   header comment, standalone `main`). It fails on the unfixed header and passes unchanged on the
   fixed one — both runs are shown, not claimed. The test lands in `tests/` when the fix is
   applied; its number is assigned at apply time (several proposals may carry the same
   placeholder number).
6. **How it was checked.** The validation, honestly: what was run (pasted output), and what was
   NOT-CHECKED with the reason (a backend that cannot compile on this machine is the usual one) —
   a fix that names its blind spots is worth more than one that claims none.
7. **Interactions.** The other fixes that touch the same region: whether they compose, subsume,
   or conflict, and in which order they should apply.
8. **Open questions.** Every contract decision the fix had to make, stated for the user to rule
   on. A fix that changes the public contract decides nothing silently.

## How a fix is validated before it is written

Every fix is built and run against a private copy of `colib.h` in a scratch workspace; the real
`colib.h` is never written before the user's word. The ladder: the repro (or the regression test)
fails on the unpatched copy; it passes on the patched copy; the closest existing tests still pass
on the patched copy. Anything that cannot run here is marked NOT-CHECKED in the file.
