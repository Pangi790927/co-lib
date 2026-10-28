#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test96 - IO: wait_msg
=================================================================================================
wait_msg() waits until the pool's thread has window messages and leaves them in the queue. Checked:
the port behavior it relies on (Microsoft does not document it), a message already queued, a message
posted from another thread while the pool waits, a timer and a pipe read completing while a
coroutine waits in wait_msg(), and a wait_msg() stopped by create_timeo(). */

#if COLIB_OS_WINDOWS

static int test96_done = 0;     /* the cases that finished their checks */
static int test96_read_done = 0;

/* The port's handle reads as signaled while it holds packets, and waiting on it takes none of
them: handle_ready_msg() relies on both. */
int test96_port_waitable() {
    HANDLE iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    ASSERT_FN(CHK_BOOL(iocp != NULL));
    FnScope scope([iocp] { CloseHandle(iocp); });

    OVERLAPPED_ENTRY entries[4];
    ULONG cnt = 0;

    ASSERT_FN(CHK_BOOL(WaitForSingleObject(iocp, 0) == WAIT_TIMEOUT));
    ASSERT_FN(CHK_BOOL(PostQueuedCompletionStatus(iocp, 1, 1, NULL)));
    ASSERT_FN(CHK_BOOL(WaitForSingleObject(iocp, 0) == WAIT_OBJECT_0));
    ASSERT_FN(CHK_BOOL(WaitForSingleObject(iocp, 0) == WAIT_OBJECT_0));
    ASSERT_FN(CHK_BOOL(GetQueuedCompletionStatusEx(iocp, entries, 4, &cnt, 0, FALSE) && cnt == 1));

    /* a packet posted while this thread waits on the port is still in it after the wait */
    std::thread poster([iocp] { Sleep(20); PostQueuedCompletionStatus(iocp, 2, 2, NULL); });
    DWORD ret = MsgWaitForMultipleObjectsEx(1, &iocp, 2000, QS_ALLINPUT,
            MWMO_ALERTABLE | MWMO_INPUTAVAILABLE);
    poster.join();
    ASSERT_FN(CHK_BOOL(ret == WAIT_OBJECT_0));
    cnt = 0;
    ASSERT_FN(CHK_BOOL(GetQueuedCompletionStatusEx(iocp, entries, 4, &cnt, 0, FALSE) && cnt == 1));
    return 0;
}

/* a message already in the queue: wait_msg() returns at once and leaves it there */
co::task_t test96_co_queued() {
    ASSERT_COFN(CHK_BOOL(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 1, 0, 0)));
    ASSERT_COFN(co_await co::wait_msg());

    MSG msg;
    ASSERT_COFN(CHK_BOOL(PeekMessageW(&msg, NULL, WM_APP + 1, WM_APP + 1, PM_REMOVE)));
    test96_done++;
    co_return 0;
}

/* a message posted by another thread while the pool waits in wait_msg() */
co::task_t test96_co_posted_later() {
    DWORD tid = GetCurrentThreadId();
    std::thread poster([tid] { Sleep(50); PostThreadMessageW(tid, WM_APP + 2, 0, 0); });
    FnScope join_scope([&poster] { poster.join(); });

    ASSERT_COFN(co_await co::wait_msg());

    MSG msg;
    ASSERT_COFN(CHK_BOOL(PeekMessageW(&msg, NULL, WM_APP + 2, WM_APP + 2, PM_REMOVE)));
    test96_done++;
    co_return 0;
}

/* the client of the pipe: sleeps first, so its timer fires while test96_co_waiter() waits */
co::task_t test96_co_writer(std::string pipe_name) {
    ASSERT_COFN(co_await co::sleep_ms(30));

    HANDLE client = CreateFileA(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    ASSERT_COFN(CHK_HANDLE(client));
    FnScope scope([client] { CloseHandle(client); });

    DWORD sent = 0;
    ASSERT_COFN(CHK_BOOL(co_await co::WriteFile(client, "ping", 4, &sent, NULL)));
    ASSERT_COFN(CHK_BOOL(sent == 4));
    co_return 0;
}

/* the server of the pipe: its read completes while test96_co_waiter() waits, then it posts the
message that wakes the waiter */
co::task_t test96_co_reader(std::string pipe_name) {
    HANDLE pipe = CreateNamedPipeA(pipe_name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE, 1, 64, 64, 0, NULL);
    ASSERT_COFN(CHK_HANDLE(pipe));
    FnScope scope([pipe] { CloseHandle(pipe); });

    co_await co::sched(test96_co_writer(pipe_name));
    ASSERT_COFN(CHK_BOOL(co_await co::ConnectNamedPipe(pipe)));

    char buff[8] = {0};
    DWORD recved = 0;
    ASSERT_COFN(CHK_BOOL(co_await co::ReadFile(pipe, buff, 4, &recved, NULL)));
    ASSERT_COFN(CHK_BOOL(recved == 4 && memcmp(buff, "ping", 4) == 0));

    test96_read_done++;
    ASSERT_COFN(CHK_BOOL(PostThreadMessageW(GetCurrentThreadId(), WM_APP + 3, 0, 0)));
    co_return 0;
}

/* waits in wait_msg() while the timer and the pipe do their work; woken by the reader's message */
co::task_t test96_co_waiter() {
    ASSERT_COFN(co_await co::wait_msg());
    ASSERT_COFN(CHK_BOOL(test96_read_done == 1));

    MSG msg;
    ASSERT_COFN(CHK_BOOL(PeekMessageW(&msg, NULL, WM_APP + 3, WM_APP + 3, PM_REMOVE)));
    test96_done++;
    co_return 0;
}

/* no message comes: create_timeo() stops the wait with its timeout */
co::task_t test96_co_stopped() {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {}

    auto pool = co_await co::get_pool();
    auto [ret, err] = co_await co::create_timeo(co::wait_msg(), pool,
            std::chrono::milliseconds(50));
    ASSERT_COFN(CHK_BOOL(err == co::ERROR_TIMEO));
    test96_done++;
    co_return 0;
}

int test96_wait_msg() {
    MSG msg;
    PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE);    /* gives this thread its message queue */

    ASSERT_FN(test96_port_waitable());
    {
        auto pool = co::create_pool();
        pool->sched(COLIB_REGNAME(test96_co_queued()));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    {
        auto pool = co::create_pool();
        pool->sched(COLIB_REGNAME(test96_co_posted_later()));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    {
        auto pool = co::create_pool();
        pool->sched(COLIB_REGNAME(test96_co_waiter()));
        pool->sched(COLIB_REGNAME(test96_co_reader("\\\\.\\pipe\\colib_test96_wait_msg")));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    {
        auto pool = co::create_pool();
        pool->sched(COLIB_REGNAME(test96_co_stopped()));
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    }
    ASSERT_FN(CHK_BOOL(test96_done == 4));
    return 0;
}

#else

int test96_wait_msg() {
    DBG("wait_msg: not supported on this OS");
    return 0;
}

#endif

int main() {
    int ret = test96_wait_msg();
    print_test_result("005-006-io_wait_msg.cpp", ret >= 0);
    return ret;
}
