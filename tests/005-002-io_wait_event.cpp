#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#if COLIB_OS_LINUX
#include <unistd.h>
#endif

/* Test20 - IO: wait_event
================================================================================================= */

/* wait_event(io_desc) wraps a raw io_desc_t in an awaiter and resolves to ERROR_OK when that OS
event fires - meant for events colib.h doesn't already wrap itself (see its doc comment). io_desc_t
is a completely different struct per OS (Linux: {fd, events} with epoll flags; UNIX: {ident, filter,
fflags, data} with kqueue filters; Windows: HANDLE-based) - only the Linux path is implemented and
verified here; UNIX/Windows would need their own OS-specific io_desc_t construction. */

#if COLIB_OS_LINUX

co::task_t test20_wait_event_coro() {
    int fds[2];
    ASSERT_COFN(pipe(fds) == 0);
    FnScope scope([&] { close(fds[0]); close(fds[1]); });

    char written = 'x';
    ASSERT_COFN(write(fds[1], &written, sizeof(written)) == sizeof(written));

    co::io_desc_t desc{ .fd = fds[0], .events = EPOLLIN };
    co::error_e ret = (co::error_e)co_await co::wait_event(desc);
    ASSERT_COFN(CHK_BOOL(ret == co::ERROR_OK));

    /* prove the fd really was readable when wait_event resolved, not just that the call returned OK */
    char read_back = 0;
    ASSERT_COFN(CHK_BOOL(read(fds[0], &read_back, sizeof(read_back)) == 1));
    ASSERT_COFN(CHK_BOOL(read_back == written));

    co_return 0;
}

#else

co::task_t test20_wait_event_coro() {
    DBG("wait_event: not supported/verified on this OS");
    co_return 0;
}

#endif

int test20_wait_event() {
    co::pool_p pool = co::create_pool();
    pool->sched(test20_wait_event_coro());
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test20_wait_event();
    print_test_result("005-002-io_wait_event.cpp", ret >= 0);
    return ret;
}
