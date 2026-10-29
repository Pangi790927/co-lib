#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test108 - Reproduced Bugs: a co::connect socket came out without its connect context
================================================================================================= */

/* MSDN lets a socket from ConnectEx behave as an ordinary connected one only once it carries
SO_UPDATE_CONNECT_CONTEXT, and colib set it nowhere: getpeername answered WSAENOTCONN on a socket
co::connect had connected, and so did the shutdown right after it, the very call tests/005-001's
client makes and never checked. The accepted side carries SO_UPDATE_ACCEPT_CONTEXT since the
accept rework (fix #38 made its failure fail the accept). Reproduced 28-09-2026,
repro/23-so_update_context.md; fix #23 (tests/fixes/FIX_23-so_update_contexts.md), in the shape
the user chose: co::connect, the Linux-like face, sets the context and fails the connect when
that fails; the colib::ConnectEx wrapper stays the same as the Windows call, whose caller sets
the context, as this file's second client does for itself. */

#include <chrono>
#include <cstdio>
#include <thread>

#if COLIB_OS_WINDOWS

static SOCKET test108_listener = INVALID_SOCKET;
static uint16_t test108_listen_port = 0;
static SOCKET test108_sock_a = INVALID_SOCKET;   /* the co::connect leg   */
static SOCKET test108_sock_b = INVALID_SOCKET;   /* the co::ConnectEx leg */

static int test108_accepts = 0;
static int test108_acc_peer_rc[2] = { -1, -1 };
static int test108_acc_sock_rc[2] = { -1, -1 };
static uint16_t test108_acc_peer_port[2] = { 0, 0 };

static int test108_a_connect_rc = -99;
static int test108_a_peer_rc = -1;
static uint16_t test108_a_peer_port = 0;
static uint16_t test108_a_own_port = 0;
static int test108_a_shutdown_rc = -99;

static int test108_b_peer_rc_bare = -99;        /* right after the wrapper: the Windows contract */
static int test108_b_peer_err_bare = 0;
static int test108_b_peer_rc = -1;              /* after the caller's own update */
static uint16_t test108_b_peer_port = 0;
static uint16_t test108_b_own_port = 0;
static int test108_b_shutdown_rc = -99;

/* the probe the update marks gate: the call's rc, and the port it answered on success */
static int test108_name_call(SOCKET s, bool peer, uint16_t *port) {
    sockaddr_in a = {};
    int len = sizeof(a);

    int rc = peer ? getpeername(s, (sockaddr *)&a, &len) : getsockname(s, (sockaddr *)&a, &len);
    *port = (rc == 0) ? ntohs(a.sin_port) : 0;
    return rc;
}

static co::task_t test108_client_a() {
    test108_sock_a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_COFN(CHK_BOOL(test108_sock_a != INVALID_SOCKET));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test108_listen_port);

    /* on Windows co::connect is bind + ConnectEx + the context update */
    test108_a_connect_rc = co_await co::connect(test108_sock_a, (sockaddr *)&addr, sizeof(addr));
    ASSERT_COFN(test108_a_connect_rc);

    test108_a_peer_rc = test108_name_call(test108_sock_a, true, &test108_a_peer_port);
    test108_name_call(test108_sock_a, false, &test108_a_own_port);
    test108_a_shutdown_rc = shutdown(test108_sock_a, SD_BOTH);
    co_return 0;
}

static co::task_t test108_client_b() {
    test108_sock_b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_COFN(CHK_BOOL(test108_sock_b != INVALID_SOCKET));

    /* ConnectEx requires the socket to be bound first; co::connect does it itself */
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    ASSERT_COFN(CHK_BOOL(bind(test108_sock_b, (sockaddr *)&any, sizeof(any)) == 0));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(test108_listen_port);

    BOOL ok = co_await co::ConnectEx(test108_sock_b, (sockaddr *)&addr, sizeof(addr), NULL, 0,
            NULL);
    ASSERT_COFN(CHK_BOOL(ok));

    /* the wrapper is the Windows call: the socket is connected but unmarked, as MSDN says */
    uint16_t unused = 0;
    test108_b_peer_rc_bare = test108_name_call(test108_sock_b, true, &unused);
    test108_b_peer_err_bare = WSAGetLastError();

    /* the caller's job, as with ::ConnectEx */
    ASSERT_COFN(CHK_BOOL(setsockopt(test108_sock_b, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0)
            == 0));
    test108_b_peer_rc = test108_name_call(test108_sock_b, true, &test108_b_peer_port);
    test108_name_call(test108_sock_b, false, &test108_b_own_port);
    test108_b_shutdown_rc = shutdown(test108_sock_b, SD_BOTH);
    co_return 0;
}

