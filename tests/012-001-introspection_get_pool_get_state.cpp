#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test16 - Introspection: get_pool/get_state
================================================================================================= */

int test16_counter = 0;
co::pool_t *test16_pool = nullptr;

co::task_t test16_coro() {
    co::state_t *state = co_await co::get_state();
    state->user_ptr = std::shared_ptr<int>(new int, [](int *c){ delete c; test16_counter++; });
    ASSERT_COFN(CHK_BOOL(test16_pool == co_await co::get_pool()));
    test16_counter++;
    co_return 0;
}

int test16_getters() {
    co::pool_p pool = co::create_pool();
    test16_pool = pool.get();
    pool->sched(test16_coro());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test16_counter == 2));
    return 0;
}

int main() {
    int ret = test16_getters();
    print_test_result("012-001-introspection_get_pool_get_state.cpp", ret >= 0);
    return ret;
}
