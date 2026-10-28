#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <memory>

/* Test89 - Reproduced Bugs: a kill that ran its target freed it under whoever took it on the way
================================================================================================= */

/* Was an open bug (repro/44): a kill of a target whose wait already did its work runs the target
up to its next wait inside killer_state_t::drive(), where the killer's own on_wait parks it, and
then ended the chain without looking. Something else could take the target on the way and keep a
pointer to it: a user modif parking it at the same wait, a user CALL modif taking a call it makes,
an external awaitable. The kill destroyed it anyway, and the holder then used freed memory. The
same went for the root's caller when the driven root returned and a user RETURN modif took the
caller: the kill queued the caller too, so it ran twice.

The kill now asks whenever the drive stops somewhere it didn't stop it: the RECLAIM callbacks of
the coroutine it needs are called, and a modif that holds it lets it go (forgets it, returns
ERROR_OK) or keeps it (returns an error or ERROR_SUSPENDED). Kept, the kill throws
kill_not_colib_owned_t and the holder keeps it. Let go at a wait, the kill ends it there; let go
at a call or a return, it goes on as colib would have, until its next wait or its end. An external
awaitable is asked through the modifs it attached (with none, it lets go): let go, the kill ends
the target at the hold, a wait; a target it gave straight back to the ready queue goes on. For the
root's caller, the caller's modifs are asked (a RETURN modif that takes it is on it too, inherited
from it): one that keeps it resumes it, one that lets it go has the kill queue it, and one that
already gave it back is left alone.

The runs, each on a target whose semaphore wait already gave it its token: a modif parks it at its
next wait and lets it go, added before and after the killer's pack; the same modif keeps it, by an
error and by ERROR_SUSPENDED; a CALL modif takes its call and lets the callee go, or keeps it; a
callee returns and a RETURN modif takes the target, then lets it go or keeps it; an external
awaitable takes it, or takes it and gives it straight back; the driven root returns and a RETURN
modif keeps its caller, lets it go, or already gave it back. */

static const char *test89_file = "018-035-reproduced_killer_drive_reclaim.cpp";

static int test89_destructed = 0;
static bool test89_got_token = false;       /* the code after the completed wait ran */
static bool test89_after = false;           /* the code after the hold ran */
static co::state_t *test89_held = nullptr;  /* what the holder keeps */
static bool test89_threw = false;           /* the kill threw, carrying the held frame */
static co::error_e test89_ret = co::ERROR_GENERIC;

struct test89_marker_t {
    ~test89_marker_t() { test89_destructed++; }
};

static void test89_reset() {
    test89_destructed = 0;
    test89_got_token = false;
    test89_after = false;
    test89_held = nullptr;
    test89_threw = false;
    test89_ret = co::ERROR_GENERIC;
}

/* the target waits on sem, it gets the token, and the kill runs it: the controller of every case */
static co::task_t test89_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the target waits on sem */
    sem->signal();              /* it takes the token and is queued, not resumed yet */
    try {
        test89_ret = kill_fn(); /* its wait did its work: the kill runs it */
    }
    catch (co::kill_not_colib_owned_t &e) {
        test89_threw = (e.target == test89_held);
    }
    co_return 0;
}

/* ------------------------------------------------- a user modif parks it at its next wait too */

static co::task_t test89_park_victim(co::sem_p sem, co::sem_p never) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    co_await never->wait();     /* the user modif and the killer's on_wait both park it here */
    test89_after = true;        /* must never run */
    co_return 0;
}

/* parks the target at `never`; asked for it, it answers `answer` (ERROR_OK lets it go) */
static co::modif_pack_t test89_park_mods(co::sem_t *never, co::error_e answer) {
    auto f = co::CO_MODIF_INHERIT_NONE;
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(f,
            [never](co::state_t *s, co::sem_t *sem) -> co::error_e {
                if (sem != never)
                    return co::ERROR_OK;
                test89_held = s;
                return co::ERROR_SUSPENDED;     /* parked: this modif holds it */
            }));
    pack.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [answer](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (answer != co::ERROR_OK)
                    return answer;              /* it keeps it */
                test89_held = nullptr;          /* it lets it go: it forgets it */
                return co::ERROR_OK;
            }));
    return pack;
}

static int test89_copark(bool park_first, co::error_e answer) {
    test89_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
    auto park_mods = test89_park_mods(never.get(), answer);

    auto victim = test89_park_victim(sem, never);
    co::add_modifs(pool.get(), victim, park_first ? park_mods : killer_mods);
    co::add_modifs(pool.get(), victim, park_first ? killer_mods : park_mods);

    pool->sched(COLIB_REGNAME(victim));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test89_got_token));
    ASSERT_FN(CHK_BOOL(!test89_after));
    if (answer == co::ERROR_OK) {
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* the kill ended it */
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* its holder let it go */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept it */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        co::destroy_state(test89_held);                     /* its holder ends it, once */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ------------------------------------------------- a user CALL modif takes the call it makes */

static std::weak_ptr<int> test89_callee_frame;
static int test89_callee_got = -1;

static co::task_t test89_callee(std::shared_ptr<int> keep) {
    (void)keep;                 /* the frame's copy: it expires when the frame is destroyed */
    co_return 5;
}

static co::task_t test89_call_victim(co::pool_t *pool, co::sem_p sem, co::modif_pack_t take) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    auto keep = std::make_shared<int>(0);
    test89_callee_frame = keep;
    auto callee = co::add_modifs(pool, test89_callee(keep), take);
    keep.reset();               /* the callee's frame holds the last copy */
    test89_callee_got = co_await callee;    /* the user CALL modif takes this call */
    test89_after = true;        /* runs only once the holder starts the callee */
    co_return 0;
}

