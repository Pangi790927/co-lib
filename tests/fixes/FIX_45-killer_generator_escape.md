# Proposal: modifs that last one call (CO_MODIF_UNINHERIT_ON_YIELD) (fix #45)

Status: `[APPLIED 2026-10-12 on the user's word]`. The diff below is against commit 9a13a9a
("fix 02") with FIX_44 and FIX_47 applied, and its checks are in "How it was checked". It was
first proposed on 9dfe29e with FIX_02, before those two landed; applying it on them changed one
thing, told in "The change".

After the apply, `colib.h` is commit 9a13a9a with FIX_44, FIX_47 and the diff below, and `make` in
`tests/` builds and runs its 91 tests against it: `[PASSED: 91]/[FAILED: 0]`; under ASan, on a
scratch copy, `[PASSED: 91]/[FAILED: 0]`.

## The bug

A callee inherits its caller's ON_CALL modifs: `task<T>::await_suspend()` runs `inherit_modifs()`,
which adds every inheritable modif of the caller that the callee doesn't have yet. It only ever
adds. For a plain callee that is harmless: it has one caller, and it dies when it returns. A
generator outlives the call. It `co_yield`s back, stays alive, can be awaited by a different
coroutine, and still carries what it inherited from its first caller:

```
target awaits g      -> g's table: the target's killer
outsider awaits g    -> g's table: the target's killer + the outsider's modifs (nothing dropped)
```

The killer is where this hurts. It tracks its chain with `root` and `top`, and moves `top` in its
CALL and RETURN callbacks, which fire for any frame that carries them. Once the outsider awaits the
generator, the target's killer follows the generator into the outsider's chain while its `root`
stays the target. `repro/45-killer_generator_escape.md` confirms it under ASan in nine modes:

- the kill ends the generator and the outsider (or the outsider alone, or the generator's callee
  too) and returns `ERROR_OK`, while the target lives on;
- once the outsider frees itself, the next kill reads freed memory (heap-use-after-free);
- with a killer on each chain, the outsider's killer ends the target's chain;
- with the outsider suspended in a call, the kill throws `kill_not_colib_owned_t` naming the
  outsider.

`repro/18-killer_rebind.cpp` is the same leak after the target's chain ended: the spent killer,
still in the yielded generator's table, binds itself to the generator again when another
coroutine awaits it, and its next kill ends that coroutine's callee (FIX_18).

## The rule

Whether an inherited modif outlives the call is the modif's choice, like whether it is inherited
at all. A modif flagged `CO_MODIF_UNINHERIT_ON_YIELD` lasts one call: it leaves a coroutine's table
when that coroutine `co_yield`s back to its caller, and a generator awaited again inherits it again
from its new caller. Modifs without the flag stay, as today.

The killer's modifs carry the flag, so a generator belongs to the killer's chain only while that
chain awaits it:

```
target awaits g      -> g inherits the killer
g co_yields          -> RETURN: the killer moves top back to the target; then the killer leaves g
outsider awaits g    -> g inherits the outsider's modifs only: the killer never sees g again
target awaits g      -> g inherits the killer again, and the kill ends g with the target
```

The flag is public, and the killer only opts in to it, as DESIGN.md asks of the killer.

## Where the fix lands

| `colib.h` region           | what changes                                              | +/-     |
|----------------------------|-----------------------------------------------------------|---------|
| `modif_flags_e`            | `CO_MODIF_UNINHERIT_ON_YIELD`, with what it means         | +3 -0   |
| `cpp_yield_awaiter()`      | drops the flagged modifs once the RETURN callbacks ran    | +2 -0   |
| `drop_modifs_with()`       | FIX_47's helper, reused: its declaration moves up         | +2 -1   |
| `create_killer()`          | the killer's pack carries the flag                        | +2 -1   |
| total                      |                                                           | +9 -2   |

## The change

The diff is against `colib.h` at commit 9a13a9a with FIX_44 and FIX_47 applied. The reasoning,
per hunk:

1. **`CO_MODIF_UNINHERIT_ON_YIELD`.** The new flag, next to the inheritance flags it completes,
   with what a user observes. It takes `0x4`, which FIX_47 left free for it.
