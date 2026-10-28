#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test95 - Reproduced Bugs: pool_t::run() returned RUN_OK after the io backend failed
================================================================================================= */

/* pool_internal_t::run() checked its helper's nullptr return with a literal 'return RUN_OK', but
 * next_task_state() has two nullptr returns with opposite meanings and pre-stores the truth of
 * each into the pool's ret_val: the "nothing left to do" exit stores RUN_OK, the
 * "io_pool.handle_ready() failed" exit stores RUN_ERRORED. run() discarded ret_val at that exit,
 * so a failed io backend was reported as a clean run - violating pool_t::run()'s documented
 * contract ("Returns RUN_OK if no error occurred during the run, else the return code") - and every
 * coroutine left suspended on io was abandoned unreported (repro/09-run_swallows_errored.md).
 *
 * The fix returns ret_val at that exit, so the value next_task_state() stored is what run()
 * reports. The trigger below forces a real io-backend failure through the public API:
 * pool_t::get_internal_handle() hands out the pool's iocp handle, and closing it makes the
 * blocking wait inside handle_ready() fail for real (ERROR_INVALID_HANDLE), through the same
 * branch any genuine IOCP failure takes. It is closed from inside the pool, once the read is
 * pending: closed before the read, its value is reused by the event the read creates, and the
 * wait then waits on that event instead of failing. The control phase proves in the same binary
 * that a healthy pool still completes its reader and returns RUN_OK. */

static std::atomic<const char *> test95_wd_state{"startup"};

static void test95_watchdog_main() {
    for (int i = 0; i < 150; i++)   /* 15 s in 100 ms slices */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("018-040-reproduced_run_swallows_errored.cpp: TIMED OUT after 15s while '%s' - "
           "run() did not come back\n", test95_wd_state.load());
    fflush(stdout);     /* _Exit drops what printf buffered when stdout is a pipe */
    std::_Exit(1);
}

#if COLIB_OS_WINDOWS

static int test95_ctrl_started = 0;   /* control reader began its body */
static int test95_ctrl_resumed = 0;   /* control reader ran past its read */
static int test95_bug_started = 0;    /* bug reader began its body */
static int test95_bug_resumed = 0;    /* bug reader ran past its read (never, by design) */
static int test95_port_closed = 0;    /* the closer coroutine closed the pool's port */

static co::task_t test95_reader(int *started, int *resumed, HANDLE sock) {
    (*started)++;
    char buff[64];
    long long n = (long long)co_await co::read(sock, buff, sizeof(buff));
    (*resumed)++;
    DBG("reader resumed with n=%lld", n);
    co_return 0;
}

struct test95_sock_pair_t {
    SOCKET lst;
    SOCKET cli;   /* main() holds this end */
    SOCKET srv;   /* handed to the reader coroutine */
};

/* blocking loopback pair setup: bind 127.0.0.1:0, listen, connect, accept */
static int test95_make_pair(test95_sock_pair_t *out) {
    out->lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    out->cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out->lst == INVALID_SOCKET || out->cli == INVALID_SOCKET)
        return -1;

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0;
    if (bind(out->lst, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;
    if (listen(out->lst, 1) != 0)
        return -1;

    int len = sizeof(addr);
    if (getsockname(out->lst, (struct sockaddr *)&addr, &len) != 0)
        return -1;
    if (connect(out->cli, (struct sockaddr *)&addr, sizeof(addr)) != 0)
        return -1;

    out->srv = accept(out->lst, NULL, NULL);
    if (out->srv == INVALID_SOCKET)
        return -1;
    return 0;
}

static void test95_close_pair(test95_sock_pair_t *p) {
    closesocket(p->cli);
    closesocket(p->srv);
    closesocket(p->lst);
}

/* a healthy pool: the peer sends, the reader completes, run() drains to RUN_OK */
static int test95_control() {
    test95_sock_pair_t p;
    ASSERT_FN(CHK_BOOL(test95_make_pair(&p) == 0));
    FnScope close_pair([&p] { test95_close_pair(&p); });

    char byte = 'x';
    ASSERT_FN(CHK_BOOL(send(p.cli, &byte, 1, 0) == 1));

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test95_reader(&test95_ctrl_started, &test95_ctrl_resumed,
            (HANDLE)p.srv)));

    test95_wd_state = "control: pool->run()";
    co::run_e r = pool->run();
    DBG("control run() = %d (%s)", (int)r, co::dbg_enum(r).c_str());

    ASSERT_FN(CHK_BOOL(r == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test95_ctrl_started == 1));
    ASSERT_FN(CHK_BOOL(test95_ctrl_resumed == 1));
    return 0;
}

