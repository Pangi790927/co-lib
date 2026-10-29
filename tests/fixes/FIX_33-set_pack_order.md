# Proposal: add_modifs keeps the pack's order (fix #33)

Status: [APPLIED 2026-10-29 on the user's word]

Apply verification, 2026-10-29: the test fails on the header before the fix (a flipped pair,
`CHK_BOOL(o == 'A')`) and passes on the header as applied, two runs of two on Windows and one on
Linux; the suites were not run, at the user's word ("no need to start tests for now"), so their
numbers come with the next run. The check table is the last item of "How it was checked".

## The bug

`add_modifs` copied the caller's pack into a `std::set<modif_p>` and handed that set to
`add_modifs_to_table`, which appends the pack's modifs into the per-slot vectors of the task's
`modif_table_t` in set iteration order. `modif_p` is `std::shared_ptr<modif_t>`, and `shared_ptr`'s
`operator<` compares the raw pointers, so the set's order is heap-address order. The wrong step is
the append loop of `add_modifs_to_table`: it walks the set, so the slot vector receives the modifs
sorted by address, and the callback engine later runs that vector front-to-back (opening callbacks)
or back-to-front (closing callbacks, "the closing ones run in reverse, so they nest like
destructors"). Modifs of different slots land in different vectors and never see each other; the
trigger is one `add_modifs` call carrying two or more modifs of the same slot whose heap addresses
invert the pack order — whenever the second modif of the pack sits at a lower address than the
first, it lands first in the vector and its callbacks run first.

The confirmed consequence: the invocation order for one pack is nondeterministic — decided by the
heap, not by the code. `repro/33-set_pack_order.md` (REPRODUCED, behavioral, two fresh processes:
92/200 and then 99/200 single-pack trials fired the ENTER callbacks B-then-A although the pack was
[A, B]), with an exact correlation: zero order-vs-address mismatches in both runs — every
single-pack trial ran B-then-A if and only if modif B sat below modif A. The control phase of the
same repro attached the same two tracers through two separate `add_modifs` calls and stayed A-then-B in
200/200 trials, including the 92/107 trials where B sat lower: identical addresses, opposite
outcome, so the single-call path is the difference. Re-verified fresh on the current tree (your
accept rework plus applied #06) before this fix was built: 102/200 B-then-A, zero mismatches
(`repro/fix/33/base/33-base-run.log`). Independent corroboration predates the repro: `repro/08` had
to attach its two same-slot WAIT_SEM modifs via two separate `add_modifs()` calls precisely to get
deterministic order.

Why the set was there at all — it is a dedup structure, twice over, never an ordering decision:

- building the set collapses a modif listed twice in one pack (it would otherwise attach twice), and
- `add_modifs_to_table`'s first loop erases from the set every modif the table already holds, so
  re-attaching an attached modif adds nothing.

Nothing in the library consumes the set's ordering as ordering. The other table consumers, checked
one by one:

| consumer                 | what it relies on                       | this fix                     |
|--------------------------|-----------------------------------------|------------------------------|
| `do_generic_modifs`      | slot vector order = attach order;       | unaffected, it only reads    |
|                          | closing pass walks it reversed          | the order                    |
| `inherit_modifs`         | parent's slot vectors in order, pushed  | unaffected, and its input    |
|                          | into the child in that order; its own   | order becomes deterministic  |
|                          | dedup is an `unordered_set`             | too                          |
| `rm_modifs_from_table`   | membership only (`has` on its own set,  | untouched                    |
|                          | `remove_if`) — never order              |                              |
| `get_task_modifs`        | collects slot by slot, vector order     | output order for one call    |
|                          | within a slot (introspection; the       | becomes pack order, not      |
|                          | tests read only `.size()`)              | address order                |
| `get_modif_table_sz`     | counts entries                          | untouched                    |
| `pool_internal_t::sched` | attach the `own_modifs` pack, then      | same sequence, the pack's    |
|                          | inherit the parent's table              | own order now kept           |

So the set's iteration order leaked into the slot vectors and nowhere else; replacing the container
of the add path reorders nothing except the within-one-call landing order, which is exactly the
defect.

## Where the fix lands

As applied on 2026-10-29, in the proposal's shape; the user's ruling on open question 2: the
duplicates stay absorbed.

| colib.h region                                | what changes                                       | adds/dels |
|-----------------------------------------------|----------------------------------------------------|-----------|
| `add_modifs_to_table`                         | takes the pack (`const modif_pack_t&`) instead of  | +8/-6     |
|                                               | the `std::set<modif_p>`; walks the pack in order,  |           |
|                                               | with a private set for the two dedups; a comment   |           |
|                                               | states the rule                                    |           |
| `add_modifs_to_state(pool, state, _mods)`     | drops the set construction, passes the pack        | +1/-2     |
| the self-target `add_modifs(_mods)` coroutine | same                                               | +1/-2     |

Total +10/-8. `add_modifs_to_table` is internal; its only two callers are the two above (checked
on the header of the day), so the signature change is contained. No declaration or doc comment
changes, `02_api.md` stays.

## The change

`diff -u` of the header right before this fix (2026-10-29, with fixes #11, #12, #14, #24, #32,
#38, #39, #40, #48 and `wait_msg()` in) against the header as applied.

Reasoning per hunk:

- `add_modifs_to_table`: the order comes from the pack, which is walked as given; the set exists
  only to answer "is this one already in the table" and "was this one already taken from this
  pack" (`fresh.erase(mod)` answers the second and consumes the entry in one step), so a repeated
  entry attaches once, at its first position, and a re-attached entry attaches not at all. A null
  entry lands nowhere, as before.
- the two callers: the set they built is gone; the pack goes through as the caller wrote it.

```diff
--- base/colib.h
+++ patched/colib.h
@@ -6360,15 +6360,19 @@
     co_return get_task_modifs(table);
 }
 
-inline void add_modifs_to_table(modif_table_p table, std::set<modif_p> mods) {
+/* The pack's modifs land in their slot vectors in pack order: the callbacks run in the order the
+modifs were added, so one call must not reorder the pack. An entry the table already holds, a later
+copy of an entry earlier in the same pack, or a null entry lands nowhere. 2026-10-29 */
+inline void add_modifs_to_table(modif_table_p table, const modif_pack_t& mods) {
     if (!table)
         return;
+    std::set<modif_p> fresh{mods.begin(), mods.end()};
     for (auto &cbk_table : table->table)
         for (auto &modif : cbk_table)
-            if (has(mods, modif))
-                mods.erase(modif);
+            if (has(fresh, modif))
+                fresh.erase(modif);
     for (auto &mod : mods)
-        if (mod)
+        if (mod && fresh.erase(mod))
             table->table[mod->type].push_back(mod);
 }
 
@@ -6388,8 +6392,7 @@
     if (!table)
         table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                 dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
-    std::set<modif_p> mods{_mods.begin(), _mods.end()};
-    add_modifs_to_table(table, mods);
+    add_modifs_to_table(table, _mods);
     if (get_modif_table_sz(table) == 0)
         table = nullptr;
 }
@@ -6427,8 +6430,7 @@
     if (!table)
         table = std::shared_ptr<modif_table_t>(alloc<modif_table_t>(pool, pool),
                 dealloc_create<modif_table_t>(pool), allocator_t<int>{pool});
-    std::set<modif_p> mods{_mods.begin(), _mods.end()};
-    add_modifs_to_table(table, mods);
+    add_modifs_to_table(table, _mods);
     if (get_modif_table_sz(table) == 0)
         table = nullptr;
     co_return ERROR_OK;
```

## The regression test

Landed as `tests/018-049-reproduced_set_pack_order.cpp` (Test105) on 2026-10-29; it was first
written as 018-041 / Test84, the names the older logs below carry. One comment line was rewrapped
to 100 columns at landing.

It attaches two tracers of the same slot in one `add_modifs` call, over many trials with heap
churn so the two land at inverted addresses in some of them, and requires the ENTER callbacks to
run A then B in every trial; a control attaches the same two through two separate calls.

On the header before the fix (a copy kept aside at apply time), built through the tests makefile:

```
Failed CHK_BOOL(o == 'A') err_str: The operation completed successfully.
[FAILED]: 018-049-reproduced_set_pack_order.cpp
```

On the header as applied, two runs of two on Windows and one on Linux (WSL Ubuntu, g++ 15.2; the
code is platform-independent):

```
single-pack: 21 trials, 8 with B below A, all A-then-B
[PASSED]: 018-049-reproduced_set_pack_order.cpp
```

The file as landed:

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test105 - Reproduced Bugs: one add_modifs call ordered same-slot modifs by heap address
================================================================================================= */

/* Was an open bug in add_modifs_to_table: it took the pack as a std::set<modif_p>, whose order is
the raw-pointer order of the shared_ptrs, and appended to the slot vectors in that set order. Two
same-slot modifs attached via ONE add_modifs call landed in heap-address order, not in the order
the caller wrote the pack, so which callback ran first - opening order, and via the reversed
closing pass the closing order too - was decided by the heap, not the code
(repro/33-set_pack_order.md: B-then-A in ~half the trials whenever the second modif sat at a
lower address; two separate calls,
the control, were the only deterministic way). Now fixed: the pack is walked in pack order and the
set only remembers which entries must not land at all (already attached, or an earlier copy in the
same pack).

Each trial churns the heap first (same-size-class blocks with a random subset freed, so the second
tracer's allocation can land below the first), then attaches two fresh ENTER tracers, A created
before B, to a trivial task in ONE call and records the ENTER order. The trial loop stops once the
trigger precondition (B allocated below A) has occurred a few times, so a pass means the order held
in the very trials the bug reordered. The control attaches the same two tracers through two separate
add_modifs calls and must stay A-then-B as it always was.

The pack is a named local, never a temporary inside a co_await (tests/CLAUDE.md's gcc-ICE note);
add_modifs here is the explicit-target plain function, no co_await involved. */

#include <cstdint>
#include <memory>
#include <vector>

static std::string test105_log; /* ENTER tracers append their letter, in invocation order */

static co::modif_p test105_tracer(char id) {
    return co::create_modif<co::CO_MODIF_ENTER_CBK>(co::CO_MODIF_INHERIT_NONE,
            [id](co::state_t *) -> co::error_e {
                test105_log += id;
                return co::ERROR_OK;
            });
}

static co::task_t test105_noop() {
    co_return 0;
}

/* deterministic LCG, so a failure's churn sequence replays from the fixed seed */
static uint32_t test105_lcg(uint32_t &seed) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* Heap churn before A/B creation. Seed blocks around the make_shared block size
(sizeof(modif_t) + ~16 bytes of control block) so freed slots land in the size class the tracers
allocate from; free a random subset (at least two), keep the rest alive across A/B creation, plus
some varied-size noise. */
static void test105_churn(uint32_t &seed, std::vector<std::unique_ptr<char[]>> &alive) {
    const size_t modif_blk = sizeof(co::modif_t) + 16;
    std::vector<std::unique_ptr<char[]>> seed_pool;
    int n_seed = 8 + (int)(test105_lcg(seed) % 16);
    for (int i = 0; i < n_seed; i++) {
        size_t sz = modif_blk + 16 * (size_t)(test105_lcg(seed) % 4);
        seed_pool.emplace_back(new char[sz]);
        seed_pool.back()[0] = (char)i;                        /* touch the page */
    }
    int freed = 0;
    for (size_t i = 0; i < seed_pool.size(); i++) {
        if ((test105_lcg(seed) & 1) && freed < (int)seed_pool.size() - 2) {
            seed_pool[i].reset();
            freed++;
        }
    }
    for (int i = 0; freed < 2 && i < 2; i++) { /* guarantee at least 2 free same-class slots */
        seed_pool[i].reset();
        freed++;
    }
    for (auto &b : seed_pool)
        if (b)
            alive.push_back(std::move(b));
    int n_noise = 2 + (int)(test105_lcg(seed) % 6);
    for (int i = 0; i < n_noise; i++) {
        size_t sz = (size_t)16 << (test105_lcg(seed) % 11); /* 16 .. 16K, spread over classes */
        alive.emplace_back(new char[sz]);
        alive.back()[0] = (char)i;
    }
    { /* one immediate alloc+free pair for good measure */
        size_t sz = 64 + test105_lcg(seed) % 512;
        auto tmp = std::make_unique<char[]>(sz);
        tmp[0] = 1;
    }
}

/* One trial: creates A then B after a churn, attaches them to a fresh trivial task - one named
pack when single_pack, two calls otherwise - runs the pool, returns 'A' or 'B' by ENTER order and
reports through b_lower_out whether B sat at a lower address than A. */
static char test105_trial(int idx, bool single_pack, uint32_t &seed, bool *b_lower_out) {
    auto pool = co::create_pool();
    std::vector<std::unique_ptr<char[]>> alive;
    test105_churn(seed, alive);

    auto a = test105_tracer('A');
    auto b = test105_tracer('B'); /* always created in this order, nothing in between */

    co::task_t t = test105_noop();
    if (single_pack) {
        co::modif_pack_t pack;
        pack.push_back(a);
        pack.push_back(b);
        t = co::add_modifs(pool.get(), t, pack);
    }
    else {
        co::modif_pack_t pack_a; /* two separate calls: land in call order */
        pack_a.push_back(a);
        co::modif_pack_t pack_b;
        pack_b.push_back(b);
        t = co::add_modifs(pool.get(), t, pack_a);
        t = co::add_modifs(pool.get(), t, pack_b);
    }

    test105_log.clear();
    pool->sched(t);
    pool->run();

    bool b_lower = b.get() < a.get();
    if (b_lower_out)
        *b_lower_out = b_lower;
    DBG("[%s] t%03d order=%s addrA=%p addrB=%p b_lower=%d", single_pack ? "pack" : "ctrl",
            idx, test105_log.c_str(), (void *)a.get(), (void *)b.get(), b_lower ? 1 : 0);
    return test105_log == "AB" ? 'A' : 'B';
}

int test105_pack_order() {
    const int MAX_TRIALS = 300;
    const int WANT_B_LOWER = 8; /* stop once the trigger precondition occurred this often */
    uint32_t seed = 0xC0FFEE11u;

    /* one call: every trial must run A-then-B, B-below-A trials included */
    int b_lower_seen = 0;
    int trials = 0;
    for (; trials < MAX_TRIALS && b_lower_seen < WANT_B_LOWER; trials++) {
        bool b_lower = false;
        char o = test105_trial(trials, true, seed, &b_lower);
        ASSERT_FN(CHK_BOOL(o == 'A'));        /* pack order, whatever the addresses */
        if (b_lower)
            b_lower_seen++;
    }
    DBG("single-pack: %d trials, %d with B below A, all A-then-B", trials, b_lower_seen);
    ASSERT_FN(CHK_BOOL(b_lower_seen > 0));    /* the reordering precondition did occur */

    /* control: two separate calls keep the documented call order */
    for (int i = 0; i < 40; i++) {
        bool b_lower = false;
        char o = test105_trial(i, false, seed, &b_lower);
        ASSERT_FN(CHK_BOOL(o == 'A'));
    }
    return 0;
}

int main() {
    int ret = test105_pack_order();
    print_test_result("018-049-reproduced_set_pack_order.cpp", ret >= 0);
    return ret;
}
```

## How it was checked

All builds ran in `repro/fix/33/` (patched copy) and `repro/fix/33/base/` (pristine copy of the
current colib.h), with `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I.`, per the fix spec.
The diff of base vs patched copy is exactly the three hunks above (`repro/fix/33/33-fix.diff`,
+10/-8). The repro needed its verdict rule turned to the fixed behavior (the original asserts that
single-pack order FOLLOWS the address order — its mismatch check would count every fixed trial as
an anomaly), so the workspace copy asserts the fixed contract instead; no trial count, churn, or
control was weakened, and the baseline below ran the ORIGINAL unmodified verdict.

1. Repro fails on the unpatched copy — fresh run on the current tree, matching the recorded verdict:
   ```
   summary: single-pack AB=98 BA=102 other=0 b_lower=102 order-vs-addr mismatches=0
            control    AB=200 BA=0 other=0 b_lower=99 (control must stay AB regardless)
   FAIL-REPRO: single-pack ENTER order was B-then-A in 102/200 trials - the pack order is silently
   reordered by shared_ptr address via std::set<modif_p>
   ```
   (`repro/fix/33/base/33-base-run.log`, exit 1)

2. Repro passes on the patched copy, twice (exit 0 both), address correlation gone, control
   unchanged:
   ```
   summary: single-pack AB=200 BA=0 other=0 b_lower=97 (all b_lower trials must be AB)
            control    AB=200 BA=0 other=0 b_lower=77 (control must stay AB regardless)
   PASS: all 200 single-pack trials ran A-then-B, including the 97 where B sat at a lower address
   than A (the exact condition that reordered B-first before the fix); the control's 77 lower-B
   trials stayed A-then-B too. Pack order and call order, not addresses, decide.
   ```
   (run 1: 97 B-below-A; run 2: 88 — `repro/fix/33/33-fixed-run1.log`, `33-fixed-run2.log`)

3. The regression test above, run against both copies (plain build):
   - base copy: FAILED at trial t001 —
     `[pack] t001 order=BA addrA=...7950 addrB=...7560 b_lower=1`, then
     `Failed CHK_BOOL(o == 'A')` (`repro/fix/33/base/018-041-base-run.log`)
   - patched copy: PASSED, exit 0 —
     `single-pack: 27 trials, 8 with B below A, all A-then-B` + `[PASSED]`
     (`repro/fix/33/018-041-patched-run.log`)

4. No-regression, the three closest existing tests against the patched copy (include changed to
   `"colib.h"`, nothing else): all PASSED, exit 0 —
   - `011-005-modifs_standalone_explicit.cpp` (explicit-target add/rm_modifs, task_modifs)
   - `011-007-modifs_close_order.cpp` (the order contract: two packs, opening order and reversed
     closing order)
   - `018-001-reproduced_modif_helpers_self_target.cpp` (no-arg self-target add/rm_modifs)

5. Composition with fix #08 (same modifs-engine row): a merged copy `repro/fix/33/colib-0833.h`
   carries both diffs, and both sides were run against it:
   - fix #08's regression test (its two-separate-calls pattern, reconstructed verbatim from
     `FIX_08-suspended_override.md`): PASSED on the merged copy — the workaround-based pattern
     stays valid, and the WAIT_SEM vector order it builds is exactly [A, B] as it requires
     (`repro/fix/33/018-029-fix08-0833-run.log`).
   - this fix's repro: PASSED on the merged copy, 200/200 A-then-B with 89 B-below-A trials
     (`repro/fix/33/33-0833-run.log`).
   - fix #08's test on this fix ALONE (patched copy, no 08) fails at fix #08's own assertion
     (`order: AB after_wait: 1`, `Failed CHK_BOOL(test83_after_wait == 0)` —
     `repro/fix/33/018-029-fix08-on33-run.log`): fix #33 neither fixes nor masks fix #08's bug, and
     the separate-calls order under 33 was exactly what the workaround expects.

6. NOT-CHECKED: no Linux/kqueue build on this box (no make, per the spec). The changed region is
   platform-independent C++ — no `#if COLIB_OS_*` is involved, and every OS backend consumes the
   slot-vector order only through the same `do_*_modifs` wrappers — so reading is the validation
   there. Also, the trigger's heap behavior was exercised on this box's MSVC heap only; the
   regression test asserts its own precondition occurred (`b_lower_seen > 0`), so a heap that never
   inverts addresses would report that shortfall rather than pass vacuously.

At apply time, 2026-10-29, on the header of that day, in the applied shape:

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: one `add_modifs` call with two modifs of one slot, in   |
|                             | about half the trials, decided by the heap                   |
| DESIGN.md                   | the killer attaches its pack through the same call; its      |
|                             | modifs sit in different slots, so their order was never at   |
|                             | stake; no kill path changes                                  |
| epoll / kqueue              | platform-independent; the test builds and passes on Linux    |
|                             | (WSL, g++ 15.2); kqueue NOT-CHECKED (WIP)                    |
| contract / docs             | within one call, same-slot modifs run in pack order and      |
|                             | close in reverse pack order, as `011-007` asserts across     |
|                             | calls; stated in the function's comment; no declaration or   |
|                             | public doc changed                                           |
| callers                     | the two `add_modifs` paths; every combinator that attaches a |
|                             | pack goes through them                                       |
| pending fixes               | #16 and #30 touch the modifs engine elsewhere (the sched     |
|                             | refusal, the type bounds); no shared lines                   |
| runs                        | the test fails before (a flipped pair) and passes after (2   |
|                             | of 2 on Windows, 1 on Linux); the suites were not run, on    |
|                             | the user's word                                              |

## Interactions

The modifs-engine row of `repro/fix/00-FIXSTATUS.md` (08 30 33 16 18 19):

- #08 (an ERROR_SUSPENDED is final, applied 2026-10-14): no shared lines, and the two were
  composed and run together (step 5 above) — they hold. The semantic link runs the other way: #08 makes the callback order
  decisive (which callback returns first now decides ownership versus refusal, and #08's own open
  question 3 leans on "earlier in the vector" meaning something). With #08 applied, THIS fix is a
  correctness requirement, not a nicety: one mixed pack [parker, refuser] would otherwise have its
  ownership decided by heap addresses. repro/08's workaround (two separate `add_modifs` calls)
  remains valid — the control phase and #08's test on the merged copy both prove it — it just stops
  being the only way to get deterministic order. Apply in either order; the diffs don't touch the
  same lines.
- #30 (modif type OOB at the attach sites): FIX_30's proposal had not landed in tests/fixes/ when
  this was written (repro 30 is REPRODUCED; its fix direction, per the repro's promotion note and
  #08's interactions row, is a type bounds check at the `table->table[mod->type].push_back` sites —
  `add_modifs_to_table` and `inherit_modifs`). That is line-level overlap with this fix's first
  hunk: the append statement this fix reconditions is the statement #30 guards. The two compose
  mechanically in either application order — #30's range check nests beside this fix's
  `mod && fresh.erase(mod)` in the same `if` (or wraps the push) — and neither subsumes the other:
  30 decides what happens to an out-of-range type, 33 decides the order and the dedup of everything
  that lands. If #30's landed shape differs, the merge is still that one statement.
- #16 (sched refusal leak), #18 (killer rebind), #19 (external enter skipped): #18 consumes
  `rm_modifs_from_table` (membership only — untouched); #16 and #19 share no code with the attach
  path. Independent.

## Open questions for the user

1. The contract this fix states, made explicit: within one `add_modifs` call, same-slot modifs now
   run in pack order, and close in reverse pack order. That is what the engine's vector order and
   `tests/011-007` already assert across separate calls; this fix extends it to the one case that
   silently violated it. Code that inadvertently relied on the old within-call address order was
   relying on the bug — but it is a behavior change, so it is named here rather than assumed.
2. Both dedups of the old set are kept on purpose: a pack listing the same modif twice attaches it
   once (at its first position), and re-attaching an already-attached modif changes nothing. If you
   would rather have a duplicate attach be an error or a move-to-the-end, that is a separate
   contract decision this fix does not make.
3. `get_task_modifs`' output order for one call's same-slot modifs changes from address order to
   pack order (introspection only; nothing in the suite asserts on that order — the tests read
   sizes). Stated so the change is visible, not decided silently.
4. A one-line docs touch is NOT included: colib.h's own comments state the reverse-closing rule but
   never state the attach-order rule in words (it lives in 011-007's header). The new comment on
   `add_modifs_to_table` says it where it is implemented; if you want it in the public docs too
   (README.md / docs/), say so and it goes in at apply time.

Decided at apply time, 2026-10-29, on the user's word: (1) the contract stands as stated: pack
order within one call, reverse pack order on closing; (2) the duplicates stay absorbed, a
repeated modif attaches once at its first position and a re-attached one changes nothing; (3) the
introspection order follows; (4) no public doc change.