2. **`drop_modifs_with()`.** FIX_47's helper takes the modifs that carry a flag off a coroutine's
   own table, and drops the table when it ends up empty, as `add_modifs()` and `rm_modifs()` do.
   That is all this fix needs, so it calls the helper. The proposal had its own function,
   `uninherit_on_yield()`, with the same body; FIX_47 landed first, so the fix reuses its helper
   instead of adding a copy. `cpp_yield_awaiter()` is defined above the helper's declaration, so
   the declaration moves up, to just before it. Every coroutine has its own table
   (`inherit_modifs()` and `add_modifs()` allocate one per state), so nothing else loses a modif.
3. **`cpp_yield_awaiter()`.** It drops the flagged modifs after the RETURN callbacks, so the
   killer's `on_return` still moves its `top` back to the caller first. No callback list of that
   table is running at that point.
4. **`create_killer()`.** The killer's pack opts in.

```diff
--- colib.h (commit 9a13a9a + FIX_44 + FIX_47)
+++ colib.h (patched)
@@ -800,6 +800,9 @@
     CO_MODIF_INHERIT_NONE = 0x0,    /*!< Not inherited */
     CO_MODIF_INHERIT_ON_CALL = 0x1, /*!< Inherited on call */
     CO_MODIF_INHERIT_ON_SCHED = 0x2,/*!< Inherited on sched */
+    CO_MODIF_UNINHERIT_ON_YIELD = 0x4, /*!< Removed from a corutine when it co_yields back to its
+                                       caller: it lasts one call, and a generator awaited again
+                                       inherits it again from its new caller */
     CO_MODIF_UNINHERIT_ON_RESUME = 0x8, /*!< Removed from a corutine when the external hold it was
                                         attached for ends (see external_on_suspend()) */
 };
@@ -4412,6 +4415,8 @@
 
 #endif /* COLIB_ALLOCATOR_REPLACE */
 
+inline void drop_modifs_with(state_t *state, modif_flags_e flag);
+
 inline handle<void> cpp_yield_awaiter(state_t *yielding_task_state) {
     COLIB_DEBUG_TRACE_SCOPE("state: %p", yielding_task_state);
     /* bassicaly the same as bellow, except we don't destroy the corutine */
@@ -4422,6 +4427,8 @@
 
     /* it returns to its caller, alive: no exit, it is called again */
     error_e exit_err = do_return_modifs(yielding_task_state);
+    /* the call is over: what lasts one call goes */
+    drop_modifs_with(yielding_task_state, CO_MODIF_UNINHERIT_ON_YIELD);
 
     if (caller_state) {
         if (exit_err == ERROR_SUSPENDED)
@@ -4517,7 +4524,6 @@
 }
 
 inline void add_modifs_to_state(pool_t *pool, state_t *state, const modif_pack_t &mods);
-inline void drop_modifs_with(state_t *state, modif_flags_e flag);
 
 /* External Part
 ------------------------------------------------------------------------------------------------- */
@@ -6625,7 +6631,8 @@
 
     COLIB_DEBUG_TRACE("created killer: %p", k.get());
 
-    modif_flags_e flags = CO_MODIF_INHERIT_ON_CALL;
+    /* inherited by the calls of its chain, and left behind by a generator that yields out of it */
+    modif_flags_e flags = modif_flags_e(CO_MODIF_INHERIT_ON_CALL | CO_MODIF_UNINHERIT_ON_YIELD);
     modif_pack_t pack;
     pack.push_back(create_modif<CO_MODIF_CALL_CBK>(flags,
             [k](state_t *s) { return k->on_call(s); }));
```

## The regression test

`tests/018-037-reproduced_killer_generator_escape.cpp` (Test91), promoted from the draft the
reproduction left in `repro/fix/45/`, with FIX_18's case added. The cases:

1. the generator, lent to an outsider, parks inside the outsider's await: the kill ends the target
   and nothing of the outsider;
2. the generator `co_yield`s to the outsider, which parks: the same;
3. the generator `co_return`s to the outsider, which parks: the same;
4. a killer on each chain, the generator going from one to the other and back: each kill ends its
   own chain;
5. the control: the target awaits the generator again, and the kill ends the generator with it;
6. the target's chain is over before the outsider awaits the generator: the spent killer stays
   spent (`ERROR_FINISHED`), and the outsider and the generator end normally.

