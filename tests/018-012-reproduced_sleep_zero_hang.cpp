#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

#include <cstdlib>
#if COLIB_OS_LINUX || COLIB_OS_UNIX
#include <unistd.h>
#include <csignal>
#endif

/* Test43 - Reproduced Bugs: sleep(0)/sleep_us(0) must not hang forever
================================================================================================= */

/* Was an open bug (BUGS.md #3): a 0-duration sleep() built an itimerspec with it_value = {0, 0} on
Linux and handed it straight to timerfd_settime(). Per timerfd_settime(2), setting it_value to zero
DISARMS the timer rather than firing it immediately - so the io_awaiter_t suspended waiting on a
timer fd that would never signal, and the coroutine never resumed. pool->run() hung forever.

Confirmed to genuinely hang (not just per the docs) by running this exact scenario against colib.h
before the fix: it printed "before" and then sat there until killed by an external timeout - it
never crashed, never asserted, just hung, which is why this test carries its own watchdog below
rather than relying on run_tests.py (which has no per-test timeout of its own) to ever notice.

The platform split was real: the Windows backend's SetWaitableTimer treats a 0 due-time as an
absolute FILETIME timestamp in the deep past (not a relative time - only negative values are
relative), so it fires immediately there. sleep(0) used to succeed instantly on Windows and hang
forever on Linux - same call, same semantics intended, opposite observed behavior depending on which
backend happened to compile.

Fixed by not routing a 0 duration through either platform's timer backend at all: sleep() now checks
timeo.count() == 0 and co_returns immediately, before ever calling get_timer()/set_timer(). This
doesn't pick a side of the two platform behaviors, it removes the platform dependency entirely - a
0-duration sleep is now defined by colib itself, uniformly, rather than by whatever a 0 due-time
happens to mean to the underlying OS timer API. */

bool test43_resumed = false;

co::task_t test43_co_sleep_zero() {
    co_await co::sleep_us(0);
    test43_resumed = true;
    co_return 0;
}

int test43_sleep_zero_hang() {
    auto pool = co::create_pool();
    pool->sched(test43_co_sleep_zero());

    ASSERT_FN(pool->run());
    ASSERT_FN(CHK_BOOL(test43_resumed == true));

    return 0;
}

#if COLIB_OS_LINUX || COLIB_OS_UNIX
static void test43_on_alarm(int) {
    /* async-signal-safe-ish effort: write() + _exit() rather than fprintf()/exit(), since this
    can fire while arbitrary library code is mid-execution */
    const char msg[] = "018-012-reproduced_sleep_zero_hang.cpp: TIMED OUT after 5s - "
            "sleep(0)/sleep_us(0) hung (the exact regression this test exists to catch)\n";
    ssize_t written = write(2, msg, sizeof(msg) - 1); (void)written;
    _exit(1);
}
#endif

int main() {
    /* This bug's failure mode is a literal hang, not a wrong value or a crash - so a plain
    ASSERT_FN can never fail on it, and run_tests.py has no per-test timeout of its own to notice a
    hung child process either. Without this watchdog, a regression here would hang the entire test
    suite indefinitely instead of failing this one file.

    POSIX alarm()/SIGALRM only, not a std::thread-based watchdog: the actual hang mechanism this
    test guards against (timerfd_settime(0) disarming instead of firing) is Linux/Unix-specific -
    the Windows backend already resolved a 0 due-time immediately even before the fix (SetWaitableTimer
    treats it as an absolute timestamp in the deep past), so there's no comparable hang mode to guard
    against there. Also sidesteps std::thread needing -lpthread, which linux.makefile's compile rule
    doesn't actually pass through (LINK_FLAGS is set but never referenced in the $(TEST_TARGETS)
    recipe - a separate, pre-existing makefile bug, not one to route around by fixing the makefile
    here). */
#if COLIB_OS_LINUX || COLIB_OS_UNIX
    signal(SIGALRM, test43_on_alarm);
    alarm(5);
#endif

    int ret = test43_sleep_zero_hang();

#if COLIB_OS_LINUX || COLIB_OS_UNIX
    alarm(0);
#endif

    print_test_result("018-012-reproduced_sleep_zero_hang.cpp", ret >= 0);
    return ret;
}
