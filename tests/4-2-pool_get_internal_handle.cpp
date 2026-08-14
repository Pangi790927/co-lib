#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#if COLIB_OS_LINUX || COLIB_OS_UNIX
#include <fcntl.h>
#endif

/* Test22 - Pool: get_internal_handle
================================================================================================= */

co::task_t test22_coro() {
    co_return 0;
}

int test22_get_internal_handle() {
    co::pool_p pool1 = co::create_pool();
    co::pool_p pool2 = co::create_pool();

    intptr_t h1 = pool1->get_internal_handle();
    intptr_t h2 = pool2->get_internal_handle();

    ASSERT_FN(CHK_BOOL(h1 >= 0));
    ASSERT_FN(CHK_BOOL(h2 >= 0));
    ASSERT_FN(CHK_BOOL(h1 != h2)); /* two live pools must have distinct handles (distinct epoll fds) */

#if COLIB_OS_LINUX || COLIB_OS_UNIX
    /* on Linux/UNIX this is a real epoll/kqueue fd, so fcntl must succeed on it */
    ASSERT_FN(CHK_BOOL(fcntl((int)h1, F_GETFD) != -1));
    ASSERT_FN(CHK_BOOL(fcntl((int)h2, F_GETFD) != -1));
#endif

    pool1->sched(test22_coro());
    co::run_e ret = pool1->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    return 0;
}

int main() {
    int ret = test22_get_internal_handle();
    print_test_result("4-2-pool_get_internal_handle.cpp", ret >= 0);
    return ret;
}
