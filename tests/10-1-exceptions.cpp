#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test13 - Exceptions
================================================================================================= */

int test13_val_inc = 0;

co::task_t test13_c() {
    test13_val_inc += 1;
    FnScope scope([]{
        test13_val_inc += 10;
    });
    throw std::runtime_error("test13");
    test13_val_inc += 1;
    co_return 0;
}

co::task_t test13_b() {
    FnScope scope([]{
        test13_val_inc += 1000;
    });
    test13_val_inc += 100;
    co_await test13_c();
    test13_val_inc += 100;
    co_return 0;
}

co::task_t test13_a() {
    try {
        test13_val_inc += 10000;
        co_await test13_b();
        test13_val_inc += 10000;
    }
    catch (const std::exception& ex) {
        ASSERT_COFN(CHK_BOOL(std::string(ex.what()) == "test13"));
        test13_val_inc += 100000;
    }
    test13_val_inc += 1000000;
    co_return 0;
}

co::task_t test13_exception_test() {
    co_await test13_a();
    test13_val_inc += 10000000;
    throw std::runtime_error("Custom exception");
    co_return 0;
}

int test13_except() {
    auto pool = co::create_pool();
    pool->sched(test13_exception_test());
    bool excepted = false;
    try {
        pool->run();
    }
    catch (std::exception &ex) {
        excepted = true;
        ASSERT_FN(CHK_BOOL(std::string(ex.what()) == "Custom exception"));
    }
    ASSERT_FN(CHK_BOOL(excepted));
    ASSERT_FN(CHK_BOOL(test13_val_inc == 11111111));
    return 0;
}

int main() {
    int ret = test13_except();
    print_test_result("10-1-exceptions.cpp", ret >= 0);
    return ret;
}
