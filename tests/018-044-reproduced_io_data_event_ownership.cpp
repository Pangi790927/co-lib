#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <string>
#include <type_traits>

/* Test100 - Reproduced Bugs: io_data_t's event had one owner by convention, not by the type
================================================================================================= */

/* io_data_t closes overlapped.hEvent when it dies. Its copies were still allowed (and, with its
destructor declared, a move was a copy), so two io_data_t could end up closing the same handle;
and a close that failed in that destructor left no trace in the log. io_data_t is now built in
place and cannot be copied; there is no move either. The test checks that it cannot be copied and
that a failed close is logged. Found by review 10-10-2026; fix #40
(tests/fixes/FIX_40-io_data_event_ownership.md). */

#if COLIB_OS_WINDOWS

int test100_not_copyable() {
    DBG("copy-constructible %d, copy-assignable %d",
            (int)std::is_copy_constructible_v<co::io_data_t>,
            (int)std::is_copy_assignable_v<co::io_data_t>);
    ASSERT_FN(CHK_BOOL(!std::is_copy_constructible_v<co::io_data_t>));
    ASSERT_FN(CHK_BOOL(!std::is_copy_assignable_v<co::io_data_t>));
    return 0;
}

int test100_failed_close_logged() {
    /* no process holds a handle this high: the kernel's table stops at 2^24 entries of 4. Outside
    a debugger CloseHandle fails with ERROR_INVALID_HANDLE; under one, Windows raises
    STATUS_INVALID_HANDLE first and the debugger stops here once. */
    HANDLE bogus = (HANDLE)(uintptr_t)0x7ffffff0;
    std::string captured;
    auto old_log = co::log_str;
    co::log_str = [&captured](const co::dbg_string_t& s) {
        captured += s.c_str();
        return 0;
    };
    {
        co::io_data_t data;
        data.overlapped.hEvent = bogus;
    }
    co::log_str = old_log;

    DBG("captured log: [%s]", captured.c_str());
    ASSERT_FN(CHK_BOOL(captured.find("Failed to close event") != std::string::npos));
    return 0;
}

int test100_io_data_event_ownership() {
    int ret = 0;
    if (test100_not_copyable() < 0)
        ret = -1;
    if (test100_failed_close_logged() < 0)
        ret = -1;
    return ret;
}

#else /* COLIB_OS_WINDOWS */

int test100_io_data_event_ownership() {
    DBG("io_data_t's event is a Windows-only member: nothing to check here");
    return 0;
}

#endif /* COLIB_OS_WINDOWS */

int main() {
    int ret = test100_io_data_event_ownership();
    print_test_result("018-044-reproduced_io_data_event_ownership.cpp", ret >= 0);
    return ret;
}
