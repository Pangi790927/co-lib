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
