#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test19 - Flow Control: create_killer
================================================================================================= */

/* create_killer(pool, e) returns {modif_pack_t, kill_fn}. Attach the modif_pack_t to a task via
add_modifs(); calling kill_fn() later force-destroys that task's entire call stack. This exercises
the semaphore-waiter kill path specifically (colib.h's own sig_kill() has a dedicated branch for a
coroutine parked on a semaphore, erasing it from the wait queue before destroying it). */

int test19_destruct_cnt = 0;

struct test19_marker_t {
    ~test19_marker_t() { test19_destruct_cnt++; }
};

co::task_t test19_victim(co::sem_p block_forever) {
    test19_marker_t marker;
    co_await block_forever->wait(); /* never signaled: would block forever without the killer */
    test19_destruct_cnt += 1000;    /* must NOT run: killed, not resumed */
    co_return 0;
}

co::task_t test19_controller(co::sem_p block_forever, std::function<co::error_e(void)> kill_fn) {
    co_await co::yield(); /* let test19_victim run first and register as a semaphore waiter */
    co::error_e ret = kill_fn();
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));
    co_return 0;
}

int test19_create_killer() {
    auto pool = co::create_pool();
    auto block_forever = co::create_sem(pool, 0);

    auto [killer_mods, kill_fn] = co::create_killer(pool.get(), co::ERROR_USER);

    auto victim = co::add_modifs(pool.get(), test19_victim(block_forever), killer_mods);
    pool->sched(victim);
    pool->sched(test19_controller(block_forever, kill_fn));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test19_destruct_cnt == 1)); /* victim's stack was force-destroyed, not resumed */

    /* nothing left to kill: colib.h's sig_kill() reports this when the call stack is empty */
    ASSERT_FN(CHK_BOOL(kill_fn() == co::ERROR_GENERIC));

    return 0;
}

int main() {
    int ret = test19_create_killer();
    print_test_result("2-2-flowctrl_create_killer.cpp", ret >= 0);
    return ret;
}
