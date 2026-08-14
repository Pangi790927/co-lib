#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test17 - Modifs: await()
================================================================================================= */

int test17_counter = 0;

co::task_t test17_other() {
    test17_counter *= 3;
    co_return 0;
}

co::task_t test17_coro() {
    test17_counter += 1;
    co_await co::sched(test17_other());
    co_await co::await(co::yield());
    test17_counter -= 1;
    co_return 0;
}

int test17_await() {
    co::pool_p pool = co::create_pool();
    pool->sched(test17_coro());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test17_counter == 2));
    return 0;
}

int main() {
    int ret = test17_await();
    print_test_result("011-002-modifs_await.cpp", ret >= 0);
    return ret;
}