static co::task_t test108_server() {
    for (int i = 0; i < 2; i++) {
        sockaddr_in sa = {};
        uint32_t len = sizeof(sa);
        SOCKET c = co_await co::accept(test108_listener, (sockaddr *)&sa, &len);
        ASSERT_COFN(CHK_BOOL(c != INVALID_SOCKET));

        uint16_t own_port = 0;
        test108_acc_peer_rc[i] = test108_name_call(c, true, &test108_acc_peer_port[i]);
        test108_acc_sock_rc[i] = test108_name_call(c, false, &own_port);
        closesocket(c);
        test108_accepts++;
    }
    co_return 0;
}

static int test108_so_update_context() {
    /* the watchdog goes in before anything that could hang */
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        printf("WATCHDOG: still waiting for connect/accept\n");
        fflush(stdout);
        std::_Exit(42);
    }).detach();

    WSADATA wsa;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa) == 0));

    test108_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test108_listener != INVALID_SOCKET));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(test108_listener, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(test108_listener, 4) == 0));
    int len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(test108_listener, (sockaddr *)&addr, &len) == 0));
    test108_listen_port = ntohs(addr.sin_port);

    auto pool = co::create_pool();
    pool->sched(test108_server());
    pool->sched(test108_client_a());
    pool->sched(test108_client_b());
    co::run_e ret = pool->run();

    if (test108_sock_a != INVALID_SOCKET)
        closesocket(test108_sock_a);
    if (test108_sock_b != INVALID_SOCKET)
        closesocket(test108_sock_b);
    closesocket(test108_listener);

    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test108_accepts == 2));

    DBG("listen %u | co::connect rc %d, peer rc %d port %u, own port %u, shutdown rc %d | "
            "ConnectEx bare peer rc %d err %d, after the caller's update: peer rc %d port %u, "
            "own port %u, shutdown rc %d | accepted peer rc %d/%d port %u/%u, getsockname rc %d/%d",
            test108_listen_port, test108_a_connect_rc, test108_a_peer_rc, test108_a_peer_port,
            test108_a_own_port, test108_a_shutdown_rc, test108_b_peer_rc_bare,
            test108_b_peer_err_bare, test108_b_peer_rc, test108_b_peer_port, test108_b_own_port,
            test108_b_shutdown_rc, test108_acc_peer_rc[0], test108_acc_peer_rc[1],
            test108_acc_peer_port[0], test108_acc_peer_port[1], test108_acc_sock_rc[0],
            test108_acc_sock_rc[1]);

    /* co::connect: a socket that knows its peer and takes a shutdown */
    ASSERT_FN(CHK_BOOL(test108_a_connect_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_a_peer_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_a_peer_port == test108_listen_port));
    ASSERT_FN(CHK_BOOL(test108_a_shutdown_rc == 0));

    /* colib::ConnectEx: the Windows contract, unmarked until its caller marks it */
    ASSERT_FN(CHK_BOOL(test108_b_peer_rc_bare != 0 && test108_b_peer_err_bare == WSAENOTCONN));
    ASSERT_FN(CHK_BOOL(test108_b_peer_rc == 0));
    ASSERT_FN(CHK_BOOL(test108_b_peer_port == test108_listen_port));
    ASSERT_FN(CHK_BOOL(test108_b_shutdown_rc == 0));

    /* the accepted side: peer and local names both answer, and each peer is one of the clients */
    for (int i = 0; i < 2; i++) {
        ASSERT_FN(CHK_BOOL(test108_acc_peer_rc[i] == 0));
        ASSERT_FN(CHK_BOOL(test108_acc_sock_rc[i] == 0));
        bool known_client = test108_acc_peer_port[i] == test108_a_own_port
                || test108_acc_peer_port[i] == test108_b_own_port;
        ASSERT_FN(CHK_BOOL(known_client));
    }
    ASSERT_FN(CHK_BOOL(test108_acc_peer_port[0] != test108_acc_peer_port[1]));
    return 0;
}

#else /* !COLIB_OS_WINDOWS */

static int test108_so_update_context() {
    DBG("not runnable here: SO_UPDATE_CONNECT_CONTEXT is the ConnectEx Windows contract");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test108_so_update_context();
    print_test_result("018-052-reproduced_so_update_context.cpp", ret >= 0);
    return ret;
}
