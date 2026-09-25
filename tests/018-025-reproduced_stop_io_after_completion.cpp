#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test78 - Reproduced Bugs: stop_io() on a request whose completion was already taken queues its
coroutine a second time
================================================================================================= */

/* A user's own overlapped request (create_io_desc() + io_awaiter_t, the way colib's wrappers are
built) shares its io_desc_t with another coroutine, which stops it with pool_t::stop_io(). If the
request's completion was already taken from the IOCP queue (its coroutine is in the ready queue,
not resumed yet), the Windows force_awake() still finds it "completed during the cancel"
(CancelIoEx finds nothing, GetOverlappedResult says done) and delivers it again: the coroutine is
pushed to the ready queue a second time. The queue's guard against being in two places at once
terminates the program. The order is forced: the pool is blocked while the other coroutine's read
completes, then this one's, so the pool takes both completions together and the other coroutine
runs first. The request must be delivered once: its coroutine resumes once, with its byte, and the
stop finds nothing left to stop. Noticed while writing the killer redesign (REDESIGN_KILLER.md,
"Open points"), reproduced 2026-09-25. */

#if COLIB_OS_WINDOWS

#include <cstdlib>
#include <exception>
#include <thread>
#include <tuple>

static co::io_desc_t test78_desc;           /* A's request, shared with B */
static SOCKET test78_client_a = INVALID_SOCKET;
static SOCKET test78_client_b = INVALID_SOCKET;
static int test78_a_resumed = 0;
static DWORD test78_a_got = 0;
static co::error_e test78_stop_ret = co::ERROR_GENERIC;
static bool test78_in_stop = false;
static bool test78_b_done = false;

/* the request: an overlapped ReadFile built like colib's own wrappers */
static co::task_t test78_a(SOCKET s) {
    auto desc = co::create_io_desc(co_await co::get_pool());
    static char buff[16];
    DWORD nread = 0;
    auto params = std::tuple{(HANDLE)s, (LPVOID)buff, (DWORD)sizeof(buff), (LPDWORD)&nread,
            &desc.data->overlapped};
    using params_t = decltype(params);
    desc.data->overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    desc.data->h = desc.h = (HANDLE)s;
    desc.data->io_request = +[](void *ptr) -> co::error_e {
        BOOL ok = std::apply(::ReadFile, *(params_t *)ptr);
        return (!ok && GetLastError() != ERROR_IO_PENDING) ? co::ERROR_GENERIC : co::ERROR_OK;
    };
    desc.data->ptr = (void *)&params;
    test78_desc = desc;
    co::error_e ret = co_await co::io_awaiter_t(desc);
    test78_a_resumed++;
    co::handle_done_req(desc.data.get(), ret, &test78_a_got, nullptr);
    co_return 0;
}

/* its completion is taken first: it runs while A is queued, not resumed yet */
static co::task_t test78_b(SOCKET s, co::pool_t *pool) {
    char buff[16];
    co_await co::read((HANDLE)s, buff, sizeof(buff));
    test78_in_stop = true;
    test78_stop_ret = pool->stop_io(test78_desc);
    test78_in_stop = false;
    test78_b_done = true;
    co_return 0;
}

/* both reads are pending: B's byte, then A's, while the pool is blocked */
static co::task_t test78_blocker() {
    send(test78_client_b, "b", 1, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    send(test78_client_a, "a", 1, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    co_return 0;
}

static SOCKET test78_pair(SOCKET srv, sockaddr_in &addr, SOCKET *client) {
    *client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL nodelay = TRUE;
    setsockopt(*client, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    if (connect(*client, (sockaddr *)&addr, sizeof(addr)) != 0)
        return INVALID_SOCKET;
    return accept(srv, NULL, NULL);
}

int test78_stop_io_after_completion() {
    std::set_terminate([] {
        DBG("terminated%s: the coroutine was queued twice",
                test78_in_stop ? " inside stop_io()" : "");
        print_test_result("018-025-reproduced_stop_io_after_completion.cpp", false);
        std::_Exit(1);
    });

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
    ASSERT_FN(CHK_BOOL(listen(srv, 2) == 0));
    int addr_len = sizeof(addr);
    ASSERT_FN(CHK_BOOL(getsockname(srv, (sockaddr *)&addr, &addr_len) == 0));

    SOCKET sa = test78_pair(srv, addr, &test78_client_a);
    SOCKET sb = test78_pair(srv, addr, &test78_client_b);
    ASSERT_FN(CHK_BOOL(sa != INVALID_SOCKET && sb != INVALID_SOCKET));
    FnScope close_all([sa, sb] {
        closesocket(sa);
        closesocket(sb);
        closesocket(test78_client_a);
        closesocket(test78_client_b);
    });

    auto pool = co::create_pool();
    pool->sched(test78_a(sa));
    pool->sched(test78_b(sb, pool.get()));
    pool->sched(test78_blocker());
    ASSERT_FN(pool->run());

    DBG("A resumed %d time(s) with %lu byte(s), stop_io returned %d", test78_a_resumed,
            test78_a_got, (int)test78_stop_ret);
    ASSERT_FN(CHK_BOOL(test78_b_done));
    ASSERT_FN(CHK_BOOL(test78_a_resumed == 1));
    ASSERT_FN(CHK_BOOL(test78_a_got == 1));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test78_stop_io_after_completion() {
    DBG("IOCP only");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test78_stop_io_after_completion();
    print_test_result("018-025-reproduced_stop_io_after_completion.cpp", ret >= 0);
    return ret;
}