```cpp
#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test91 - Reproduced Bugs: a generator awaited outside the killer's chain took the kill with it
================================================================================================= */

/* Was an open bug (repro/45, and repro/18 for FIX_18): a generator that the target calls inherits
the killer's modifs (CO_MODIF_INHERIT_ON_CALL), and inherit_modifs() only ever adds to a table, so
the generator kept them after it yielded back. When a coroutine outside the target's chain awaited
the yielded generator, the killer's CALL and RETURN callbacks followed it there: the kill ended the
generator and the outsider (or the outsider alone) and returned ERROR_OK while the target lived on,
and once the outsider freed itself the next kill read freed memory (ASan heap-use-after-free). With
a killer on each chain, the outsider's killer ended the target's chain. With the target's chain
already over, its spent killer bound itself to the generator again (FIX_18).

Now a modif flagged CO_MODIF_UNINHERIT_ON_YIELD lasts one call: it leaves a coroutine's table when
that coroutine co_yields back to its caller, and a generator awaited again inherits it again from
its new caller. The killer's modifs carry the flag, so a generator belongs to a chain only while
that chain awaits it.

The runs: the generator parks inside the outsider's await; it co_yields to the outsider, which
parks; it co_returns to the outsider, which parks; a killer on each chain, with the generator going
from one chain to the other and back; the target awaiting the generator again after the outsider,
where the kill ends the generator with the target; and the target's chain over before the outsider
awaits the generator, where the spent killer stays spent. */

static const char *test91_file = "018-037-reproduced_killer_generator_escape.cpp";

enum test91_mode_e { TEST91_WAIT, TEST91_YIELD, TEST91_RETURN };

static int test91_t_destroyed = 0;
static int test91_g_destroyed = 0;
static int test91_o_destroyed = 0;
static bool test91_t_after = false;
static bool test91_g_after = false;
static bool test91_o_after = false;
static int test91_o_got = -1;
static int test91_o_again = -1;
static co::error_e test91_kill_ret = co::ERROR_GENERIC;
static co::task<int> test91_gen;    /* the generator the target and the outsider share */

struct test91_marker_t {
    int *cnt;
    ~test91_marker_t() { (*cnt)++; }
};

static void test91_reset() {
    test91_t_destroyed = test91_g_destroyed = test91_o_destroyed = 0;
    test91_t_after = test91_g_after = test91_o_after = false;
    test91_o_got = test91_o_again = -1;
    test91_kill_ret = co::ERROR_GENERIC;
}

/* runs after the others parked, and calls the kill */
static co::task_t test91_controller(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();
    test91_kill_ret = kill_fn();
    co_return 0;
}

/* ----------------------------------------------- one killer, the generator lent to an outsider */

static co::task<int> test91_generator(co::sem_p block, test91_mode_e mode) {
    test91_marker_t marker{&test91_g_destroyed};
    co_yield 1;                 /* back to the target: out of its chain from here */
    if (mode == TEST91_WAIT)
        co_await block->wait(); /* parks inside the outsider's await */
    if (mode == TEST91_YIELD)
        co_yield 2;             /* to the outsider */
    co_return 3;
}

static co::task_t test91_target(co::sem_p never) {
    test91_marker_t marker{&test91_t_destroyed};
    co_await test91_gen;        /* the generator yields 1 back */
    co_await never->wait();     /* the kill ends it here */
    test91_t_after = true;      /* must never run */
    co_return 0;
}

static co::task_t test91_outsider(co::sem_p park, test91_mode_e mode) {
    test91_marker_t marker{&test91_o_destroyed};
    test91_o_got = co_await test91_gen;         /* the generator runs here, out of the chain */
    if (mode != TEST91_WAIT) {
        co_await park->wait();                  /* the kill must not end it */
        test91_o_after = true;
    }
    if (mode == TEST91_YIELD)
        test91_o_again = co_await test91_gen;   /* it finishes the generator */
    co_return 0;
}

static int test91_single(test91_mode_e mode) {
    test91_reset();

    auto pool = co::create_pool();
    auto never = co::create_sem(pool, 0);
    auto block = co::create_sem(pool, 0);
    auto park = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    test91_gen = test91_generator(block, mode);
    auto target = co::add_modifs(pool.get(), test91_target(never), killer_mods);
    pool->sched(COLIB_REGNAME(target));
    pool->sched(COLIB_REGNAME(test91_outsider(park, mode)));
    pool->sched(test91_controller(kill_fn));
    ASSERT_FN(pool->run());

    /* the kill ended the target, and nothing of the outsider's await */
    ASSERT_FN(CHK_BOOL(test91_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after));
    ASSERT_FN(CHK_BOOL(test91_o_destroyed == 0));
    ASSERT_FN(CHK_BOOL(test91_g_destroyed == (mode == TEST91_RETURN ? 1 : 0)));

    /* the outsider goes on with the generator, and the kill finds nothing left */
    block->signal();
    park->signal();
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test91_o_destroyed == 1 && test91_g_destroyed == 1));
    ASSERT_FN(CHK_BOOL(test91_o_got == (mode == TEST91_YIELD ? 2 : 3)));
    ASSERT_FN(CHK_BOOL(mode != TEST91_YIELD || test91_o_again == 3));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ------------------------------------------ a killer on each chain, one generator between them */

static co::task<int> test91_shared() {
    test91_marker_t marker{&test91_g_destroyed};
    co_yield 1;                 /* to the target */
    co_yield 2;                 /* to the outsider */
    co_yield 3;                 /* to the target again */
    co_return 4;                /* to the finisher */
}

static co::task_t test91_two_target(co::sem_p gate, co::sem_p never) {
    test91_marker_t marker{&test91_t_destroyed};
    co_await test91_gen;        /* 1 */
    co_await gate->wait();      /* the outsider's turn */
    co_await test91_gen;        /* 3: the outsider's killer must not follow it here */
    co_await never->wait();     /* its own killer ends it here */
    test91_t_after = true;      /* must never run */
    co_return 0;
}

static co::task_t test91_two_outsider(co::sem_p gate, co::sem_p never) {
    test91_marker_t marker{&test91_o_destroyed};
    test91_o_got = co_await test91_gen;     /* 2 */
    gate->signal();
    co_await never->wait();     /* its own killer ends it here */
    test91_o_after = true;      /* must never run */
    co_return 0;
}

static co::task_t test91_finisher() {
    test91_o_again = co_await test91_gen;   /* 4: the generator ends */
    co_return 0;
}

static int test91_two() {
    test91_reset();

    auto pool = co::create_pool();
    auto gate = co::create_sem(pool, 0);
    auto t_never = co::create_sem(pool, 0);
    auto o_never = co::create_sem(pool, 0);
    auto [t_mods, kill_t] = co::create_killer(pool.get(), co::ERROR_USER);
    auto [o_mods, kill_o] = co::create_killer(pool.get(), co::ERROR_USER);

    test91_gen = test91_shared();
    auto target = co::add_modifs(pool.get(), test91_two_target(gate, t_never), t_mods);
    auto outsider = co::add_modifs(pool.get(), test91_two_outsider(gate, o_never), o_mods);
    pool->sched(COLIB_REGNAME(target));
    pool->sched(COLIB_REGNAME(outsider));
    pool->sched(test91_controller(kill_o));     /* the outsider's killer */
    ASSERT_FN(pool->run());

    /* the outsider's killer ended the outsider, and nothing of the target */
    ASSERT_FN(CHK_BOOL(test91_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test91_o_destroyed == 1 && !test91_o_after));
    ASSERT_FN(CHK_BOOL(test91_t_destroyed == 0));

    /* the target's killer ends the target */
    ASSERT_FN(CHK_BOOL(kill_t() == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after));

    pool->sched(test91_finisher());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test91_o_got == 2 && test91_o_again == 4 && test91_g_destroyed == 1));
    ASSERT_FN(CHK_BOOL(kill_o() == co::ERROR_FINISHED && kill_t() == co::ERROR_FINISHED));
    return 0;
}

/* ------------------------------------------ the target awaits the generator again, and owns it */

static co::task<int> test91_rejoin(co::sem_p block) {
    test91_marker_t marker{&test91_g_destroyed};
    co_yield 1;                 /* to the target */
    co_yield 2;                 /* to the outsider */
    co_await block->wait();     /* inside the target's second await: it dies with the target */
    test91_g_after = true;      /* must never run */
    co_return 3;
}

static co::task_t test91_rejoin_target(co::sem_p gate) {
    test91_marker_t marker{&test91_t_destroyed};
    co_await test91_gen;        /* 1 */
    co_await gate->wait();      /* the outsider's turn */
    co_await test91_gen;        /* the generator parks inside this await */
    test91_t_after = true;      /* must never run */
    co_return 0;
}

static co::task_t test91_rejoin_outsider(co::sem_p gate, co::sem_p park) {
    test91_marker_t marker{&test91_o_destroyed};
    test91_o_got = co_await test91_gen;     /* 2 */
    gate->signal();
    co_await park->wait();      /* the kill must not end it */
    test91_o_after = true;
    co_return 0;
}

static int test91_rejoin_run() {
    test91_reset();

    auto pool = co::create_pool();
    auto gate = co::create_sem(pool, 0);
    auto block = co::create_sem(pool, 0);
    auto park = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    test91_gen = test91_rejoin(block);
    auto target = co::add_modifs(pool.get(), test91_rejoin_target(gate), killer_mods);
    pool->sched(COLIB_REGNAME(target));
    pool->sched(COLIB_REGNAME(test91_rejoin_outsider(gate, park)));
    pool->sched(test91_controller(kill_fn));
    ASSERT_FN(pool->run());

    /* the generator was the target's again: the kill ended it with the target */
    ASSERT_FN(CHK_BOOL(test91_kill_ret == co::ERROR_OK));
    ASSERT_FN(CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after));
    ASSERT_FN(CHK_BOOL(test91_g_destroyed == 1 && !test91_g_after));
    ASSERT_FN(CHK_BOOL(test91_o_destroyed == 0));

    park->signal();
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test91_o_got == 2 && test91_o_after && test91_o_destroyed == 1));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ----------------------------------- the target's chain is over: its killer stays spent */

static co::task<int> test91_spent_gen(co::sem_p block) {
    test91_marker_t marker{&test91_g_destroyed};
    co_yield 1;                 /* to the target, which then ends */
    co_await block->wait();     /* inside the outsider's await */
    co_return 2;
}

static co::task_t test91_spent_target() {
    test91_marker_t marker{&test91_t_destroyed};
    co_await test91_gen;        /* 1 */
    co_return 0;                /* the killer's chain is over */
}

static co::task_t test91_spent_outsider() {
    test91_marker_t marker{&test91_o_destroyed};
    test91_o_got = co_await test91_gen;     /* 2, once the generator's wait ends */
    co_return 0;
}

static int test91_spent() {
    test91_reset();

    auto pool = co::create_pool();
    auto block = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    test91_gen = test91_spent_gen(block);
    auto target = co::add_modifs(pool.get(), test91_spent_target(), killer_mods);
    pool->sched(COLIB_REGNAME(target));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test91_t_destroyed == 1));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));     /* nothing left to kill */

    pool->sched(COLIB_REGNAME(test91_spent_outsider()));
    ASSERT_FN(pool->run());                                     /* the generator waits on block */
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));     /* still nothing: it stays spent */
    ASSERT_FN(CHK_BOOL(test91_o_destroyed == 0 && test91_g_destroyed == 0));

    block->signal();
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test91_o_got == 2 && test91_o_destroyed == 1 && test91_g_destroyed == 1));
    return 0;
}

int main() {
    int ret = test91_single(TEST91_WAIT);
    if (ret == 0)
        ret = test91_single(TEST91_YIELD);
    if (ret == 0)
        ret = test91_single(TEST91_RETURN);
    if (ret == 0)
        ret = test91_two();
    if (ret == 0)
        ret = test91_rejoin_run();
    if (ret == 0)
        ret = test91_spent();
    print_test_result(test91_file, ret >= 0);
    return ret;
}
```

