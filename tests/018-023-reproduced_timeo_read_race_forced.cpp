#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test74 - Reproduced Bugs: 018-015's race, with its order forced
================================================================================================= */

/* The usage that lost data: `while (true) co_await create_timeo(co::read(sock, buff, len), 1ms);`
with data arriving at random times. On IOCP the read is an overlapped ReadFile issued before the
wait, so the kernel can take the bytes out of the socket before the reader is resumed. If the timer
fires in that window and the timeout wins, the read consumed data and the user is told nothing
arrived. 018-015 hits the window by chance; this forces it, in one iteration:
1. The pool is blocked while two timers expire: first the helper's 1ms sleep, then the timeout's.
   The pool then takes both completions: the helper runs first, the timer after it. (The timeout is
   40ms, not 1ms: timers expiring on the same Windows tick, ~15.6ms, come in no fixed order, and
   only the order matters for the race.)
2. The helper sends one byte and blocks the pool again, so the kernel completes the pending read
   (the byte is out of the socket) while the reader is still waiting.
3. Then the timer kills the reader, whose read already finished.
The byte must come back from that same create_timeo, with ERROR_OK - not ERROR_TIMEO. */

#if COLIB_OS_WINDOWS

#include <thread>

static SOCKET test74_client = INVALID_SOCKET;
static bool test74_sent = false;
static int64_t test74_first_ret = -100;
static co::error_e test74_first_err = co::ERROR_GENERIC;
static int64_t test74_second_ret = -100;
static co::error_e test74_second_err = co::ERROR_GENERIC;
static bool test74_ok = false;

/* issues its read first thing: nothing else of it goes through the pool's io before the race */
static co::task_t test74_reader(SOCKET c) {
    auto pool = co_await co::get_pool();
    char buff[4096];
    auto [ret, err] = co_await co::create_timeo(co::read((HANDLE)c, buff, sizeof(buff)),
            pool, std::chrono::milliseconds(40));
    test74_first_ret = ret;
    test74_first_err = err;

    /* nothing else was sent: the next one times out, the byte isn't read twice */
    auto [ret2, err2] = co_await co::create_timeo(co::read((HANDLE)c, buff, sizeof(buff)),
            pool, std::chrono::milliseconds(40));
    test74_second_ret = ret2;
    test74_second_err = err2;

    test74_ok = true;
    co_return 0;
}

/* its timer is set before the timeout's and expires ticks earlier: it runs before the timer */
static co::task_t test74_helper() {
    co_await co::sleep_ms(1);
    test74_sent = send(test74_client, "x", 1, 0) == 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));    /* the read completes */
    co_return 0;
}

/* lets the reader issue its read and the timeout set its timer, then blocks the pool while both
timers expire */
static co::task_t test74_blocker() {
    for (int i = 0; i < 4; i++)
        co_await co::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    co_return 0;
}

int test74_timeo_read_race_forced() {
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

    /* connected and accepted before the pool runs */
    test74_client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_FN(CHK_BOOL(test74_client != INVALID_SOCKET));
    FnScope close_client([]{ closesocket(test74_client); });
    ASSERT_FN(CHK_BOOL(connect(test74_client, (sockaddr *)&addr, sizeof(addr)) == 0));
    SOCKET c = accept(srv, NULL, NULL);
    ASSERT_FN(CHK_BOOL(c != INVALID_SOCKET));
    FnScope close_c([c]{ closesocket(c); });
    BOOL nodelay = TRUE;
    setsockopt(test74_client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));

    auto pool = co::create_pool();
    pool->sched(test74_reader(c));
    pool->sched(test74_helper());
    pool->sched(test74_blocker());
    ASSERT_FN(pool->run());

    DBG("first: ret %lld err %d, second: ret %lld err %d", (long long)test74_first_ret,
            (int)test74_first_err, (long long)test74_second_ret, (int)test74_second_err);
    ASSERT_FN(CHK_BOOL(test74_ok));
    ASSERT_FN(CHK_BOOL(test74_sent));
    ASSERT_FN(CHK_BOOL(test74_first_err == co::ERROR_OK));      /* not ERROR_TIMEO */
    ASSERT_FN(CHK_BOOL(test74_first_ret == 1));                 /* the byte is delivered */
    ASSERT_FN(CHK_BOOL(test74_second_err == co::ERROR_TIMEO));  /* nothing more to read */
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test74_timeo_read_race_forced() {
    DBG("readiness-based backend: read() runs after the resume, nothing can be lost this way");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test74_timeo_read_race_forced();
    print_test_result("018-023-reproduced_timeo_read_race_forced.cpp", ret >= 0);
    return ret;
}