static int test89_call(bool lets_go) {
    test89_reset();
    test89_callee_got = -1;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto f = co::CO_MODIF_INHERIT_NONE;
    co::modif_pack_t take;
    take.push_back(co::create_modif<co::CO_MODIF_CALL_CBK>(f, [](co::state_t *s) -> co::error_e {
        test89_held = s;        /* the callee: this modif would start it later */
        return co::ERROR_SUSPENDED;
    }));
    take.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [lets_go](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (!lets_go)
                    return co::ERROR_USER;      /* it keeps the callee */
                test89_held = nullptr;          /* it lets the callee go */
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test89_call_victim(pool.get(), sem, take), killer_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (lets_go) {
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the holder let the callee go */
        ASSERT_FN(CHK_BOOL(test89_callee_got == 5));        /* the callee ran, inside the kill */
        ASSERT_FN(CHK_BOOL(test89_callee_frame.expired())); /* its caller's await freed it */
        ASSERT_FN(CHK_BOOL(test89_after));                  /* no wait after it: it went on */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* and ended by itself */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept the callee */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        co::external_sched_resume(test89_held);             /* the holder starts the callee */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_callee_got == 5 && test89_after));
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));        /* the chain ended by itself */
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* -------------------------------------- a callee returns, and its RETURN modif takes the target */

static int test89_c_got = -1;
static std::weak_ptr<int> test89_c_frame;

static co::task_t test89_c(co::sem_p sem, std::shared_ptr<int> in_frame) {
    (void)in_frame;             /* this copy dies with C's frame */
    co_await sem->wait();       /* signaled, then killed: the kill runs C from here */
    test89_got_token = true;
    co_return 5;                /* its RETURN modif takes the target, C's caller */
}

static co::task_t test89_r(co::sem_p sem, co::sem_p never) {
    test89_marker_t marker;
    auto in_frame = std::make_shared<int>(0);
    test89_c_frame = in_frame;
    test89_c_got = co_await test89_c(sem, std::move(in_frame));
    co_await never->wait();     /* its next wait */
    test89_after = true;        /* runs only once never is signaled */
    co_return 0;
}

static int test89_callee_return(bool lets_go) {
    test89_reset();
    test89_c_got = -1;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto never = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* on the target, inherited by its callee: the callee's RETURN takes the target */
    auto f = co::CO_MODIF_INHERIT_ON_CALL;
    co::modif_pack_t user_mods;
    user_mods.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f,
            [](co::state_t *s) -> co::error_e {
                if (!s->caller_state)
                    return co::ERROR_OK;        /* the target's own return: no caller to take */
                test89_held = s->caller_state;  /* it takes the target: it resumes it later */
                return co::ERROR_SUSPENDED;
            }));
    user_mods.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [lets_go](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (!lets_go)
                    return co::ERROR_USER;      /* it keeps the target */
                test89_held = nullptr;          /* it lets the target go */
                return co::ERROR_OK;
            }));

    auto r = co::add_modifs(pool.get(), test89_r(sem, never), killer_mods);
    co::add_modifs(pool.get(), r, user_mods);
    pool->sched(r);
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (lets_go) {
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the holder let the target go */
        ASSERT_FN(CHK_BOOL(test89_c_got == 5));             /* the target went on: it read the 5 */
        ASSERT_FN(CHK_BOOL(test89_c_frame.expired()));      /* and its await freed C's frame */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* ended at its next wait */
        ASSERT_FN(CHK_BOOL(!test89_after));
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_threw));                  /* the holder kept the target */
        ASSERT_FN(CHK_BOOL(test89_destructed == 0));
        ASSERT_FN(CHK_BOOL(!test89_c_frame.expired()));     /* C's 5 waits for the target */
        co::external_sched_resume(test89_held);             /* the holder resumes the target */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_c_got == 5 && test89_c_frame.expired()));
        never->signal();
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test89_after && test89_destructed == 1));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ---------------------------------------------------- an external awaitable takes the target */

struct test89_hold_t {
    bool hands_back;            /* it gives the target straight back to the ready queue */

    ~test89_hold_t() { test89_held = nullptr; }    /* destroyed while holding: it lets go */

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        test89_held = co::external_on_suspend(h);   /* it keeps it, to resume it later */
        if (hands_back) {
            co::external_sched_resume(test89_held); /* or it gives it back at once */
            test89_held = nullptr;
        }
        return std::noop_coroutine();
    }

    void await_resume() {}
};

