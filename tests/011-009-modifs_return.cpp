#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test60 - Modifs: CO_MODIF_RETURN_CBK, a co_yield returns without exiting
================================================================================================= */

/* A coroutine hands control back to its caller with RETURN: on a co_yield (err is ERROR_YIELDED, it
stays alive and the next await calls it again) and on its co_return (then EXIT, it dies). EXIT is
the last callback it ever gets, so a co_yield doesn't fire it. A killer on the caller follows the
chain through CALL and RETURN: after the generator yielded, the caller is the one waiting, and the
kill ends the caller, not the yielded generator. Awaited again by another coroutine, the generator
is that killer's chain until it returns. (It is finished there: an abandoned generator is its
holder's to free, and colib doesn't free it.) */

static std::string test60_order;
static int test60_sum = 0;

static co::task_t test60_generator() {
    co_yield 1;
    co_yield 2;
    co_return 3;
}

static co::task_t test60_caller(co::modif_pack_t pack) {
    auto gen = co::add_modifs(co_await co::get_pool(), test60_generator(), pack);
    for (int i = 0; i < 3; i++)
        test60_sum += co_await gen;
    co_return 0;
}

static bool test60_after_wait = false;
static co::error_e test60_kill_ret = co::ERROR_GENERIC;

static co::task_t test60_gen_once() {
    co_yield 1;
    co_return 2;
}

static co::task_t test60_gen;

static co::task_t test60_target(co::sem_p sem) {
    test60_gen = test60_gen_once();
    co_await test60_gen;        /* it yields back: the killer's chain is us again */
    co_await sem->wait();       /* killed here */
    test60_after_wait = true;
    co_return 0;
}

static co::task_t test60_killer(std::function<co::error_e(void)> kill_fn) {
    co_await co::yield();       /* the target waits on sem */
    test60_kill_ret = kill_fn();
    co_await test60_gen;        /* its caller is dead, we finish it */
    co_return 0;
}

int test60_return() {
    {
        auto pool = co::create_pool();
        auto flags = co::CO_MODIF_INHERIT_NONE;
        co::modif_pack_t pack;
        pack.push_back(co::create_modif<co::CO_MODIF_CALL_CBK>(flags,
            [](co::state_t *) -> co::error_e { test60_order += "C"; return co::ERROR_OK; }));
        pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
            [](co::state_t *) -> co::error_e { test60_order += "E"; return co::ERROR_OK; }));
        pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
            [](co::state_t *) -> co::error_e { test60_order += "L"; return co::ERROR_OK; }));
        pack.push_back(co::create_modif<co::CO_MODIF_RETURN_CBK>(flags,
            [](co::state_t *s) -> co::error_e {
                test60_order += s->err == co::ERROR_YIELDED ? "y" : "R";
                return co::ERROR_OK;
            }));
        pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(flags,
            [](co::state_t *) -> co::error_e { test60_order += "X"; return co::ERROR_OK; }));
        pool->sched(test60_caller(pack));
        ASSERT_FN(pool->run());
        DBG("order: %s", test60_order.c_str());
        ASSERT_FN(CHK_BOOL(test60_sum == 6));
        ASSERT_FN(CHK_BOOL(test60_order == "CELy" "CELy" "CELRX"));
    }
    {
        auto pool = co::create_pool();
        auto sem = co::create_sem(pool, 0);
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        pool->sched(co::add_modifs(pool.get(), test60_target(sem), mods));
        pool->sched(test60_killer(kill_fn));
        ASSERT_FN(pool->run());
        ASSERT_FN(CHK_BOOL(test60_kill_ret == co::ERROR_OK));
        ASSERT_FN(CHK_BOOL(!test60_after_wait));
        ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_FINISHED));
    }
    return 0;
}

int main() {
    int ret = test60_return();
    print_test_result("011-009-modifs_return.cpp", ret >= 0);
    return ret;
}
