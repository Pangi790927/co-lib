#define COLIB_ENABLE_DEBUG_NAMES true

/* The load of GetAcceptExSockaddrs fails only when WSAIoctl does, which no input to co::accept can
cause. This test therefore stands between colib.h and WSAIoctl: Winsock comes in first, under the
macros colib.h would set itself, and colib.h then calls test99_wsaioctl in WSAIoctl's place. That
is the one exception to the suite's include order (colib.h first), for that reason only. */
#if defined(_WIN32)
# define WIN32_LEAN_AND_MEAN
# define NOMINMAX
# define _WIN32_WINNT 0x0600
# include <winsock2.h>
# include <mswsock.h>
# include <windows.h>
# include <string.h>

static bool test99_fail_load = false;

static int test99_wsaioctl(SOCKET s, DWORD code, LPVOID in, DWORD in_len, LPVOID out,
        DWORD out_len, LPDWORD ret_len, LPWSAOVERLAPPED ov,
        LPWSAOVERLAPPED_COMPLETION_ROUTINE cbk)
{
    GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
    if (test99_fail_load && code == SIO_GET_EXTENSION_FUNCTION_POINTER
            && in_len == sizeof(guid) && memcmp(in, &guid, sizeof(guid)) == 0) {
        WSASetLastError(WSAEOPNOTSUPP);
        return SOCKET_ERROR;
    }
    return WSAIoctl(s, code, in, in_len, out, out_len, ret_len, ov, cbk);
}

# define WSAIoctl test99_wsaioctl
#endif /* _WIN32 */

#include "../colib.h"
#include "tests_common.h"

/* Test99 - Reproduced Bugs: co::accept on Windows lost why its GetAcceptExSockaddrs load failed,
and load_win_fn leaked its dummy socket on that failure
================================================================================================= */

/* co::accept documents that on INVALID_SOCKET with ERROR_GENERIC the reason is GetLastError(). When
the load of GetAcceptExSockaddrs failed, co::accept closed the accepted socket before returning, and
the successful closesocket reset the thread's last error to 0, so the caller read no reason at all
(fix #39). load_win_fn itself returned from a failed WSAIoctl without closing the dummy socket it
had made for the call: one socket leaked per failed load (fix #48). The load is made to fail here
with WSAEOPNOTSUPP; the accept must answer INVALID_SOCKET, ERROR_GENERIC and that same code in
GetLastError(), and a second failing accept must leave the process's handle count where the first
one left it. Each run makes its own listener: a socket keeps the completion port it was first
associated with, so one listener cannot serve a second pool. Found by review 10-10-2026. */

#if COLIB_OS_WINDOWS

static bool  test99_ok = false;
static int   test99_err = 0;
static DWORD test99_last_error = 0;

static co::task_t test99_acceptor(SOCKET srv) {
    sockaddr_in sa = {};
    uint32_t len = sizeof(sa);
    co::error_e err = co::ERROR_OK;
    SOCKET c = co_await co::accept(srv, (sockaddr *)&sa, &len, &err);
    test99_last_error = GetLastError();
    test99_err = err;
    if (c != INVALID_SOCKET) {
        closesocket(c);
        co_return -1;
    }
    test99_ok = true;
    co_return 0;
}

/* one listener, one client that connects before the accept is asked for, one accept with the
load failing; everything is closed again before returning, so two runs make the same handles */
static int test99_failing_accept() {
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
    ASSERT_FN(CHK_BOOL(cl != INVALID_SOCKET));
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));

    test99_ok = false;
    test99_fail_load = true;
    {
        auto pool = co::create_pool();
        pool->sched(test99_acceptor(srv));
        ASSERT_FN(pool->run());
    }
    test99_fail_load = false;

    DBG("accept err %d, last error %lu (expected %d)", test99_err, test99_last_error,
            WSAEOPNOTSUPP);
    ASSERT_FN(CHK_BOOL(test99_ok));
    ASSERT_FN(CHK_BOOL(test99_err == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test99_last_error == WSAEOPNOTSUPP));
    return 0;
}

int test99_accept_load_last_error() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    /* the first failing accept also makes Winsock's one-time handles; the count is taken after
    it, and a second failing accept must leave it as it is */
    ASSERT_FN(test99_failing_accept());
    DWORD before = 0, after = 0;
    ASSERT_FN(CHK_BOOL(GetProcessHandleCount(GetCurrentProcess(), &before)));
    ASSERT_FN(test99_failing_accept());
    ASSERT_FN(CHK_BOOL(GetProcessHandleCount(GetCurrentProcess(), &after)));
    DBG("handles after the first failing accept %lu, after the second %lu", before, after);
    ASSERT_FN(CHK_BOOL(after == before));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test99_accept_load_last_error() {
    DBG("GetAcceptExSockaddrs is a Windows-only step: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test99_accept_load_last_error();
    print_test_result("018-043-reproduced_accept_load_last_error.cpp", ret >= 0);
    return ret;
}
