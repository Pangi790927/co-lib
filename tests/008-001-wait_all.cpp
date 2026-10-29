#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test11 - wait_all
================================================================================================= */

co::task_t test11_co_wait_all() {
    auto ret = co_await co::wait_all(
        []() -> co::task<std::string> { co_return "test11"; }(),
        []() -> co::task<int> { co_return 11; }(),
        []() -> co::task<float> { co_return 0.11f; }()
    );

    /* each task reports for itself: its value and ERROR_OK, since all three returned */
    bool is_ok = ret == std::tuple<std::pair<std::string, co::error_e>, std::pair<int, co::error_e>,
            std::pair<float, co::error_e>>{ {"test11", co::ERROR_OK}, {11, co::ERROR_OK},
            {0.11f, co::ERROR_OK} };
    ASSERT_COFN(CHK_BOOL(is_ok));

    co_return 0;
}

int test11_wait_all() {
    auto pool = co::create_pool();
    pool->sched(test11_co_wait_all());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test11_wait_all();
    print_test_result("008-001-wait_all.cpp", ret >= 0);
    return ret;
}
