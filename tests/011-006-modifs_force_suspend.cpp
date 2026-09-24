#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test50 - Modifs: ERROR_SUSPENDED and CO_MODIF_YIELD_CBK
================================================================================================= */

/* A WAIT_SEM callback answering ERROR_SUSPENDED parks the coroutine: it has left (LEAVE), the
wait isn't registered (no token can reach it) and the wait callbacks are unwound (UNWAIT_SEM). The
modif owns it from there: the engine keeps nothing of it, it is off the stack once the resume that
parked it returned, and nothing else runs it until its owner destroys it. A YIELD callback
returning an error refuses the yield: the coroutine continues right away, and doesn't get a second
ENTER (the debug checks would abort). */

static std::string test50_order;
static co::state_t *test50_parked = nullptr;
static int test50_after_wait = 0;
static int test50_destructed = 0;
static int test50_after_yield = 0;
static bool test50_owner_ok = false;

struct test50_marker_t {
    ~test50_marker_t() { test50_destructed++; }
};

static co::task_t test50_parked_task(co::sem_p sem) {
    test50_marker_t marker;
    co_await sem->wait();
    test50_after_wait++;        /* must not run */
    co_return 0;
}

static co::task_t test50_owner(co::sem_p sem) {
    co_await co::yield();       /* the parked task runs and parks */
    sem->signal();              /* no waiter: the count goes to 1, nobody is woken */
    co_await co::yield();
    ASSERT_COFN(CHK_BOOL(test50_parked != nullptr));
    ASSERT_COFN(CHK_BOOL(test50_after_wait == 0));
    ASSERT_COFN(CHK_BOOL(test50_parked->get_state() == co::STATE_LEFT));   /* in no queue */
    co::destroy_state(test50_parked);   /* the owner ends it */
    ASSERT_COFN(CHK_BOOL(test50_destructed == 1));
    ASSERT_COFN(CHK_BOOL(sem->try_dec()));  /* the token stayed in the semaphore */
    test50_owner_ok = true;
    co_return 0;
}

static co::task_t test50_refused_yield() {
    co_await co::yield();       /* refused: continues right away */
    test50_after_yield++;
    co_return 0;
}

int test50_force_suspend() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 0);
    auto flags = co::CO_MODIF_INHERIT_NONE;

    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_WAIT_SEM_CBK>(flags,
        [](co::state_t *s, co::sem_t *) -> co::error_e {
            test50_order += "W";
            test50_parked = s;
            return co::ERROR_SUSPENDED;
        }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNWAIT_SEM_CBK>(flags,
        [](co::state_t *, co::sem_t *) -> co::error_e { test50_order += "U"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
        [](co::state_t *) -> co::error_e { test50_order += "L"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
        [](co::state_t *) -> co::error_e { test50_order += "E"; return co::ERROR_OK; }));

    auto refuse = co::modif_pack_t(1, co::create_modif<co::CO_MODIF_YIELD_CBK>(flags,
        [](co::state_t *) -> co::error_e { return co::ERROR_USER; }));

    pool->sched(co::add_modifs(pool.get(), test50_parked_task(sem), pack));
    pool->sched(test50_owner(sem));
    pool->sched(co::add_modifs(pool.get(), test50_refused_yield(), refuse));
    ASSERT_FN(pool->run());

    /* ENTER at its first run, then the park: LEAVE, WAIT_SEM, UNWAIT_SEM - no ENTER, it never
    resumed */
    DBG("order: %s", test50_order.c_str());
    ASSERT_FN(CHK_BOOL(test50_order == "ELWU"));
    ASSERT_FN(CHK_BOOL(test50_owner_ok));
    ASSERT_FN(CHK_BOOL(test50_after_yield == 1));
    return 0;
}

int main() {
    int ret = test50_force_suspend();
    print_test_result("011-006-modifs_force_suspend.cpp", ret >= 0);
    return ret;
}
