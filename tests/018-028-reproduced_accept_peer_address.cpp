#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test82 - Reproduced Bugs: co::accept on Windows answered its own address as the peer's
================================================================================================= */

/* co::accept must answer the address of whoever connected, as ::accept does, and a socket on which
getpeername works. On Windows it is built on AcceptEx, whose buffer holds the local address first
and the remote one after it, in a layout only GetAcceptExSockaddrs reads; co::accept copied the
start of it, the local address, and never set SO_UPDATE_ACCEPT_CONTEXT, without which getpeername
fails on the accepted socket. The client here connects from a port of its own, so the port the
accept answers says which of the two it was. Reproduced 28-09-2026. */

static int  test82_peer_port = -1;
static bool test82_ok = false;

#if COLIB_OS_WINDOWS

static int test82_getpeername_port = -1;

static co::task_t test82_acceptor(SOCKET srv) {
    sockaddr_in sa = {};
    uint32_t len = sizeof(sa);
    SOCKET c = co_await co::accept(srv, (sockaddr *)&sa, &len);
    if (c == INVALID_SOCKET)
        co_return -1;
    test82_peer_port = ntohs(sa.sin_port);
    sockaddr_in peer = {};
    int plen = sizeof(peer);
    if (getpeername(c, (sockaddr *)&peer, &plen) == 0)
        test82_getpeername_port = ntohs(peer.sin_port);
    closesocket(c);
    test82_ok = true;
    co_return 0;
}

int test82_accept_peer_address() {
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

    /* the client connects before the accept is asked for: the listen queue holds it */
    SOCKET cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    FnScope close_cl([cl]{ closesocket(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));
    sockaddr_in own = {};
    len = sizeof(own);
    ASSERT_FN(CHK_BOOL(getsockname(cl, (sockaddr *)&own, &len) == 0));

    auto pool = co::create_pool();
    pool->sched(test82_acceptor(srv));
    ASSERT_FN(pool->run());

    DBG("listener port %d, client port %d, co::accept answered %d, getpeername %d",
            ntohs(addr.sin_port), ntohs(own.sin_port), test82_peer_port, test82_getpeername_port);
    ASSERT_FN(CHK_BOOL(test82_ok));
    ASSERT_FN(CHK_BOOL(test82_peer_port == ntohs(own.sin_port)));
    ASSERT_FN(CHK_BOOL(test82_getpeername_port == ntohs(own.sin_port)));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

static co::task_t test82_acceptor(int srv) {
    sockaddr_in sa = {};
    socklen_t len = sizeof(sa);
    int c = co_await co::accept(srv, (sockaddr *)&sa, &len);
    if (c < 0)
        co_return -1;
    test82_peer_port = ntohs(sa.sin_port);
    close(c);
    test82_ok = true;
    co_return 0;
}

int test82_accept_peer_address() {
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_FN(CHK_BOOL(srv >= 0));
    FnScope close_srv([srv]{ close(srv); });
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_FN(CHK_BOOL(bind(srv, (sockaddr *)&addr, sizeof(addr)) == 0));
    ASSERT_FN(CHK_BOOL(listen(srv, 1) == 0));
    ASSERT_FN(CHK_BOOL(fcntl(srv, F_SETFL, fcntl(srv, F_GETFL, 0) | O_NONBLOCK) == 0));
    socklen_t len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &len) == 0));

    int cl = socket(AF_INET, SOCK_STREAM, 0);
    FnScope close_cl([cl]{ close(cl); });
    ASSERT_FN(CHK_BOOL(connect(cl, (sockaddr *)&addr, sizeof(addr)) == 0));
    sockaddr_in own = {};
    len = sizeof(own);
    ASSERT_FN(CHK_BOOL(getsockname(cl, (sockaddr *)&own, &len) == 0));

    auto pool = co::create_pool();
    pool->sched(test82_acceptor(srv));
    ASSERT_FN(pool->run());

    DBG("listener port %d, client port %d, co::accept answered %d",
            ntohs(addr.sin_port), ntohs(own.sin_port), test82_peer_port);
    ASSERT_FN(CHK_BOOL(test82_ok));
    ASSERT_FN(CHK_BOOL(test82_peer_port == ntohs(own.sin_port)));
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test82_accept_peer_address();
    print_test_result("018-028-reproduced_accept_peer_address.cpp", ret >= 0);
    return ret;
}
