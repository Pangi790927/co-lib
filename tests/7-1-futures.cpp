#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test10 - Futures
================================================================================================= */

struct test10_data_t {
    std::string name;
    int val = 0;
};

co::task<test10_data_t> test10_future_result() {
    co_return test10_data_t{ .name = "test10", .val = 10 };
}

co::task_t test10_co_futures() {
    auto t = test10_future_result();
    auto f = co::create_future(co_await co::get_pool(), t);
    co_await co::sched(t);
    auto [name, val] = co_await f;
    ASSERT_COFN(CHK_BOOL(name == "test10" && val == 10));
    co_return 0;
}

int test10_futures() {
    auto pool = co::create_pool();
    pool->sched(test10_co_futures());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test10_futures();
    print_test_result("7-1-futures.cpp", ret >= 0);
    return ret;
}
