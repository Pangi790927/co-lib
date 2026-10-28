#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdio>
#include <atomic>
#include <chrono>

/* Test101 - Reproduced Bugs: co::ConnectNamedPipe on an already connected pipe hung forever
================================================================================================= */

/* co::ConnectNamedPipe's io_request treated a FALSE return with GetLastError() ==
ERROR_PIPE_CONNECTED exactly like ERROR_IO_PENDING: it reported "the request pends" and the
awaiting coroutine suspended for an IOCP completion. ERROR_PIPE_CONNECTED means the client had
connected before the call, so the kernel completed the connect inline and queued no IRP - no
completion packet can ever arrive for that OVERLAPPED (the kernel probe below asserts exactly
that, with plain Win32), and pool->run() blocked forever in its wait. Reproduced 2026-09-28
(repro/12-connectnamedpipe_hang.md): a deterministic one-process hang, silent - the swallowed
branch had no log line of its own.

Fixed by letting io_request report "completed by itself" (ERROR_FINISHED, consumed only inside
add_waiter) for that branch; add_waiter then makes the waiter ready on the spot - a success with
no bytes transferred, since nothing was ever written to the overlapped - instead of enqueueing a
wait that cannot be satisfied. Before the fix this file hangs for 5 s and is killed by its own
watchdog; after it the coroutine completes on the first pass of run(). Fix #12
(tests/fixes/FIX_12-connectnamedpipe_connected.md). */

static std::atomic<bool> test101_done{false};

#if COLIB_OS_WINDOWS

static BOOL test101_conn_ok = FALSE;
static int  test101_conn_err = 12345;
static BOOL test101_write_ok = FALSE;
static int  test101_written = 0;

static co::task_t test101_server(HANDLE pipe) {
    co::error_e err = co::ERROR_GENERIC;
    test101_conn_ok = co_await co::ConnectNamedPipe(pipe, &err);
    test101_conn_err = (int)err;

    /* the success must be a real one: the server can write on the pipe right away, the
    connection is there */
    const char msg[] = "already connected";
    DWORD written = 0;
    test101_write_ok = co_await co::WriteFile(pipe, msg, sizeof(msg), &written, NULL);
    test101_written = (int)written;
    co_return 0;
}

static int test101_kernel_probe(void) {
    char name[128];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\colib-test101-%lu-probe",
            (unsigned long)GetCurrentProcessId());

    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE, 1, 1024, 1024, 0, NULL);
    ASSERT_FN(CHK_HANDLE(server));
    FnScope close_server([server]{ CloseHandle(server); });

    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(client));
    FnScope close_client([client]{ CloseHandle(client); });

    /* the client sits on the pipe instance before the server ever asks for a connection */
    OVERLAPPED ov = {};
    ov.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ASSERT_FN(CHK_PTR(ov.hEvent));
    FnScope close_event([&ov]{ CloseHandle(ov.hEvent); });

    SetLastError(0);
    BOOL ret = ConnectNamedPipe(server, &ov);
    DWORD win_err = GetLastError();
    ASSERT_FN(CHK_BOOL(ret == FALSE));
    ASSERT_FN(CHK_BOOL(win_err == ERROR_PIPE_CONNECTED));

    /* no IRP was queued for this overlapped, so nothing will ever signal its event: this is
    the fact that makes "wait for the completion" a forever wait */
    DWORD waited = WaitForSingleObject(ov.hEvent, 100);
    ASSERT_FN(CHK_BOOL(waited == WAIT_TIMEOUT));

    DWORD transferred = 0;
    DBG("probe: ret=%d err=%lu event_wait=%lu GetOverlappedResult=%d", (int)ret,
            (unsigned long)win_err, (unsigned long)waited,
            (int)GetOverlappedResult(server, &ov, &transferred, FALSE));

    return 0;
}

static int test101_already_connected(void) {
    ASSERT_FN(test101_kernel_probe());

    char name[128];
    std::snprintf(name, sizeof(name), "\\\\.\\pipe\\colib-test101-%lu",
            (unsigned long)GetCurrentProcessId());
    HANDLE server = CreateNamedPipeA(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE, 1, 1024, 1024, 0, NULL);
    ASSERT_FN(CHK_HANDLE(server));

    /* the client connects before the coroutine that awaits is even scheduled */
    HANDLE client = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, 0, NULL);
    ASSERT_FN(CHK_HANDLE(client));
    FnScope close_all([server, client]{ CloseHandle(client); CloseHandle(server); });

    auto pool = co::create_pool();
    pool->sched(test101_server(server));
    ASSERT_FN(pool->run());     /* before the fix this never returned */

    ASSERT_FN(CHK_BOOL(test101_conn_ok == TRUE));
    ASSERT_FN(CHK_BOOL(test101_conn_err == (int)co::ERROR_OK));

    /* the write inside the coroutine reached the client: the connection the await answered
    with is one that carries data */
    char buff[64] = {0};
    DWORD read_len = 0;
    ASSERT_FN(CHK_BOOL(ReadFile(client, buff, sizeof(buff) - 1, &read_len, NULL)));
    ASSERT_FN(CHK_BOOL(test101_write_ok == TRUE));
    ASSERT_FN(CHK_BOOL(test101_written == (int)read_len));

    return 0;
}

#else /* COLIB_OS_WINDOWS */

static int test101_already_connected(void) {
    DBG("named pipes are Windows-only: no ConnectNamedPipe to await elsewhere");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    /* This bug's failure mode is a hang inside pool->run(), so a plain ASSERT_FN can never
    fire on it and run_tests.py has no per-test timeout of its own: without a watchdog a
    regression would hang the whole suite instead of failing this one file. 018-012 guards its
    Linux-only hang with alarm()/SIGALRM; Windows has no alarm(), so a detached thread plays
    that role here. */
    std::thread watchdog([]{
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (test101_done)
            return;
        std::fputs("018-045-reproduced_connectnamedpipe_already_connected.cpp: TIMED OUT "
                "after 5s - co::ConnectNamedPipe never returned (the exact regression this "
                "test exists to catch)\n", stderr);
        std::fflush(stderr);
        std::_Exit(1);
    });
    watchdog.detach();

    int ret = test101_already_connected();
    test101_done = true;

    print_test_result("018-045-reproduced_connectnamedpipe_already_connected.cpp", ret >= 0);
    return ret;
}
