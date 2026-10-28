#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test97 - Reproduced Bugs: the Windows I/O wrappers' early failures left *err untouched
================================================================================================= */

/* The Windows I/O wrappers document their optional out err as "ERROR_GENERIC when it failed (the
reason is GetLastError())", but the failures that strike before the request is ever built - the
Winsock extension not loading (load_win_fn) or the event not being created (CreateEvent /
WSACreateEvent) - co_returned FALSE without writing *err at all, and left the byte count as the
caller had it, where a request that failed after its wait zeroes it. A caller that initializes err
to ERROR_OK and computes `ok ? ERROR_OK : err` on return, which is exactly what co::connect does,
then read ERROR_OK back: a success answer for a request that never ran. The load_win_fn paths are
forceable from a real process by never calling WSAStartup, which makes the dummy socket inside
load_win_fn fail; the event-creation paths would need handle-table exhaustion and share the same
fixed exit (fail_unstarted_req). Phase A runs with winsock deliberately down and probes every
wrapper that loads an extension, the count pre-loaded where the wrapper takes one; phase B starts
winsock and checks that the request-level failure paths still write *err, so the fix cannot pass
by breaking the normal error plumbing. Found by static analysis, reproduced 2026-09-28
(repro/11-win_err_unset.md); fix #11 (tests/fixes/FIX_11-early_fail_err_unset.md). */

#if COLIB_OS_WINDOWS

/* results hauled out of the coroutines: a failed ASSERT_COFN only logs (tests/CLAUDE.md) */
static int test97_a1_ok = -1, test97_a1_err = -1;
static int test97_a2_translated = -12345;
static int test97_a3_ok = -1, test97_a3_err = -1, test97_a3_cnt = -1;
static int test97_a4_ok = -1, test97_a4_err = -1, test97_a4_cnt = -1;
static int test97_a5_ok = -1, test97_a5_err = -1, test97_a5_cnt = -1;
static int test97_b1_ok = -1, test97_b1_err = -1;
static int test97_b2_ok = -1, test97_b2_err = -1;
static int test97_b3_read = 0;

static sockaddr_in test97_loopback(unsigned short port) {
    sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);
    return addr;
}

static co::task_t test97_early_fails() {
    sockaddr_in addr = test97_loopback(28011);

    /* A1: direct wrapper call, *err pre-loaded with a sentinel no real write could leave */
    co::error_e err = (co::error_e)1234;
    BOOL ok = co_await co::ConnectEx(INVALID_SOCKET, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err);
    test97_a1_ok = ok ? 1 : 0;
    test97_a1_err = (int)err;

    /* A2: the exact one-line translation co::connect performs on that result */
    co::error_e err2 = co::ERROR_OK;
    BOOL ok2 = co_await co::ConnectEx(INVALID_SOCKET, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err2);
    test97_a2_translated = ok2 ? (int)co::ERROR_OK : (int)err2;

    /* A3: AcceptEx, same load_win_fn early exit; the count pre-loaded, to be zeroed */
    char buff[64];
    DWORD recved = 777;
    co::error_e err3 = (co::error_e)1234;
    BOOL ok3 = co_await co::AcceptEx(INVALID_SOCKET, INVALID_SOCKET, buff, 0,
            sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, &recved, &err3);
    test97_a3_ok = ok3 ? 1 : 0;
    test97_a3_err = (int)err3;
    test97_a3_cnt = (int)recved;

    /* A4, A5: the other two wrappers that load an extension, same shape */
    WSABUF wbuf = { 0, NULL };
    WSAMSG msg;
    ZeroMemory(&msg, sizeof(msg));
    msg.name = (sockaddr *)&addr;
    msg.namelen = (int)sizeof(addr);
    msg.lpBuffers = &wbuf;
    msg.dwBufferCount = 1;
    DWORD sent = 777;
    co::error_e err4 = (co::error_e)1234;
    BOOL ok4 = co_await co::WSASendMsg(INVALID_SOCKET, &msg, 0, &sent, NULL, &err4);
    test97_a4_ok = ok4 ? 1 : 0;
    test97_a4_err = (int)err4;
    test97_a4_cnt = (int)sent;

    recved = 777;
    co::error_e err5 = (co::error_e)1234;
    BOOL ok5 = co_await co::WSARecvMsg(INVALID_SOCKET, &msg, &recved, NULL, &err5);
    test97_a5_ok = ok5 ? 1 : 0;
    test97_a5_err = (int)err5;
    test97_a5_cnt = (int)recved;

    co_return 0;
}

