#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test21 - IO: stop_fd
================================================================================================= */

#if COLIB_OS_LINUX || COLIB_OS_UNIX

co::task_t test21_co_stop_fd_registered() {
    /* Test case 1: fd was given to the coro engine */
    int fds[2];
    ASSERT_COFN(pipe(fds) == 0);
    FnScope scope([&] { close(fds[0]); close(fds[1]); });
    
    /* Write to one end to make read on other end registerable */
    char buf[1] = {0};
    ASSERT_COFN(write(fds[1], buf, sizeof(buf)) == sizeof(buf));
    
    /* Register fds[0] with the coro engine by scheduling a read */
    auto read_task = [] (int fd) -> co::task_t {
        char read_buf[1];
        co_await co::read(fd, read_buf, sizeof(read_buf));
        co_return 0;
    };
    
    /* Schedule the read but don't await it - just register the fd */
    co_await co::sched(read_task(fds[0]));
    
    /* Now stop_fd should work since fd is registered */
    int stop_ret = co_await co::stop_fd(fds[0]);
    ASSERT_COFN(CHK_BOOL(stop_ret == co::ERROR_OK || stop_ret == co::ERROR_WAKEUP));
    
    DBG("stop_fd: registered fd stopped successfully");
    co_return 0;
}

co::task_t test21_co_stop_fd_not_registered() {
    /* Test case 2: fd was NOT given to the coro engine */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_COFN(fd >= 0);
    FnScope scope([fd] { close(fd); });
    
    /* Don't register the fd - just call stop_fd directly */
    /* This should still work, just won't find anything to stop */
    int stop_ret = co_await co::stop_fd(fd);
    ASSERT_COFN(CHK_BOOL(stop_ret == co::ERROR_OK || stop_ret == co::ERROR_WAKEUP));
    
    DBG("stop_fd: not-registered fd stopped (nothing to do)");
    co_return 0;
}

#else

co::task_t test21_co_stop_fd_not_supported() {
    /* stop_fd is not supported on this OS */
    DBG("stop_fd: not supported on this OS");
    co_return 0;
}

#endif

int test21_stop_fd() {
    auto pool = co::create_pool();
    
#if COLIB_OS_LINUX || COLIB_OS_UNIX
    pool->sched(test21_co_stop_fd_registered());
    pool->sched(test21_co_stop_fd_not_registered());
#else
    pool->sched(test21_co_stop_fd_not_supported());
#endif
    
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test21_stop_fd();
    print_test_result("5-3-io_stop_fd.cpp", ret >= 0);
    return ret;
}
