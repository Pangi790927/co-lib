#define COLIB_ENABLE_DEBUG_NAMES true
#define COLIB_ENABLE_DEBUG_CHECKS true
#define COLIB_ENABLE_MULTITHREAD_SCHED true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

/* Test75 - Pool: thread_sched() called by a SCHED callback of a thread_sched()'d coroutine
================================================================================================= */

/* A coroutine handed over with thread_sched() gets its SCHED callbacks on the pool's thread, when
the pool takes it in. One of them calls thread_sched() on the same pool, from that thread: it must
not deadlock on the lock the pool holds while taking the handed-over coroutines in, and the
coroutine it hands over runs too. A watchdog fails the test instead of letting it hang. */

static co::pool_t *test75_pool = nullptr;
static std::atomic<bool> test75_first_ran = false;
static std::atomic<bool> test75_second_ran = false;
static std::atomic<bool> test75_done = false;

static co::task_t test75_second() {
    test75_second_ran = true;
    co_return 0;
}

static co::task_t test75_first() {
    test75_first_ran = true;
    co_return 0;
}

static co::task_t test75_keep_busy() {
    for (int i = 0; i < 3000 && !(test75_first_ran && test75_second_ran); i++)
        co_await co::sleep_ms(1);
    co_return 0;
}

int test75_thread_sched_from_sched_cbk() {
    std::thread watchdog([] {
        for (int i = 0; i < 500 && !test75_done; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!test75_done) {
            DBG("hung: thread_sched() from a SCHED callback deadlocked");
            print_test_result("004-006-pool_thread_sched_from_sched_cbk.cpp", false);
            std::_Exit(1);
        }
    });

    auto pool = co::create_pool();
    test75_pool = pool.get();

    /* the modifs go on before the handover, on this thread */
    auto on_sched = co::create_modif<co::CO_MODIF_SCHED_CBK>(co::CO_MODIF_INHERIT_NONE,
        [](co::state_t *) -> co::error_e {
            test75_pool->thread_sched(test75_second());   /* on the pool's thread */
            return co::ERROR_OK;
        });
    auto first = co::add_modifs(pool.get(), test75_first(), co::modif_pack_t(1, on_sched));

    pool->sched(test75_keep_busy());
    std::thread other([&pool, first]() mutable { pool->thread_sched(first); });

    co::run_e ret = pool->run();
    other.join();
    test75_done = true;
    watchdog.join();

    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test75_first_ran));
    ASSERT_FN(CHK_BOOL(test75_second_ran));
    return 0;
}

int main() {
    int ret = test75_thread_sched_from_sched_cbk();
    print_test_result("004-006-pool_thread_sched_from_sched_cbk.cpp", ret >= 0);
    return ret;
}
