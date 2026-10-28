#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <vector>

/* Test90 - Reproduced Bugs: a kill that ran its target into an external awaitable couldn't end it
================================================================================================= */

/* Was an open question of FIX_44: an external awaitable that took the target during a kill's run
(external_on_suspend()) couldn't be asked to let it go, so the kill threw kill_not_colib_owned_t
and the target lived on, although an external hold is a wait, where a kill ends its target.

Now the hold is asked like any holder, through the modifs the awaitable attached with
external_on_suspend() (flagged CO_MODIF_UNINHERIT_ON_EXTERN_RESUME, so they come off when the hold
ends); with none, it lets go. Let go, the kill ends the target at the hold, and the awaitable
object, which lives in the target's frame, is destroyed with it: its destructor cancels what it
started. Kept, the kill throws.

The runs, each on a target whose semaphore wait already gave it its token: an awaitable with no
modifs, which cleans up in its destructor; one whose RECLAIM callback keeps the target, which it
later gives back, after which its modifs are gone; one whose RECLAIM callback lets the target go,
and whose EXIT callback then hears of its death. */

static const char *test90_file = "018-036-reproduced_killer_external_reclaim.cpp";

static int test90_destructed = 0;
static bool test90_got_token = false;
static bool test90_after = false;
static co::state_t *test90_held = nullptr;      /* what the awaitable would resume later */
static bool test90_cancelled = false;           /* the awaitable's destructor ran while holding */
static bool test90_threw = false;
static co::error_e test90_ret = co::ERROR_GENERIC;
static int test90_reclaims = 0;
static int test90_exits = 0;
static size_t test90_hold_mods_after = 99;      /* hold modifs left once the hold ended */

struct test90_marker_t {
    ~test90_marker_t() { test90_destructed++; }
};

static void test90_reset() {
    test90_destructed = 0;
    test90_got_token = false;
    test90_after = false;
    test90_held = nullptr;
    test90_cancelled = false;
    test90_threw = false;
    test90_ret = co::ERROR_GENERIC;
    test90_reclaims = 0;
    test90_exits = 0;
    test90_hold_mods_after = 99;
}

/* an awaitable that holds the coroutine until someone resumes test90_held */
struct test90_hold_t {
    co::modif_pack_t hold_mods;

    ~test90_hold_t() {
        if (test90_held) {
            test90_cancelled = true;    /* it would cancel its pending operation here */
            test90_held = nullptr;
        }
    }

    bool await_ready() { return false; }

    template <typename P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) {
        test90_held = co::external_on_suspend(h, hold_mods);
        return std::noop_coroutine();
    }

    void await_resume() { test90_held = nullptr; }
};

static co::task_t test90_victim(co::sem_p sem, co::modif_pack_t hold_mods) {
    test90_marker_t marker;
    co_await sem->wait();       /* signaled, then killed: the kill runs it from here */
    test90_got_token = true;
    co_await test90_hold_t{hold_mods};
    test90_after = true;        /* runs only once the awaitable gives it back */
    size_t left = 0;
    for (auto &modif : co_await co::task_modifs())
        left += (modif->flags & co::CO_MODIF_UNINHERIT_ON_EXTERN_RESUME) != 0;
    test90_hold_mods_after = left;
    co_return 0;
}

static co::task_t test90_controller(co::sem_p sem, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the target waits on sem */
    sem->signal();              /* it takes the token and is queued, not resumed yet */
    try {
        test90_ret = kill_fn(); /* its wait did its work: the kill runs it into the awaitable */
    }
    catch (co::kill_not_colib_owned_t &e) {
        test90_threw = (e.target == test90_held);
    }
    co_return 0;
}

/* the modifs an awaitable attaches while it holds the target: RECLAIM answers `answer` */
static co::modif_pack_t test90_hold_mods(co::error_e answer) {
    auto f = co::CO_MODIF_UNINHERIT_ON_EXTERN_RESUME;
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_RECLAIM_CBK>(f,
            [answer](co::state_t *) -> co::error_e {
                test90_reclaims++;
                return answer;
            }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(f,
            [](co::state_t *) -> co::error_e {
                test90_exits++;         /* the target died while held */
                return co::ERROR_OK;
            }));
    return pack;
}

static int test90_run(co::modif_pack_t hold_mods, bool keeps) {
    test90_reset();
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    pool->sched(co::add_modifs(pool.get(), test90_victim(sem, hold_mods), killer_mods));
    pool->sched(test90_controller(sem, kill_fn));
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test90_got_token));

    if (keeps) {
        ASSERT_FN(CHK_BOOL(test90_threw));                  /* the awaitable kept it */
        ASSERT_FN(CHK_BOOL(test90_destructed == 0 && test90_reclaims == 1));
        co::external_sched_resume(test90_held);             /* it gives the target back */
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test90_after && test90_destructed == 1));
        ASSERT_FN(CHK_BOOL(test90_hold_mods_after == 0));   /* the hold's modifs came off */
        ASSERT_FN(CHK_BOOL(test90_exits == 0));             /* gone before the target ended */
    }
    else {
        ASSERT_FN(CHK_BOOL(test90_ret == co::ERROR_OK));    /* the kill ended it at the hold */
        ASSERT_FN(CHK_BOOL(!test90_after && test90_destructed == 1));
        ASSERT_FN(CHK_BOOL(test90_cancelled));              /* the awaitable cleaned up */
        ASSERT_FN(CHK_BOOL(test90_held == nullptr));
        ASSERT_FN(CHK_BOOL(test90_reclaims == (hold_mods.empty() ? 0 : 1)));
        ASSERT_FN(CHK_BOOL(test90_exits == (hold_mods.empty() ? 0 : 1)));
    }
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    return 0;
}

int main() {
    int ret = test90_run(co::modif_pack_t{}, false);            /* no modifs: it lets go */
    if (ret == 0)
        ret = test90_run(test90_hold_mods(co::ERROR_USER), true);   /* it keeps the target */
    if (ret == 0)
        ret = test90_run(test90_hold_mods(co::ERROR_OK), false);    /* it lets the target go */
    print_test_result(test90_file, ret >= 0);
    return ret;
}
