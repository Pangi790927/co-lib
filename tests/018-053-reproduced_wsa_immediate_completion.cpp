#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test109 - Reproduced Bugs: the WSA wrappers read an immediate completion as a failure
================================================================================================= */

/* ::WSASend, ::WSARecv, ::WSASendTo, ::WSARecvFrom and the WSASendMsg/WSARecvMsg extensions return
an int: 0 when the operation completed at once (its completion packet still comes through the
port), SOCKET_ERROR with WSA_IO_PENDING when it is pending, SOCKET_ERROR with the reason when it
failed. The wrappers stored that int in a bool and tested it like ReadFile's BOOL, so the 0 of an
immediate completion read as "failed" with GetLastError() not ERROR_IO_PENDING: the request was
reported failed (ERROR_GENERIC, 0 bytes, "The operation completed successfully"), while its packet
was still on its way to the port. A send completes at once nearly always, a receive whenever the
data is already there. Nothing in the suite moved socket data through these wrappers, so nobody
saw it. Found 2026-10-29 while auditing the wrappers for fix #23; fix #49
(tests/fixes/FIX_49-wsa_immediate_completion.md): the wrappers read the int, SOCKET_ERROR without
WSA_IO_PENDING is the failure.

Three scenarios on loopback pairs, each pair and pool fresh (a socket keeps the first port it was
associated with): a WSASend (immediate), a WSARecv with the byte already there (immediate), and a
WSARecv issued before the byte is sent from another thread (pending). All three must succeed with
one byte. */

#include <chrono>
#include <thread>

#if COLIB_OS_WINDOWS

static BOOL test109_ok = FALSE;
static DWORD test109_bytes = 0;
static co::error_e test109_err = co::ERROR_OK;
static char test109_buf[8];

static co::task_t test109_sender(SOCKET s) {
    char x = 'x';
    WSABUF wb{ 1, &x };
    test109_ok = co_await co::WSASend(s, &wb, 1, &test109_bytes, 0, &test109_err);
    co_return 0;
}

static co::task_t test109_receiver(SOCKET s) {
    WSABUF wb{ sizeof(test109_buf), test109_buf };
    DWORD flags = 0;
    test109_ok = co_await co::WSARecv(s, &wb, 1, &test109_bytes, &flags, &test109_err);
    co_return 0;
}

/* a connected loopback pair: `c` the client's end, `srv` the server's */
static int test109_pair(SOCKET *c, SOCKET *srv) {
    SOCKET l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(l != INVALID_SOCKET));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(l, (sockaddr *)&a, sizeof(a)) == 0));
    ASSERT_FN(CHK_BOOL(listen(l, 1) == 0));
    int len = sizeof(a);
    ASSERT_FN(CHK_BOOL(getsockname(l, (sockaddr *)&a, &len) == 0));
    *c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(*c != INVALID_SOCKET));
    ASSERT_FN(CHK_BOOL(::connect(*c, (sockaddr *)&a, sizeof(a)) == 0));
    *srv = ::accept(l, NULL, NULL);
    ASSERT_FN(CHK_BOOL(*srv != INVALID_SOCKET));
    closesocket(l);
    return 0;
}

static int test109_one(int scenario) {
    test109_ok = FALSE;
    test109_bytes = 0;
    test109_err = co::ERROR_OK;
    SOCKET c, srv;
    ASSERT_FN(test109_pair(&c, &srv));

    auto pool = co::create_pool();
    if (scenario == 0) {                        /* the send: done at once */
        pool->sched(test109_sender(c));
    }
    else if (scenario == 1) {                   /* the receive, the byte already there: at once */
        ASSERT_FN(CHK_BOOL(send(c, "x", 1, 0) == 1));
        pool->sched(test109_receiver(srv));
    }
    else {                                      /* the receive first, the byte later: pending */
        pool->sched(test109_receiver(srv));
        std::thread([c] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            send(c, "x", 1, 0);
        }).detach();
    }
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    closesocket(c);
    closesocket(srv);

    DBG("scenario %d: ok %d, bytes %lu, err %d", scenario, (int)test109_ok, test109_bytes,
            (int)test109_err);
    ASSERT_FN(CHK_BOOL(test109_ok));
    ASSERT_FN(CHK_BOOL(test109_bytes == 1));
    ASSERT_FN(CHK_BOOL(test109_err == co::ERROR_OK));
    if (scenario != 0)
        ASSERT_FN(CHK_BOOL(test109_buf[0] == 'x'));
    return 0;
}

static int test109_wsa_immediate_completion() {
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        printf("WATCHDOG: a wrapper never completed\n");
        fflush(stdout);
        std::_Exit(42);
    }).detach();

    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));
    for (int scenario = 0; scenario < 3; scenario++)
        ASSERT_FN(test109_one(scenario));
    return 0;
}

#else /* !COLIB_OS_WINDOWS */

static int test109_wsa_immediate_completion() {
    DBG("not runnable here: the WSA wrappers are Windows only");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test109_wsa_immediate_completion();
    print_test_result("018-053-reproduced_wsa_immediate_completion.cpp", ret >= 0);
    return ret;
}