## How it was checked

All runs on Windows 10, MSVC 19.43, built through the tests makefile.

### On the applied tree

The patched header is byte for byte the working tree's `colib.h`.

**1. The header before this fix fails.** That header is 9a13a9a with FIX_44 and FIX_47. Each case
of the test was built alone, with these flags:
`CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /FS /I.. /fsanitize=address'`.

| case                      | before this fix                                               |
|---------------------------|---------------------------------------------------------------|
| 1: parks in the await     | `Failed CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after)` |
| 2: co_yields to it        | `Failed CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after)` |
| 3: co_returns to it       | `Failed CHK_BOOL(test91_t_destroyed == 1 && !test91_t_after)` |
| 4: a killer on each chain | `Failed CHK_BOOL(test91_o_destroyed == 1 && !test91_o_after)` |
| 5: the control            | `[PASSED]`                                                    |
| 6: the spent killer       | `Failed CHK_BOOL(kill_fn() == co::ERROR_FINISHED)`            |

Cases 1 to 3: the target lives on. Case 4: the killers crossed. Case 6: the spent killer killed
again. These are the same failures as on the header the fix was first proposed on.

**2. The patched header passes.** `make 018-037-reproduced_killer_generator_escape.exe` in
`tests/` and a run: `[PASSED]`, exit 0. Under ASan it passes too, in the suite run below.

