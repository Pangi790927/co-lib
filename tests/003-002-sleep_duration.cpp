#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test18 - Sleep: sleep(c++Duration)
================================================================================================= */

/* co::sleep() only has one overload: sleep(const std::chrono::microseconds&). Coarser durations
(milliseconds, seconds) work via implicit widening conversion. This is templated on Duration so the
same coroutine body exercises microseconds (the native type, no conversion), milliseconds, and
seconds through the same code path. Measures actual wall-clock elapsed time against a margin,
rather than just checking the call compiles/returns OK. */

const auto test18_margin = std::chrono::milliseconds(20);

template <typename Duration>
co::task_t test18_sleep_for(Duration dur, bool *ok) {
    auto dur_ms = std::chrono::duration_cast<std::chrono::milliseconds>(dur);
    auto start = std::chrono::steady_clock::now();
    co_await co::sleep(dur);
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
    *ok = elapsed_ms >= dur_ms && elapsed_ms <= dur_ms + test18_margin;
    if (!*ok) {
        DBG("sleep(%lldms) took %lldms, outside [%lldms, %lldms]",
            (long long)dur_ms.count(), (long long)elapsed_ms.count(),
            (long long)dur_ms.count(), (long long)(dur_ms + test18_margin).count());
    }
    co_return 0;
}

int test18_sleep() {
    auto pool = co::create_pool();
    bool ok_100ms = false, ok_200ms = false, ok_500ms = false, ok_us = false, ok_s = false;

    pool->sched(test18_sleep_for(std::chrono::milliseconds(100), &ok_100ms));
    pool->sched(test18_sleep_for(std::chrono::milliseconds(200), &ok_200ms));
    pool->sched(test18_sleep_for(std::chrono::milliseconds(500), &ok_500ms));
    pool->sched(test18_sleep_for(std::chrono::microseconds(150000), &ok_us));
    pool->sched(test18_sleep_for(std::chrono::seconds(1), &ok_s));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(ok_100ms));
    ASSERT_FN(CHK_BOOL(ok_200ms));
    ASSERT_FN(CHK_BOOL(ok_500ms));
    ASSERT_FN(CHK_BOOL(ok_us));
    ASSERT_FN(CHK_BOOL(ok_s));

    return 0;
}

int main() {
    int ret = test18_sleep();
    print_test_result("003-002-sleep_duration.cpp", ret >= 0);
    return ret;
}
