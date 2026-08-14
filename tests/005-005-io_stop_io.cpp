#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#if COLIB_OS_LINUX
#include <unistd.h>
#endif

/* Test29 - IO: stop_io
================================================================================================= */

/* stop_io(io_desc) itself always resolves to ERROR_OK regardless of whether anything was actually
being waited on (confirmed in colib.h's epoll force_awake(): unregistered fd -> early ERROR_OK,
registered fd -> sets the waiter's state->err = ERROR_WAKEUP and re-queues it, then still returns
ERROR_OK). So the real proof stop_io() worked is checking the *waiter's* result, not stop_io()'s own
return value. */

#if COLIB_OS_LINUX

int test29_woken_with = -999;

co::task_t test29_waiter(int fd) {
    co::io_desc_t desc{ .fd = fd, .events = EPOLLIN };
    test29_woken_with = (int)co_await co::wait_event(desc);
    co_return 0;
}

co::task_t test29_stopper(int fd) {
    co_await co::yield(); /* let test29_waiter register first (FIFO ready-queue order) */
    co::io_desc_t desc{ .fd = fd, .events = EPOLLIN };
    co::error_e ret = (co::error_e)co_await co::stop_io(desc);
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK)); /* stop_io() itself always reports OK */
    co_return 0;
}

int test29_stop_io_registered() {
    int fds[2];
    ASSERT_FN(pipe(fds) == 0);
    FnScope scope([&] { close(fds[0]); close(fds[1]); });
    /* nothing written to fds[1]: fds[0] would never become readable on its own */

    auto pool = co::create_pool();
    pool->sched(test29_waiter(fds[0]));
    pool->sched(test29_stopper(fds[0]));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test29_woken_with == co::ERROR_WAKEUP));

    return 0;
}

co::task_t test29_stop_unregistered_coro(int fd) {
    co::io_desc_t desc{ .fd = fd, .events = EPOLLIN };
    co::error_e ret = (co::error_e)co_await co::stop_io(desc);
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK)); /* no-op: nothing was waiting, still reports OK */
    co_return 0;
}

int test29_stop_io_not_registered() {
    int fds[2];
    ASSERT_FN(pipe(fds) == 0);
    FnScope scope([&] { close(fds[0]); close(fds[1]); });

    auto pool = co::create_pool();
    pool->sched(test29_stop_unregistered_coro(fds[0]));

    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));

    return 0;
}

#else

int test29_stop_io_registered() {
    DBG("stop_io: not supported/verified on this OS");
    return 0;
}

int test29_stop_io_not_registered() {
    return 0;
}

#endif

int main() {
    int ret = test29_stop_io_registered();
    if (ret >= 0)
        ret = test29_stop_io_not_registered();
    print_test_result("005-005-io_stop_io.cpp", ret >= 0);
    return ret;
}