static co::task_t test89_ext_victim(co::sem_p sem, bool hands_back) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test89_got_token = true;
    co_await test89_hold_t{hands_back};
    test89_after = true;        /* runs once it is resumed */
    co_return 0;
}

static int test89_ext(bool hands_back) {
    test89_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test89_ext_victim(sem, hands_back), killer_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test89_got_token));

    if (hands_back) {
        ASSERT_FN(CHK_BOOL(test89_after));                  /* given back: it went on */
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* and ended by itself */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    }
    else {
        ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_OK));    /* asked, silent: it let go */
        ASSERT_FN(CHK_BOOL(!test89_after));                 /* the kill ended it at the hold */
        ASSERT_FN(CHK_BOOL(test89_destructed == 1));
        ASSERT_FN(CHK_BOOL(test89_held == nullptr));        /* the awaitable went with it */
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

/* ----------------------------------- the driven root returns, a RETURN modif takes its caller */

enum test89_return_e { TEST89_KEEPS, TEST89_LETS_GO, TEST89_GAVE_BACK };

static int test89_parent_runs = 0;
static int test89_parent_got = 0;
static test89_return_e test89_return_mode = TEST89_KEEPS;

static co::task_t test89_ret_root(co::sem_p sem) {
    test89_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it, and it returns */
    test89_got_token = true;
    co_return 7;
}

static co::task_t test89_parent(co::pool_t *pool, co::sem_p sem, co::modif_pack_t killer_mods) {
    test89_parent_got = co_await co::add_modifs(pool, test89_ret_root(sem), killer_mods);
    test89_parent_runs++;       /* once, whoever resumes it */
    co_return 0;
}

static int test89_return(test89_return_e mode) {
    test89_reset();
    test89_parent_runs = 0;
    test89_parent_got = 0;
    test89_return_mode = mode;
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    /* the user's modifs go on the parent, and the root inherits them: the RETURN callback takes
    the root's caller (the parent), and the parent's RECLAIM callback is the one a kill asks */
    auto f = co::CO_MODIF_INHERIT_ON_CALL;
    co::modif_pack_t user_mods;
    user_mods.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(f,
            [](co::state_t *s) -> co::error_e {
                if (!s->caller_state)
                    return co::ERROR_OK;        /* the parent's own return: no caller to take */
                test89_held = s->caller_state;  /* it takes the caller: it resumes it later */
                if (test89_return_mode == TEST89_GAVE_BACK) {
                    co::external_sched_resume(test89_held);     /* or at once */
                    test89_held = nullptr;
                }
                return co::ERROR_SUSPENDED;
            }));
    user_mods.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [](co::state_t *) -> co::error_e {
                if (!test89_held)
                    return co::ERROR_OK;        /* it holds nothing */
                if (test89_return_mode == TEST89_KEEPS)
                    return co::ERROR_USER;      /* it keeps the caller */
                test89_held = nullptr;          /* it lets the caller go */
                return co::ERROR_OK;
            }));

    pool->sched(co::add_modifs(pool.get(), test89_parent(pool.get(), sem, killer_mods), user_mods));
    pool->sched(test89_controller(sem, kill_fn));
    ASSERT_FN(pool->run());

    ASSERT_FN(CHK_BOOL(test89_ret == co::ERROR_FINISHED));  /* the root ended by itself */
    ASSERT_FN(CHK_BOOL(test89_got_token));
    ASSERT_FN(CHK_BOOL(test89_destructed == 1));
    if (mode == TEST89_KEEPS) {
        ASSERT_FN(CHK_BOOL(test89_parent_runs == 0));       /* the kill didn't queue the caller */
        co::external_sched_resume(test89_held);             /* the RETURN modif resumes it */
        ASSERT_FN(pool->run());
    }
    ASSERT_FN(CHK_BOOL(test89_parent_runs == 1));           /* exactly once */
    ASSERT_FN(CHK_BOOL(test89_parent_got == 7));
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test89_copark(true, co::ERROR_OK);            /* the user's modif first */
    if (ret == 0)
        ret = test89_copark(false, co::ERROR_OK);           /* after it */
    if (ret == 0)
        ret = test89_copark(true, co::ERROR_USER);          /* the holder keeps it */
    if (ret == 0)
        ret = test89_copark(true, co::ERROR_SUSPENDED);     /* it keeps it, the other way */
    if (ret == 0)
        ret = test89_call(true);
    if (ret == 0)
        ret = test89_call(false);
    if (ret == 0)
        ret = test89_callee_return(true);
    if (ret == 0)
        ret = test89_callee_return(false);
    if (ret == 0)
        ret = test89_ext(false);
    if (ret == 0)
        ret = test89_ext(true);
    if (ret == 0)
        ret = test89_return(TEST89_KEEPS);
    if (ret == 0)
        ret = test89_return(TEST89_LETS_GO);
    if (ret == 0)
        ret = test89_return(TEST89_GAVE_BACK);
    print_test_result(test89_file, ret >= 0);
    return ret;
}
