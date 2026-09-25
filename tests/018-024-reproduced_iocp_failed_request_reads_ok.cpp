#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test77 - Reproduced Bugs: on IOCP a request that failed reads as a success with 0 bytes
================================================================================================= */

/* The Windows io_pool_t's handle_ready_events() takes each completion packet and sets its
coroutine's err to ERROR_OK, whatever the packet's status says. handle_done_req() then trusts that
err, so a request that failed returns TRUE with 0 bytes. For co::read() on a socket that means a
connection reset reads as 0: a clean end of stream, not an error. Here the peer resets the
connection (SO_LINGER 0, then close) while a read is pending: the read must report an error, not
0. Noticed while writing the killer redesign (REDESIGN_KILLER.md, "Open points"), reproduced
2026-09-25. */

#if COLIB_OS_WINDOWS

static SOCKET test77_client = INVALID_SOCKET;
static SSIZE_T test77_ret = 12345;
static bool test77_ok = false;

static co::task_t test77_reader(SOCKET c) {
    char buff[64];
    test77_ret = co_await co::read((HANDLE)c, buff, sizeof(buff));    /* pending when reset */
    test77_ok = true;
    co_return 0;
}

static co::task_t test77_resetter() {
    co_await co::yield();       /* the reader's read is pending */
    linger lin = { 1, 0 };      /* close with a reset (RST), not a graceful shutdown */
    setsockopt(test77_client, SOL_SOCKET, SO_LINGER, (const char *)&lin, sizeof(lin));
    closesocket(test77_client);
    test77_client = INVALID_SOCKET;
    co_return 0;
}

int test77_failed_request_reads_ok() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(srv != INVALID_SOCKET));
    FnScope close_srv([srv]{ closesocket(srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
    int addr_len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &addr_len) == 0));

    test77_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test77_client != INVALID_SOCKET));
    ASSERT_FN(CHK_BOOL(connect(test77_client, (sockaddr *)&addr, sizeof(addr)) == 0));
    SOCKET c = accept(srv, NULL, NULL);
    ASSERT_FN(CHK_BOOL(c != INVALID_SOCKET));
    FnScope close_c([c]{ closesocket(c); });

    auto pool = co::create_pool();
    pool->sched(test77_reader(c));
    pool->sched(test77_resetter());
    ASSERT_FN(pool->run());

    DBG("read after a reset returned %lld", (long long)test77_ret);
    ASSERT_FN(CHK_BOOL(test77_ok));
    ASSERT_FN(CHK_BOOL(test77_ret < 0));    /* an error, not 0 (end of stream) */
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test77_failed_request_reads_ok() {
    DBG("IOCP only: epoll/kqueue report the error from read() itself");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test77_failed_request_reads_ok();
    print_test_result("018-024-reproduced_iocp_failed_request_reads_ok.cpp", ret >= 0);
    return ret;
}
