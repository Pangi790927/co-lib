#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test21 - IO: stop_handle
================================================================================================= */

#if COLIB_OS_WINDOWS

co::task_t test21_co_stop_handle_registered() {
    /* Test case 1: handle was given to the coro engine */
    HANDLE hPipe;
    
    /* Create a named pipe for testing */
    const char* pipeName = "\\.\\pipe\\TestPipeStopHandle";
    hPipe = CreateNamedPipeA(
        pipeName,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE,
        1,
        1024,
        1024,
        0,
        NULL
    );
    
    if (hPipe == INVALID_HANDLE_VALUE) {
        DBG("Failed to create pipe for stop_handle test");
        co_return 0; /* Skip this test if pipe creation fails */
    }
    FnScope scope([hPipe] { CloseHandle(hPipe); });
    
    /* Register the handle with the coro engine by scheduling a read */
    auto read_task = [](HANDLE h) -> co::task_t {
        char buf[1];
        DWORD bytesRead;
        co_await co::ReadFile(h, buf, sizeof(buf), &bytesRead, NULL);
        co_return 0;
    };
    
    /* Schedule the read but don't await it - just register the handle */
    co_await co::sched(read_task(hPipe));
    
    /* Now stop_handle should work since handle is registered */
    int stop_ret = co_await co::stop_handle(hPipe);
    ASSERT_COFN(CHK_BOOL(stop_ret == co::ERROR_OK || stop_ret == co::ERROR_WAKEUP));
    
    DBG("stop_handle: registered handle stopped successfully");
    co_return 0;
}

co::task_t test21_co_stop_handle_not_registered() {
    /* Test case 2: handle was NOT given to the coro engine */
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_COFN(CHK_BOOL(sock != INVALID_SOCKET));
    FnScope scope([sock] { closesocket(sock); });
    
    /* Don't register the handle - just call stop_handle directly */
    /* This should still work, just won't find anything to stop */
    int stop_ret = co_await co::stop_handle((HANDLE)sock);
    ASSERT_COFN(CHK_BOOL(stop_ret == co::ERROR_OK || stop_ret == co::ERROR_WAKEUP));
    
    DBG("stop_handle: not-registered handle stopped (nothing to do)");
    co_return 0;
}

#else

co::task_t test21_co_stop_handle_not_supported() {
    /* stop_handle is not supported on this OS */
    DBG("stop_handle: not supported on this OS");
    co_return 0;
}

#endif

int test21_stop_handle() {
    auto pool = co::create_pool();
    
#if COLIB_OS_WINDOWS
    pool->sched(test21_co_stop_handle_registered());
    pool->sched(test21_co_stop_handle_not_registered());
#else
    pool->sched(test21_co_stop_handle_not_supported());
#endif
    
    co::run_e ret = pool->run();
    ASSERT_FN(CHK_BOOL(ret == co::RUN_OK));
    return 0;
}

int main() {
    int ret = test21_stop_handle();
    print_test_result("005-004-io_stop_handle.cpp", ret >= 0);
    return ret;
}