**3. The whole suite.** `make` in `tests/`, then `make` with the ASan flags above on scratch copies
of the patched header and of `tests/` (91 files, the new test included):

```
plain  [PASSED: 91]/[FAILED: 0]
asan   [PASSED: 91]/[FAILED: 0]
```

### As first proposed

On 9dfe29e with FIX_02, on scratch copies, before FIX_44 and FIX_47 landed. The test was then
numbered `018-036` (Test90), and the header had `uninherit_on_yield()` where it now calls
`drop_modifs_with()`.

`repro/45-killer_generator_escape.cpp` and `repro/18-killer_rebind.cpp` (type name updated),
ASan, every mode on both headers:

| mode       | unpatched                                     | patched                         |
|------------|-----------------------------------------------|---------------------------------|
| `wait`     | the kill ends the generator and the outsider  | `PASS`                          |
| `wait2`    | heap-use-after-free                           | `PASS`                          |
| `yield`    | the kill ends the outsider                    | `PASS`                          |
| `return`   | the kill ends the outsider                    | `PASS`                          |
| `ended`    | heap-use-after-free                           | `PASS`                          |
| `left`     | the kill throws for the outsider              | `PASS`                          |
| `inner`    | the kill ends the generator's callee too      | `PASS`                          |
| `two`      | the outsider's killer ends the target's chain | `PASS`                          |
| `chain`    | `PASS` (the control)                          | `PASS`                          |
| `sched`    | the kill ends the scheduled generator         | `PASS`                          |
| repro 18   | the spent killer re-binds and kills again     | `PASS`: it stays spent          |

