#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test80 - Reproduced Bugs: a stopped co::read reported ERROR_GENERIC on Windows
================================================================================================= */

/* A pending co::read stopped with colib's stop function (stop_fd() on Linux, stop_handle() on
Windows) must return ERROR_WAKEUP, so a caller can tell "woken without data" from a failed
transmission. Linux returned the wait's own ERROR_WAKEUP; on Windows the stop cancels the request,
which then reports ERROR_OPERATION_ABORTED, and co::read turned every failure into ERROR_GENERIC.
Reproduced 2026-09-25. */

static long long test80_got = 12345;
static bool test80_ok = false;

#if COLIB_OS_WINDOWS

static co::task_t test80_reader(SOCKET s) {
    char buff[16];
    test80_got = co_await co::read((HANDLE)s, buff, sizeof(buff));
    test80_ok = true;
    co_return 0;
}

static co::task_t test80_stopper(SOCKET s) {
    co_await co::yield();                       /* the read is pending */
    co_await co::stop_handle((HANDLE)s);
    co_return 0;
}

int test80_stopped_read_code() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(srv != INVALID_SOCKET));
    FnScope close_srv([srv]{ closesocket(srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &len) == 0));
    SOCKET cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));
    SOCKET c = accept(srv, NULL, NULL);
    ASSERT_FN(CHK_BOOL(c != INVALID_SOCKET));
    FnScope close_c([c]{ closesocket(c); });

    auto pool = co::create_pool();
    pool->sched(test80_reader(c));
    pool->sched(test80_stopper(c));
    ASSERT_FN(pool->run());

    DBG("a stopped co::read returned %lld", test80_got);
    ASSERT_FN(CHK_BOOL(test80_ok));
    ASSERT_FN(CHK_BOOL(test80_got == co::ERROR_WAKEUP));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

#include <sys/socket.h>
#include <unistd.h>

static co::task_t test80_reader(int fd) {
    char buff[16];
    test80_got = co_await co::read(fd, buff, sizeof(buff));
    test80_ok = true;
    co_return 0;
}

static co::task_t test80_stopper(int fd) {
    co_await co::yield();                       /* the read is pending */
    co_await co::stop_fd(fd);
    co_return 0;
}

int test80_stopped_read_code() {
    int sv[2];
    ASSERT_FN(CHK_BOOL(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0));
    FnScope close_sv([sv]{ close(sv[0]); close(sv[1]); });

    auto pool = co::create_pool();
    pool->sched(test80_reader(sv[0]));
    pool->sched(test80_stopper(sv[0]));
    ASSERT_FN(pool->run());

    DBG("a stopped co::read returned %lld", test80_got);
    ASSERT_FN(CHK_BOOL(test80_ok));
    ASSERT_FN(CHK_BOOL(test80_got == co::ERROR_WAKEUP));
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test80_stopped_read_code();
    print_test_result("018-027-reproduced_stopped_read_code.cpp", ret >= 0);
    return ret;
}
