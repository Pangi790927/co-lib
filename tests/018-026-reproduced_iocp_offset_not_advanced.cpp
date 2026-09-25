#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test79 - Reproduced Bugs: on Windows a read or write with an offset doesn't move the offset
================================================================================================= */

/* Overlapped handles have no file pointer: colib's ReadFile()/WriteFile() (and co::read()/write())
read or write at *offset, and write the offset back when the request is done - but the offset they
write back is the one the request started at, not moved by the bytes transferred. A loop that
reads or writes a file in chunks with the same offset variable then reads the first chunk again
and again, or overwrites it, where the sync ::ReadFile/::WriteFile move their file pointer. Here:
two 4-byte reads of "abcdefgh" must give "abcd" then "efgh", and two 4-byte writes must leave
"wxyz1234" in the file, with the offset at 8 after each pair. Reproduced 2026-09-25. */

#if COLIB_OS_WINDOWS

static char test79_first[5] = {}, test79_second[5] = {};
static uint64_t test79_read_off = 99, test79_write_off = 99;
static bool test79_ok = false;

static co::task_t test79_rw(HANDLE rfile, HANDLE wfile) {
    uint64_t offset = 0;
    co_await co::read(rfile, test79_first, 4, &offset);
    co_await co::read(rfile, test79_second, 4, &offset);
    test79_read_off = offset;

    offset = 0;
    co_await co::write(wfile, "wxyz", 4, &offset);
    co_await co::write(wfile, "1234", 4, &offset);
    test79_write_off = offset;
    test79_ok = true;
    co_return 0;
}

int test79_offset_not_advanced() {
    char rpath[MAX_PATH], wpath[MAX_PATH];
    GetTempPathA(MAX_PATH, rpath);
    strcpy(wpath, rpath);
    strcat(rpath, "colib_test79_read.txt");
    strcat(wpath, "colib_test79_write.txt");

    HANDLE w = CreateFileA(rpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    ASSERT_FN(CHK_BOOL(w != INVALID_HANDLE_VALUE));
    DWORD n = 0;
    WriteFile(w, "abcdefgh", 8, &n, NULL);
    CloseHandle(w);

    HANDLE rfile = CreateFileA(rpath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
            NULL);
    HANDLE wfile = CreateFileA(wpath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_OVERLAPPED,
            NULL);
    ASSERT_FN(CHK_BOOL(rfile != INVALID_HANDLE_VALUE && wfile != INVALID_HANDLE_VALUE));

    {
        auto pool = co::create_pool();
        pool->sched(test79_rw(rfile, wfile));
        ASSERT_FN(pool->run());
    }
    CloseHandle(rfile);
    CloseHandle(wfile);

    char written[9] = {};
    HANDLE check = CreateFileA(wpath, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    ReadFile(check, written, 8, &n, NULL);
    CloseHandle(check);
    DeleteFileA(rpath);
    DeleteFileA(wpath);

    DBG("read \"%s\" then \"%s\", offset %llu; wrote \"%s\", offset %llu", test79_first,
            test79_second, (unsigned long long)test79_read_off, written,
            (unsigned long long)test79_write_off);
    ASSERT_FN(CHK_BOOL(test79_ok));
    ASSERT_FN(CHK_BOOL(strcmp(test79_first, "abcd") == 0));
    ASSERT_FN(CHK_BOOL(strcmp(test79_second, "efgh") == 0));
    ASSERT_FN(CHK_BOOL(test79_read_off == 8));
    ASSERT_FN(CHK_BOOL(strcmp(written, "wxyz1234") == 0));
    ASSERT_FN(CHK_BOOL(test79_write_off == 8));
    return 0;
}

#else /* COLIB_OS_WINDOWS */

int test79_offset_not_advanced() {
    DBG("Windows only: the Linux read()/write() use the fd's own position");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test79_offset_not_advanced();
    print_test_result("018-026-reproduced_iocp_offset_not_advanced.cpp", ret >= 0);
    return ret;
}