static co::task_t test97_request_fails() {
    sockaddr_in addr = test97_loopback(28011);

    /* B1: the same wrapper as A1, but winsock is up, so load_win_fn succeeds and the failure
     * comes from the request itself. 0x1234, not INVALID_SOCKET, so the iocp associate step
     * never sees INVALID_HANDLE_VALUE. */
    co::error_e err = (co::error_e)1234;
    BOOL ok = co_await co::ConnectEx((SOCKET)0x1234, (const sockaddr *)&addr,
            (int)sizeof(addr), NULL, 0, NULL, &err);
    test97_b1_ok = ok ? 1 : 0;
    test97_b1_err = (int)err;

    /* B2: handle-based wrapper, invalid-but-nonnull handle */
    DWORD junk = 0;
    co::error_e err2 = (co::error_e)1234;
    BOOL ok2 = co_await co::DeviceIoControl((HANDLE)0x1234, 0, NULL, 0, NULL, 0, &junk, &err2);
    test97_b2_ok = ok2 ? 1 : 0;
    test97_b2_err = (int)err2;

    /* B3: co::read on the same invalid handle must surface a real error, never the EOF-shaped 0 */
    char buff[16];
    test97_b3_read = (int)co_await co::read((HANDLE)0x1234, buff, sizeof(buff), NULL);

    co_return 0;
}

int test97_win_err_unset() {
    /* phase A first: load_win_fn caches its successes in globals, so the winsock-down probes
     * must run before winsock is ever started in this process */
    {
        auto pool = co::create_pool();
        pool->sched(test97_early_fails());
        ASSERT_FN(pool->run());
    }
    DBG("A1 ConnectEx: ok=%d err=%d", test97_a1_ok, test97_a1_err);
    ASSERT_FN(CHK_BOOL(test97_a1_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a1_err == (int)co::ERROR_GENERIC));
    DBG("A2 co::connect's translation: %d", test97_a2_translated);
    ASSERT_FN(CHK_BOOL(test97_a2_translated != (int)co::ERROR_OK));
    DBG("A3 AcceptEx: ok=%d err=%d count=%d", test97_a3_ok, test97_a3_err, test97_a3_cnt);
    ASSERT_FN(CHK_BOOL(test97_a3_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a3_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a3_cnt == 0));
    DBG("A4 WSASendMsg: ok=%d err=%d count=%d", test97_a4_ok, test97_a4_err, test97_a4_cnt);
    ASSERT_FN(CHK_BOOL(test97_a4_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a4_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a4_cnt == 0));
    DBG("A5 WSARecvMsg: ok=%d err=%d count=%d", test97_a5_ok, test97_a5_err, test97_a5_cnt);
    ASSERT_FN(CHK_BOOL(test97_a5_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_a5_err == (int)co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test97_a5_cnt == 0));

    /* phase B: winsock up, the request-level failure paths must still write *err */
    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    {
        auto pool = co::create_pool();
        pool->sched(test97_request_fails());
        ASSERT_FN(pool->run());
    }
    DBG("B1 ConnectEx(garbage socket): ok=%d err=%d", test97_b1_ok, test97_b1_err);
    ASSERT_FN(CHK_BOOL(test97_b1_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_b1_err == (int)co::ERROR_GENERIC));
    DBG("B2 DeviceIoControl(garbage handle): ok=%d err=%d", test97_b2_ok, test97_b2_err);
    ASSERT_FN(CHK_BOOL(test97_b2_ok == 0));
    ASSERT_FN(CHK_BOOL(test97_b2_err == (int)co::ERROR_GENERIC));
    DBG("B3 co::read(garbage handle): %d", test97_b3_read);
    ASSERT_FN(CHK_BOOL(test97_b3_read < 0));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test97_win_err_unset() {
    DBG("IOCP wrappers only: epoll/kqueue take their errors straight from the OS calls");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test97_win_err_unset();
    print_test_result("018-041-reproduced_win_err_unset.cpp", ret >= 0);
    return ret;
}
