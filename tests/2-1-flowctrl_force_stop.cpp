#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test5 - Flow Control: Force Stop
================================================================================================= */

int test5_counter = 0;
int test5_stopped = 0;
int test5_increment = 0;

co::task_t test5_co_stop() {
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < 5; j++)
            test5_counter++;
        co_await co::force_stop(i);
    }
    co_return 0;
}

int test5_stopping() {
    auto pool = co::create_pool();
    pool->sched(test5_co_stop());

    while (true) {
        auto ret = pool->run();
        test5_stopped++;
        test5_increment += pool->stopval;
        // pool->stopval = 0; // <- if the stopval is not reset, it keeps it's value 
        if (ret == co::RUN_OK)
            break;
        if (ret != co::RUN_STOPPED) {
            DBG("Something went wrong: %s", co::dbg_enum(ret).c_str());
            return -1;
        }
    }

    ASSERT_FN(CHK_BOOL(test5_stopped == 6));    /* 5 stops + 1 end */
    ASSERT_FN(CHK_BOOL(test5_counter == 25));   /* 5*5 == 25 */
    ASSERT_FN(CHK_BOOL(test5_increment == 14)); /* 0+1+2+3+4+4 the last one from not reseting stopval */

    return 0;
}

int main() {
    int ret = test5_stopping();
    print_test_result("2-1-flowctrl_force_stop.cpp", ret >= 0);
    return ret;
}
