#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test98 - Reproduced Bugs: co::accept on Windows answered success when the accept context failed
================================================================================================= */

/* co::accept must not hand out a socket on which getpeername and shutdown fail. On Windows the
accepted socket becomes an ordinary connected one only once SO_UPDATE_ACCEPT_CONTEXT is set on it
with the listener's handle; co::accept set it but ignored the result, and returned the socket as a
success when the update failed. The listener is closed here from an UNWAIT_IO callback, after
AcceptEx completed and before co::accept updates the context, which makes the update fail. The
accept must then answer INVALID_SOCKET with ERROR_GENERIC and a reason in GetLastError(). Found by
review 10-10-2026; fix #38 (tests/fixes/FIX_38-accept_context_unchecked.md). */

#if COLIB_OS_WINDOWS

static SOCKET test98_srv = INVALID_SOCKET;
static bool   test98_closed = false;
static bool   test98_ok = false;
static int    test98_err = 0;
static DWORD  test98_last_error = 0;

static co::task_t test98_acceptor() {
    sockaddr_in sa = {};
    uint32_t len = sizeof(sa);
    co::error_e err = co::ERROR_OK;
    SOCKET c = co_await co::accept(test98_srv, (sockaddr *)&sa, &len, &err);
    test98_last_error = GetLastError();
    test98_err = err;
    if (c != INVALID_SOCKET) {
        sockaddr_in peer = {};
        int plen = sizeof(peer);
        int rc = getpeername(c, (sockaddr *)&peer, &plen);
        DBG("accept answered a socket, getpeername rc %d (%d)", rc, WSAGetLastError());
        closesocket(c);
        co_return -1;
    }
    test98_ok = true;
    co_return 0;
}

int test98_accept_context_unchecked() {
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    test98_srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test98_srv != INVALID_SOCKET));
    FnScope close_srv([]{ if (!test98_closed) closesocket(test98_srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(test98_srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(test98_srv, 1) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(test98_srv, (sockaddr *)&addr, &len) == 0));

    /* the client connects before the accept is asked for: the listen queue holds it */
    SOCKET cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));

    /* the accept's only wait is AcceptEx's: its end is the moment to close the listener */
    auto closer = co::create_modif<co::CO_MODIF_UNWAIT_IO_CBK>(co::CO_MODIF_INHERIT_ON_CALL,
        [](co::state_t *, co::io_desc_t&) -> co::error_e {
            if (!test98_closed) {
                closesocket(test98_srv);
                test98_closed = true;
            }
            return co::ERROR_OK;
        });

    auto pool = co::create_pool();
    pool->sched(co::add_modifs(pool.get(), test98_acceptor(), co::modif_pack_t(1, closer)));
    ASSERT_FN(pool->run());

    DBG("listener closed %d, accept err %d, last error %lu", test98_closed, test98_err,
            test98_last_error);
    ASSERT_FN(CHK_BOOL(test98_closed));
    ASSERT_FN(CHK_BOOL(test98_ok));
    ASSERT_FN(CHK_BOOL(test98_err == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test98_last_error != 0));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test98_accept_context_unchecked() {
    DBG("SO_UPDATE_ACCEPT_CONTEXT is a Windows-only step: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test98_accept_context_unchecked();
    print_test_result("018-042-reproduced_accept_context_unchecked.cpp", ret >= 0);
    return ret;
}
