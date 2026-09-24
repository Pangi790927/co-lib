#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test46 - Reproduced Bugs: create_timeo(co::read(...)) drops bytes that were already read
================================================================================================= */

/* On IOCP co::read issues an overlapped ReadFile before it waits, so the kernel can complete it
(moving the bytes out of the socket, into the buffer) before the coroutine is resumed. When
create_timeo's timer fires in that window, its killer either destroys the already-queued reader
(the completion is dequeued, not yet resumed) or cancels a request that already finished and
ignores its result. Either way the caller gets ERROR_TIMEO and the bytes are gone. Found through
bbb_repo's ssh tunnel, which reads its client socket with a 5ms create_timeo in a loop; reproduced
standalone losing ~60% of the bytes. epoll/kqueue only wait for readiness and read() after the
resume, so they can't lose anything this way. 2026-09-23 */

#if COLIB_OS_WINDOWS

#include <atomic>
#include <random>

static const int test46_cnt = 1000;
static std::atomic<int> test46_sent{0};

static void test46_sender(uint16_t port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return;
    }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    std::mt19937 rng(1234);
    for (int i = 0; i < test46_cnt; i++) {
        char c = 'a' + i % 26;
        if (send(s, &c, 1, 0) != 1)
            break;
        test46_sent++;
        Sleep(rng() % 4);   /* 0..3ms: bytes keep landing around the 5ms read timeout */
    }
    Sleep(300);
    closesocket(s);
}

static co::task_t test46_reader(SOCKET srv, int *received) {
    auto pool = co_await co::get_pool();
    sockaddr_in peer = {};
    uint32_t len = sizeof(peer);
    SOCKET c = co_await co::accept(srv, (sockaddr *)&peer, &len);
    ASSERT_COFN(CHK_BOOL(c != INVALID_SOCKET));

    char buff[4096];
    while (true) {
        auto [ret, err] = co_await co::create_timeo(co::read((HANDLE)c, buff, sizeof(buff)),
                pool, std::chrono::microseconds(5000));
        if (err == co::ERROR_TIMEO)
            continue;
        if (err != co::ERROR_OK || ret <= 0)
            break;
        *received += (int)ret;
    }
    closesocket(c);
    co_return 0;
}

int test46_timeo_read_drops_bytes() {
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

    std::thread sender(test46_sender, ntohs(addr.sin_port));
    int received = 0;
    auto pool = co::create_pool();
    pool->sched(test46_reader(srv, &received));
    co::run_e ret = pool->run();
    sender.join();

    DBG("sent: %d received: %d", test46_sent.load(), received);
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test46_sent.load() == test46_cnt));
    ASSERT_FN(CHK_BOOL(received == test46_sent.load()));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test46_timeo_read_drops_bytes() {
    DBG("readiness-based backend: read() runs after the resume, nothing can be lost this way");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test46_timeo_read_drops_bytes();
    print_test_result("018-015-reproduced_timeo_read_drops_bytes.cpp", ret >= 0);
    return ret;
}
