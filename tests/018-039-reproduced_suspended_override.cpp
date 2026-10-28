#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test94 - Reproduced Bugs: a later WAIT_SEM error revoked an earlier ERROR_SUSPENDED park
================================================================================================= */

/* Was an open bug (repro/08): do_generic_modifs returned on the first negative callback return,
so a WAIT_SEM callback that had already returned ERROR_SUSPENDED (it parked the coroutine and owns
it) was overridden by a later WAIT_SEM callback returning an error. sem_awaiter_t::await_suspend
then took its refusal path and resumed the coroutine at once, although a modif had parked it and
still owned it: the owner's own resume then reached a freed frame (repro/08-suspended_override.md,
a heap-use-after-free in external_sched_resume). Now an ERROR_SUSPENDED is final: a later error
can't take the coroutine back from the modif that parked it, the callbacks after an
ERROR_SUSPENDED still run and the ones after an error don't. An error that comes before any
ERROR_SUSPENDED still refuses the wait, as before.

The owner ends the park with destroy_state(), as 011-006's owner does, so the debug checks stay on
and check the leave/enter pairing of both flows too.

The parker and the refuser are attached with two separate add_modifs() calls on purpose: one call
would order them inside a std::set<modif_p> by shared_ptr address, not pack order, and the vector
order decides which return the aggregate sees first. */

static std::string test94_order;
static co::state_t *test94_parked = nullptr;
static int test94_after_wait = 0;
static int test94_destructed = 0;

struct test94_marker_t {
    ~test94_marker_t() { test94_destructed++; }
};

static co::task_t test94_victim(co::sem_p sem) {
    test94_marker_t marker;
    co_await sem->wait();
    test94_after_wait++;        /* runs once per resume that gets past the wait */
    co_return 0;
}

static co::modif_p test94_parker() {
    return co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t *s, co::sem_t *) -> co::error_e {
            test94_order += "A";
            test94_parked = s;  /* the modif owns s from its return on */
            return co::ERROR_SUSPENDED;
        });
}

static co::modif_p test94_refuser() {
    return co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t *, co::sem_t *) -> co::error_e {
            test94_order += "B";
            return co::ERROR_USER;
        });
}

static void test94_reset() {
    test94_order.clear();
    test94_parked = nullptr;
    test94_after_wait = 0;
    test94_destructed = 0;
}

/* [A, B]: A parks, B's error must not revoke the park. The owner then ends the coroutine, and
nothing ever runs it past the wait. */
static int test94_park_survives_later_error() {
    test94_reset();

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    co::modif_pack_t pack_a;
    pack_a.push_back(test94_parker());
    co::modif_pack_t pack_b;
    pack_b.push_back(test94_refuser());

    /* two add_modifs calls, so the WAIT_SEM vector order is [A, B] */
    auto parked = co::add_modifs(pool.get(), test94_victim(sem), pack_a);
    pool->sched(co::add_modifs(pool.get(), parked, pack_b));

    ASSERT_FN(pool->run());
    DBG("order: %s after_wait: %d", test94_order.c_str(), test94_after_wait);
    ASSERT_FN(CHK_BOOL(test94_order == "AB"));       /* B ran, after the park */
    ASSERT_FN(CHK_BOOL(test94_after_wait == 0));     /* the park held, no engine resume */
    ASSERT_FN(CHK_PTR(test94_parked));
    ASSERT_FN(CHK_BOOL(test94_parked->get_state() == co::STATE_LEFT));   /* in no queue */

    co::destroy_state(test94_parked);                /* the owner ends it, as 011-006's does */
    ASSERT_FN(CHK_BOOL(test94_destructed == 1));     /* its frame is gone, once */
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test94_after_wait == 0));     /* nothing ran it past the wait */
    return 0;
}

/* [B, A]: an error before any suspension still refuses the wait. A never runs, nobody parks, and
the coroutine continues past the refused wait right away - the half of the rule that stays. */
static int test94_error_before_park_refuses() {
    test94_reset();

    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);

    co::modif_pack_t pack_b;
    pack_b.push_back(test94_refuser());
    co::modif_pack_t pack_a;
    pack_a.push_back(test94_parker());

    auto refused = co::add_modifs(pool.get(), test94_victim(sem), pack_b);
    pool->sched(co::add_modifs(pool.get(), refused, pack_a));

    ASSERT_FN(pool->run());
    DBG("order: %s after_wait: %d", test94_order.c_str(), test94_after_wait);
    ASSERT_FN(CHK_BOOL(test94_order == "B"));        /* the error stopped A, nothing parked */
    ASSERT_FN(CHK_BOOL(test94_parked == nullptr));
    ASSERT_FN(CHK_BOOL(test94_after_wait == 1));     /* refused: it ran past the wait at once */
    ASSERT_FN(CHK_BOOL(test94_destructed == 1));     /* and on to its end */
    return 0;
}

int main() {
    int ret = test94_park_survives_later_error();
    ret |= test94_error_before_park_refuses();
    print_test_result("018-039-reproduced_suspended_override.cpp", ret >= 0);
    return ret;
}
