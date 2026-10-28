#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <atomic>
#include <cstdio>
#include <thread>

/* Test83 - Reproduced Bugs: a killed Windows io request leaked its event handle
================================================================================================= */

/* Every Windows io wrapper creates an event handle for its overlapped request before its
co_await and closed it only in handle_done_req(), which runs after the co_await resumes. A waiter
killed while suspended (a create_timeo timeout here) never runs that code: killer_state_t::kill()
-> stop_io() -> io_pool_t::force_awake() cancels the request and the io pool drops its
shared_ptr<io_data_t>, then end_chain() destroys the suspended frame - the last reference - and
io_data_t had no destructor, so the event handle leaked with the freed request. Exactly one kernel
handle per killed io op, deterministic (repro/06-winevent_handle_leak.md: 300 killed reads, +300
handles, two identical runs; the completing control loop flat, because there handle_done_req()
runs).

The fix gives io_data_t a destructor that closes overlapped.hEvent: the handle now dies with its
single owner on every exit, resumed or killed, and handle_done_req() clears the field after its
own close. The killed loop below must therefore stay as flat as the completing one; before the
fix this test failed with kill growth == test83_iters (one leaked handle per timed-out read). */

static std::atomic<const char *> test83_wd_state{"startup"};

static void test83_watchdog_main() {
    for (int i = 0; i < 150; i++)   /* 15 s in 100 ms slices */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("018-029-reproduced_winevent_handle_leak.cpp: TIMED OUT after 15s while '%s' - "
           "the kill loop stopped waking\n", test83_wd_state.load());
    std::_Exit(1);
}

#if COLIB_OS_WINDOWS

static const int test83_iters = 300;

static int test83_ctrl_done = 0;   /* control reads that returned their byte */
static int test83_kill_done = 0;   /* timed-out reads that came back */
static int test83_kill_timeo = 0;  /* ...with err == ERROR_TIMEO */

static int test83_handle_count(DWORD *cnt) {
    if (!GetProcessHandleCount(GetCurrentProcess(), cnt)) {
        DBG("GetProcessHandleCount: %lu", GetLastError());
        return -1;
    }
    return 0;
}

static co::task_t test83_read_completes(HANDLE rd) {
    char buff[64];
    auto ret = co_await co::read(rd, buff, sizeof(buff));
    if (ret == 1)
        test83_ctrl_done++;
    co_return 0;
}

static co::task_t test83_read_killed(HANDLE rd) {
    co::pool_t *pool = co_await co::get_pool();
    char buff[64];
    auto [ret, err] = co_await co::create_timeo(co::read(rd, buff, sizeof(buff)), pool,
            std::chrono::microseconds(2000));
    (void)ret;
    test83_kill_done++;
    if (err == co::ERROR_TIMEO)
        test83_kill_timeo++;
    co_return 0;
}

static int test83_winevent_handle_leak() {
    char pipe_name[128];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\test83_leak_%lu",
            GetCurrentProcessId());

    /* an overlapped server read-end; the client connects and never writes, so every read on rd
     * pends until the timeout kills it */
    HANDLE rd = CreateNamedPipeA(pipe_name,
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE,
            1, 1024, 1024, NMPWAIT_USE_DEFAULT_WAIT, NULL);
    ASSERT_FN(CHK_HANDLE(rd));
    HANDLE wr = CreateFileA(pipe_name, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(wr));
    FnScope close_pipes([rd, wr] { CloseHandle(rd); CloseHandle(wr); });

    auto pool = co::create_pool();

    /* control: the read completes, the wrapper resumes, handle_done_req() runs after the
     * co_await - this loop was already flat before the fix and must stay flat */
    test83_wd_state = "control loop";
    DWORD ctrl_before = 0, ctrl_after = 0;
    ASSERT_FN(test83_handle_count(&ctrl_before));
    for (int i = 0; i < test83_iters; i++) {
        char byte = 'x';
        DWORD written = 0;
        ASSERT_FN(CHK_BOOL(WriteFile(wr, &byte, 1, &written, NULL) && written == 1));
        pool->sched(COLIB_REGNAME(test83_read_completes(rd)));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    ASSERT_FN(test83_handle_count(&ctrl_after));

    /* kill: create_timeo kills the read while it is suspended; the wrapper frame is destroyed
     * without resuming, so only whoever frees the io request can still close its event */
    test83_wd_state = "kill loop";
    DWORD kill_before = 0, kill_after = 0;
    ASSERT_FN(test83_handle_count(&kill_before));
    for (int i = 0; i < test83_iters; i++) {
        pool->sched(COLIB_REGNAME(test83_read_killed(rd)));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    ASSERT_FN(test83_handle_count(&kill_after));

    long ctrl_growth = long(ctrl_after) - long(ctrl_before);
    long kill_growth = long(kill_after) - long(kill_before);

    DBG("control: handles %lu -> %lu (growth %ld), completed reads %d/%d",
            ctrl_before, ctrl_after, ctrl_growth, test83_ctrl_done, test83_iters);
    DBG("kill   : handles %lu -> %lu (growth %ld), timed-out reads %d/%d",
            kill_before, kill_after, kill_growth, test83_kill_timeo, test83_iters);

    /* every read must have gone the intended way, else the growth numbers say nothing */
    ASSERT_FN(CHK_BOOL(test83_ctrl_done == test83_iters));
    ASSERT_FN(CHK_BOOL(test83_kill_done == test83_iters));
    ASSERT_FN(CHK_BOOL(test83_kill_timeo == test83_iters));

    /* both loops stay flat: the event closes with the io request on every exit now */
    ASSERT_FN(CHK_BOOL(ctrl_growth <= test83_iters / 10));
    ASSERT_FN(CHK_BOOL(kill_growth <= test83_iters / 10));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test83_winevent_handle_leak() {
    /* the bug is Windows/IOCP-only (the event handle lives in the request's OVERLAPPED) */
    DBG("test skipped on this platform");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    std::thread(test83_watchdog_main).detach();

    int ret = test83_winevent_handle_leak();
    print_test_result("018-029-reproduced_winevent_handle_leak.cpp", ret >= 0);
    return ret;
}