The regression test on the patched header, plain and ASan:

```
plain  [PASSED]: 018-036-reproduced_killer_generator_escape.cpp     exit 0
asan   [PASSED]: 018-036-reproduced_killer_generator_escape.cpp     exit 0
```

On the unpatched header, each case built alone, ASan: cases 1 to 3 fail
`CHK_BOOL(test90_t_destroyed == 1 && !test90_t_after)` (the target lives), case 4 fails
`CHK_BOOL(test90_o_destroyed == 1 && !test90_o_after)` (the killers crossed), case 6 fails
`CHK_BOOL(kill_fn() == co::ERROR_FINISHED)` (the spent killer killed again), and case 5, the
control, passes.

The whole suite (88 files, without the new test), on scratch copies of the patched header and of
`tests/`; `make`, then `make CXX_FLAGS='/EHsc /await:strict /std:c++20 /Zi /FS /I..
/fsanitize=address'`:

```
plain  [PASSED: 88]/[FAILED: 0]
asan   [PASSED: 88]/[FAILED: 0]
```

**NOT-CHECKED.** The epoll and kqueue backends (they don't compile on this machine; no
backend-specific line changed), and GCC and Clang (not installed here; `std::erase_if` on a vector
is C++20).

## Interactions

- **FIX_18, a spent killer re-binds.** Superseded: its repro passes on this fix, because the
  yielded generator no longer carries the killer, and case 6 of the test is that repro. FIX_18's
  own change (guards in the killer) is no longer needed.
- **FIX_44 and FIX_47, applied before it.** FIX_47 added `drop_modifs_with()`, the function this
  fix had as `uninherit_on_yield()`, and the flag `CO_MODIF_UNINHERIT_ON_RESUME` on `0x8`, right
  where this fix adds its own: the fix was rebased on both, as "The change" tells. That flag was
  renamed `CO_MODIF_UNINHERIT_ON_EXTERN_RESUME` after both fixes were applied. FIX_44's
  `completed = false;` was a guard against FIX_18's re-binding; it stays harmless.
- **FIX_30, modif type bounds.** It changes a line inside `inherit_modifs()`; with no new function
  next to it any more, there is no overlap.
- **FIX_33, the pack order.** No overlap.
- **README and docs.** README's Modifs section doesn't name the flags, so it doesn't change;
  `docs/TODO.md` lists the new flag for the API chapter.

## Open questions

1. **The name.** `CO_MODIF_UNINHERIT_ON_YIELD` mirrors the inheritance flags;
   `CO_MODIF_DROP_ON_YIELD` is the shorter alternative. FIX_47's flag, its neighbour, was renamed
   `CO_MODIF_UNINHERIT_ON_EXTERN_RESUME` on the user's word after both fixes were applied, so the
   two names now match in style.
2. **Modifs attached directly.** The flag removes a modif on a `co_yield` whether it was inherited
   or attached to the coroutine itself: it lasts one call either way. For the killer the
   difference doesn't arise (a killer attached to a generator ends the program at its first
   `co_yield`).
3. **FIX_18.** Superseded by this fix; its proposal was deleted on the user's word (2026-10-12).
