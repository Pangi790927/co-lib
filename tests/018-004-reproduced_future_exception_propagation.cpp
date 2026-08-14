#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test34 - Reproduced Bugs: create_future() forwards exceptions instead of crashing
================================================================================================= */

/* create_future()'s EXIT_CBK used to do `data->first = std::get<T>(h.promise().ret)`
unconditionally. If the wrapped task exited via an unhandled exception rather than co_return, ret
was still std::monostate and that std::get threw std::bad_variant_access - masking the real
exception and blowing up inside the EXIT_CBK itself instead of surfacing at the future's co_await
point. Fixed in colib.h: the EXIT_CBK now checks state->exception first and stores it, and the
future's own body rethrows it (or default-constructs T / throws a clear runtime_error if state has
neither a value nor an exception - the same monostate case fixed in
18-2-reproduced_call_modif_failure_default.cpp's await_resume() fix). */

co::task<int> test34_worker_ok() {
    co_return 42;
}

co::task<int> test34_worker_throws() {
    throw std::runtime_error("boom");
    co_return 0; /* unreachable, needed for the coroutine's return type */
}

int test34_success_result = -1;

co::task_t test34_success_caller() {
    auto pool = co_await co::get_pool();
    auto t = test34_worker_ok();
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    test34_success_result = co_await fut;
    co_return 0;
}

std::string test34_exception_msg;
bool test34_exception_caught = false;

co::task_t test34_exception_caller() {
    auto pool = co_await co::get_pool();
    auto t = test34_worker_throws();
    auto fut = co::create_future(pool, t);
    co_await co::sched(t);
    try {
        co_await fut;
    } catch (const std::exception& e) {
        test34_exception_caught = true;
        test34_exception_msg = e.what();
    }
    co_return 0;
}

int test34_future_exception_propagation() {
    auto pool = co::create_pool();
    pool->sched(test34_success_caller());
    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test34_success_result == 42));

    auto pool2 = co::create_pool();
    pool2->sched(test34_exception_caller());
    ASSERT_FN(pool2->run());
    ASSERT_FN(CHK_BOOL(test34_exception_caught));
    ASSERT_FN(CHK_BOOL(test34_exception_msg == "boom"));

    return 0;
}

int main() {
    int ret = test34_future_exception_propagation();
    print_test_result("018-004-reproduced_future_exception_propagation.cpp", ret >= 0);
    return ret;
}
