#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test26 - Semaphores: signal() with a negative increment
================================================================================================= */

/* colib.h docs: "If increment is less than 0, then it will decrease the internal counter with the
amount." No coroutines/pool->run() needed: signal()/try_dec() are plain (non-awaitable) functions. */

int test26_signal_negative() {
    auto pool = co::create_pool();
    auto sem = co::create_sem(pool, 3);

    ASSERT_FN(CHK_BOOL(sem->signal(-2) == co::ERROR_OK)); /* counter: 3 -> 1 */

    int ok_count = 0;
    for (int i = 0; i < 3; i++)
        if (sem->try_dec())
            ok_count++;
    ASSERT_FN(CHK_BOOL(ok_count == 1));

    return 0;
}

int main() {
    int ret = test26_signal_negative();
    print_test_result("001-009-semaphore_signal_negative.cpp", ret >= 0);
    return ret;
}
