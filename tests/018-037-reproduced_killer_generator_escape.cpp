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
