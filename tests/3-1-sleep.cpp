#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test6 - Sleep
================================================================================================= */

int test6_num = 0;

co::task_t test6_co_sleep_1000us() {
    co_await co::sleep_us(1000);
    test6_num = 1;
    DBG("Done");
    co_return 0;
}

co::task_t test6_co_sleep_100ms() {
    co_await co::sleep_ms(100);
    test6_num *= 100;
    DBG("Done");
    co_return 0;
}

co::task_t test6_co_sleep_1s() {
    co_await co::sleep_s(1);
    test6_num += 5;
    DBG("Done");
    co_return 0;
}

int test6_sleeping() {
    auto pool = co::create_pool();
    pool->sched(test6_co_sleep_1s());
    pool->sched(test6_co_sleep_100ms());
    pool->sched(test6_co_sleep_1000us());

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test6_num == 105));
    return 0;
}

int main() {
    int ret = test6_sleeping();
    print_test_result("3-1-sleep.cpp", ret >= 0);
    return ret;
}
