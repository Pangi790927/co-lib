#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true

#include "../colib.h"
#include "tests_common.h"

/* Test59 - Modifs: CO_MODIF_YIELD_CBK / CO_MODIF_UNYIELD_CBK on co::yield()
================================================================================================= */

/* co::yield() is a wait on the ready queue, paired like the io and semaphore waits: LEAVE, YIELD
when it goes in the queue, UNYIELD then ENTER when it is resumed. A yield refused by a modif is
closed at once (UNYIELD, ENTER) and continues. A kill of a coroutine queued by its yield closes
the yield too (UNYIELD) before it destroys it. The debug checks follow the same pairs. */

static std::string test59_order;
static std::string test59_victim_order;
static bool test59_victim_after = false;

static co::task_t test59_yielder() {
    co_await co::yield();
    co_await co::yield();
    co_return 0;
}

static co::task_t test59_victim() {
    co_await co::yield();       /* killed while queued */
    test59_victim_after = true;
    co_return 0;
}

static co::task_t test59_killer(std::function<co::error_e(void)> kill_fn) {
    kill_fn();
    co_return 0;
}

static co::modif_pack_t test59_recorder(std::string *order, co::error_e yield_ret) {
    auto flags = co::CO_MODIF_INHERIT_NONE;
    co::modif_pack_t pack;
    pack.push_back(co::create_modif<co::CO_MODIF_ENTER_CBK>(flags,
        [order](co::state_t *) -> co::error_e { *order += "E"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_LEAVE_CBK>(flags,
        [order](co::state_t *) -> co::error_e { *order += "L"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_YIELD_CBK>(flags,
        [order, yield_ret](co::state_t *) -> co::error_e { *order += "Y"; return yield_ret; }));
    pack.push_back(co::create_modif<co::CO_MODIF_UNYIELD_CBK>(flags,
        [order](co::state_t *) -> co::error_e { *order += "U"; return co::ERROR_OK; }));
    pack.push_back(co::create_modif<co::CO_MODIF_EXIT_CBK>(flags,
        [order](co::state_t *) -> co::error_e { *order += "X"; return co::ERROR_OK; }));
    return pack;
}

int test59_yield_pair() {
    {
        auto pool = co::create_pool();
        pool->sched(co::add_modifs(pool.get(), test59_yielder(),
                test59_recorder(&test59_order, co::ERROR_OK)));
        ASSERT_FN(pool->run());
        DBG("order: %s", test59_order.c_str());
        ASSERT_FN(CHK_BOOL(test59_order == "E" "LYUE" "LYUE" "LX"));
    }
    {
        test59_order.clear();
        auto pool = co::create_pool();
        pool->sched(co::add_modifs(pool.get(), test59_yielder(),
                test59_recorder(&test59_order, co::ERROR_USER)));
        ASSERT_FN(pool->run());
        DBG("refused: %s", test59_order.c_str());
        ASSERT_FN(CHK_BOOL(test59_order == "E" "LYUE" "LYUE" "LX"));
    }
    {
        auto pool = co::create_pool();
        auto [mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);
        auto victim = co::add_modifs(pool.get(), test59_victim(),
                test59_recorder(&test59_victim_order, co::ERROR_OK));
        pool->sched(co::add_modifs(pool.get(), victim, mods));
        pool->sched(test59_killer(kill_fn));
        ASSERT_FN(pool->run());
        DBG("killed: %s", test59_victim_order.c_str());
        ASSERT_FN(CHK_BOOL(test59_victim_order == "E" "LYU" "X"));
        ASSERT_FN(CHK_BOOL(!test59_victim_after));
    }
    return 0;
}

int main() {
    int ret = test59_yield_pair();
    print_test_result("011-008-modifs_yield_pair.cpp", ret >= 0);
    return ret;
}