/* runs after the reader issued its read: closes the pool's port through the documented accessor,
 * so the wait that follows finds it closed and not reused (a failed ASSERT_COFN here only logs:
 * main() checks test95_port_closed) */
static co::task_t test95_port_closer() {
    auto pool = co_await co::get_pool();
    intptr_t iocp_h = pool->get_internal_handle();
    ASSERT_COFN(CHK_BOOL(iocp_h != 0));
    ASSERT_COFN(CHK_BOOL(CloseHandle((HANDLE)iocp_h) != 0));
    test95_port_closed++;
    DBG("closed the pool iocp %p via get_internal_handle(); the pool's io backend is now dead",
            (void *)iocp_h);
    co_return 0;
}

/* the iocp closed through the documented accessor while a reader is suspended on a read that can
 * never complete: handle_ready() fails for real, so run() must report RUN_ERRORED, not RUN_OK */
static int test95_closed_iocp() {
    test95_sock_pair_t p;
    ASSERT_FN(CHK_BOOL(test95_make_pair(&p) == 0));
    FnScope close_pair([&p] { test95_close_pair(&p); });
    /* the peer (p.cli) never sends: the reader's read can never complete */

    auto pool = co::create_pool();
    pool->sched(COLIB_REGNAME(test95_reader(&test95_bug_started, &test95_bug_resumed,
            (HANDLE)p.srv)));
    pool->sched(COLIB_REGNAME(test95_port_closer()));   /* second: the read is pending by then */

    test95_wd_state = "bug: pool->run() (the wait on the dead port must fail fast, not hang)";
    co::run_e r = pool->run();
    DBG("bug run() = %d (%s), reader started=%d resumed=%d", (int)r, co::dbg_enum(r).c_str(),
            test95_bug_started, test95_bug_resumed);

    /* the contract promised an error return for a failed run; the reader staying suspended is the
     * error being reported, so it must not count as a clean drain */
    ASSERT_FN(CHK_BOOL(test95_port_closed == 1));
    ASSERT_FN(CHK_BOOL(r == co::RUN_ERRORED));
    ASSERT_FN(CHK_BOOL(test95_bug_started == 1));
    ASSERT_FN(CHK_BOOL(test95_bug_resumed == 0));
    return 0;
}

static int test95_run_swallows_errored() {
    WSADATA wsa_data;
    ASSERT_FN(CHK_BOOL(WSAStartup(MAKEWORD(2, 2), &wsa_data) == 0));
    FnScope wsa_cleanup([] { WSACleanup(); });

    ASSERT_FN(test95_control());
    ASSERT_FN(test95_closed_iocp());
    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test95_run_swallows_errored() {
    /* the swallow is platform-independent (it lives in pool_internal_t::run()), but this trigger
     * closes the pool's iocp, which only exists on Windows; the unix twin would close the epoll
     * or kqueue fd handed out by the same get_internal_handle() */
    DBG("test skipped on this platform");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    std::thread(test95_watchdog_main).detach();

    int ret = test95_run_swallows_errored();
    print_test_result("018-040-reproduced_run_swallows_errored.cpp", ret >= 0);
    return ret;
}
