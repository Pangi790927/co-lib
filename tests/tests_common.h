#ifndef TESTS_COMMON_H
#define TESTS_COMMON_H

#include "colib.h"

#include <string.h>
#include <stdexcept>
#include <thread>
#include <iostream>

/* Test common utilities
================================================================================================= */

#define DBG(fmt, ...) co::dbg(__FILE__, __func__, __LINE__, fmt, ##__VA_ARGS__)

#define CHK_BOOL(val) ((val) ? 0 : -1)
#define CHK_PTR(ptr) ((ptr) ? 0 : -1)

namespace co = colib;

struct FnScope {
    using fn_t = std::function<void(void)>;
    std::vector<fn_t> fns;
    bool done = false;

    FnScope(fn_t fn) {
        add(fn);
    }

    FnScope() {}

    ~FnScope() {
        call();
    }

    void operator() (fn_t fn) {
        add(fn);
    }

    void add(fn_t fn) {
        fns.push_back(fn);
    }

    void disable() {
        fns.clear();
    }

    void call() {
        for (auto &f : fns)
            f();
        fns.clear();
    }
};

/* Platform-specific assert macros */
#if COLIB_OS_LINUX || COLIB_OS_UNIX

#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>

/* Assert Coroutine Function */
#define ASSERT_COFN(fn_call) do { \
    int res = (int)(fn_call); \
    if (res < 0) { \
        DBG("Assert[%s] FAILED[ret: %d, errno: %d[%s]]", #fn_call, res, errno, strerror(errno)); \
        co_return res; \
    } \
} while (0)

/* Assert Function */
#define ASSERT_FN(fn_call) do { \
    int res = (int)(fn_call); \
    if (res < 0) { \
        DBG("Assert[%s] FAILED[ret: %d, errno: %d[%s]]", #fn_call, res, errno, strerror(errno)); \
        return res; \
    } \
} while (0)

#elif COLIB_OS_WINDOWS

#include <windows.h>
#include <winioctl.h>
#pragma comment(lib, "Ws2_32.lib")

#define ASSERT_FN(fn) \
do { \
    int res = (fn); \
    if (res < 0) { \
        LPVOID lpMsgBuf; \
        DWORD dw = GetLastError(); \
        if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | \
                FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), \
                (LPTSTR) &lpMsgBuf, 0, NULL) == 0) \
        { \
            DBG("Failed %s err_code: 0x%x", #fn, dw); \
        } \
        else { \
            DBG("Failed %s err_str: %s [code: 0x%x] ", #fn, (LPCTSTR)lpMsgBuf, dw); \
            LocalFree(lpMsgBuf); \
        } \
        return res; \
    } \
} while (0)

/* Assert Corutine Function */
#define ASSERT_COFN(fn) do { \
    int res = (fn); \
    if (res < 0) { \
        LPVOID lpMsgBuf; \
        DWORD dw = GetLastError(); \
        if (FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | \
                FORMAT_MESSAGE_IGNORE_INSERTS, NULL, dw, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), \
                (LPTSTR) &lpMsgBuf, 0, NULL) == 0) \
        { \
            DBG("Failed %s err_code: 0x%x", #fn, dw); \
        } \
        else { \
            DBG("Failed %s err_str: %s [code: 0x%x] ", #fn, (LPCTSTR)lpMsgBuf, dw); \
            LocalFree(lpMsgBuf); \
        } \
        co_return res; \
    } \
} while (0)

#endif

/* Test result output with colors */
inline void print_test_result(const char* filename, bool passed) {
    #if COLIB_OS_WINDOWS
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
    if (passed) {
        SetConsoleTextAttribute(hConsole, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        std::cout << "[PASSED]: " << filename << std::endl;
    } else {
        SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_INTENSITY);
        std::cout << "[FAILED]: " << filename << std::endl;
    }
    SetConsoleTextAttribute(hConsole, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
    #else
    if (passed) {
        std::cout << "\033[32m[PASSED]: " << filename << "\033[0m" << std::endl;
    } else {
        std::cout << "\033[31m[FAILED]: " << filename << "\033[0m" << std::endl;
    }
    #endif
}

#endif /* TESTS_COMMON_H */
