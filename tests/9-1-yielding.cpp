#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test12 - Yielding
================================================================================================= */

co::task_t test12_co_yielder() {
    for (int i = 0; i < 14; i++) {
        co_yield i;
    }
    co_return 14;
}

co::task_t test12_yield_test() {
    auto yielder = test12_co_yielder();
    for (int i = 0; i < 15; i++)
        if (i != co_await yielder) {
            DBG("Bad yield");
            co_return -1;
        }
    co_return 0;
}

int test12_yielding() {
    auto pool = co::create_pool();
    pool->sched(test12_yield_test());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test12_yielding();
    print_test_result("9-1-yielding.cpp", ret >= 0);
    return ret;
}
