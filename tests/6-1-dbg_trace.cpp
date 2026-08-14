#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test9 - DBG Trace
================================================================================================= */

/* TODO: more tests here */
/* This test requires visual inspection */

co::task_t test9_dbg_call() {
    DBG("called...");
    co_return 0;
}

co::task_t test9_dbg_sched() {
    DBG("scheduled...");
    co_await COLIB_REGNAME(test9_dbg_call());
    co_return 0;
}

int test9_dbg_trace() {
    auto pool = co::create_pool();
    auto trace = co::dbg_create_tracer(pool.get());
    pool->sched(COLIB_REGNAME(test9_dbg_sched()), trace);
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test9_dbg_trace();
    print_test_result("6-1-dbg_trace.cpp", ret >= 0);
    return ret;
}
