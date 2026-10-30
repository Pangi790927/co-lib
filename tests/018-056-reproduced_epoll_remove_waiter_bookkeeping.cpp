/* the waiter frames must come from malloc so AddressSanitizer can see the stale table entry this
test leaves: colib's bucket allocator keeps a freed frame intact inside its slab and hides it */
#define COLIB_DISABLE_ALLOCATOR true
#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test112 - Reproduced Bugs: a failed epoll_ctl left remove_waiter()'s tables half done
================================================================================================= */

/* The epoll io_pool_t::remove_waiter() returned on an epoll_ctl failure before finishing its own
bookkeeping. Its full-removal branch (the fd keeps no other events) had dropped the fd's data and
then returned on a failed EPOLL_CTL_DEL before ret_evs.pop_back(): the fd was gone from epoll and
from the tables, yet ret_evs kept a slot, so handle_ready() no longer took its "nothing to wait
on" exit and, once every coroutine was done, next_task_state() blocked in epoll_wait(-1) on an
empty set: run() never returned. Its partial branch (the fd keeps other events) returned on a
failed EPOLL_CTL_MOD before erasing the woken waiters from fd_data_t::waiters: the woken waiter
resumed, finished and was freed, and io_pool_t::clear() later walked the stale entry, a
heap-use-after-free in close_wait(). Both failures come from an fd closed under the pool (EBADF),
the order the docs warn against (stop_fd before close); the library answered the obvious recovery
with a hang, and a partial stop with a dangling pointer at teardown.

Fixed in remove_waiter() (fix #052): the bookkeeping runs whatever epoll_ctl answered, the waiters
are erased and the ret_evs slot is popped, and the failure is reported afterwards. The use-after-
free phase runs first: a reader (EPOLLIN) and a writer (EPOLLOUT, the socket's send buffer filled
first) park on one socket, a third coroutine closes the socket, stops EPOLLIN alone, lets the
woken reader finish and force-stops the pool; the teardown must then end the writer exactly once
and read no freed frame (the ASan build is this phase's gate; the plain build runs the same UB
unseen). The hang phase: a reader parks on a pipe, a second coroutine closes the pipe's read end
and awaits stop_fd on it; run() must come back RUN_OK with the reader woken by ERROR_WAKEUP, where
the unfixed header blocks forever (the watchdog ends the process). Reproduced 2026-10-29
(repro/fix/052/). */

#include <atomic>

static std::atomic<const char *> test112_wd_state{"startup"};

static void test112_watchdog_main() {
    for (int i = 0; i < 150; i++)   /* 15 s in 100 ms slices */
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    printf("018-056-reproduced_epoll_remove_waiter_bookkeeping.cpp: TIMED OUT after 15s while "
           "'%s' - run() did not come back\n", test112_wd_state.load());
    fflush(stdout);     /* _Exit drops what printf buffered when stdout is a pipe */
    std::_Exit(1);
}

#if COLIB_OS_LINUX

#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

static int     test112_reader_done = 0;    /* the reader ran past its read                    */
static ssize_t test112_reader_ret  = -999; /* what the read handed back: the stop's WAKEUP    */
static int     test112_writer_done = 0;    /* the writer ran past its write: never            */
static int     test112_writer_dtor = 0;    /* the writer's frame destroyed: once, at teardown */
static int     test112_stop_ret    = -999; /* what the stop coroutine handed back             */

struct test112_marker_t { int *cnt; ~test112_marker_t() { (*cnt)++; } };

static co::task_t test112_reader(int fd) {
    char buff[16];
    test112_reader_ret = co_await co::read(fd, buff, sizeof(buff));
    test112_reader_done++;
    co_return 0;
}

static co::task_t test112_writer(int fd) {
    test112_marker_t m{&test112_writer_dtor};
    co_await co::write(fd, "x", 1);
    test112_writer_done++;   /* nothing can wake it: the teardown destroys it parked */
    co_return 0;
}

/* the hang: the read end goes away under the pool, then the obvious recovery */
static co::task_t test112_close_then_stop_fd(int fd) {
    close(fd);
    test112_stop_ret = co_await co::stop_fd(fd);
    co_return 0;
}

/* the use after free: the socket goes away under the pool, then a stop of EPOLLIN alone, so the
fd keeps its EPOLLOUT waiter and remove_waiter() takes its partial branch */
static co::task_t test112_close_then_stop_in(int fd) {
    close(fd);
    test112_stop_ret = co_await co::stop_io(co::io_desc_t{ .fd = fd, .events = EPOLLIN });
    co_await co::yield();        /* the woken reader runs to its end and frees its frame */
    co_await co::force_stop(0);  /* the writer is parked for good: run() is left by force */
    co_return 0;
}

/* fills a socket's send buffer with nonblocking writes until EAGAIN, so the kernel has no
EPOLLOUT to report on it and a writer parks */
static int test112_fill_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    ASSERT_FN(flags);
    ASSERT_FN(fcntl(fd, F_SETFL, flags | O_NONBLOCK));
    char chunk[4096];
    memset(chunk, 'f', sizeof(chunk));
    while (true) {
        ssize_t n = ::write(fd, chunk, sizeof(chunk));
        if (n < 0 && errno == EAGAIN)
            break;
        ASSERT_FN(n);
    }
    return 0;
}

static void test112_reset() {
    test112_reader_done = 0;
    test112_reader_ret = -999;
    test112_writer_done = 0;
    test112_writer_dtor = 0;
    test112_stop_ret = -999;
}

static int test112_uaf_phase() {
    int sv[2];
    ASSERT_FN(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    FnScope scope([&] { close(sv[1]); });   /* sv[0] is closed by the stopping coroutine */
    ASSERT_FN(test112_fill_socket(sv[0]));
    test112_reset();
    {
        auto pool = co::create_pool();
        pool->sched(test112_reader(sv[0]));
        pool->sched(test112_writer(sv[0]));
        pool->sched(test112_close_then_stop_in(sv[0]));
        test112_wd_state = "use-after-free phase: run()";
        ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_STOPPED));
        ASSERT_FN(CHK_BOOL(test112_reader_done == 1 && test112_reader_ret == co::ERROR_WAKEUP));
        ASSERT_FN(CHK_BOOL(test112_stop_ret == co::ERROR_GENERIC));  /* the MOD failed: EBADF */
        ASSERT_FN(CHK_BOOL(test112_writer_done == 0 && test112_writer_dtor == 0));
        test112_wd_state = "use-after-free phase: teardown";
    }   /* ~pool_internal_t -> clear() walks the fd's waiters: pre-fix the freed reader is one */
    ASSERT_FN(CHK_BOOL(test112_writer_done == 0 && test112_writer_dtor == 1));
    DBG("partial stop after a close: reader woke with %zd, stop answered %d, the teardown ended "
            "the writer once", test112_reader_ret, test112_stop_ret);
    return 0;
}

static int test112_hang_phase() {
    int fds[2];
    ASSERT_FN(pipe(fds));
    FnScope scope([&] { close(fds[1]); });   /* fds[0] is closed by the stopping coroutine */
    test112_reset();
    auto pool = co::create_pool();
    pool->sched(test112_reader(fds[0]));
    pool->sched(test112_close_then_stop_fd(fds[0]));
    test112_wd_state = "hang phase: run()";
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));   /* pre-fix: blocks in epoll_wait(-1) */
    ASSERT_FN(CHK_BOOL(test112_reader_done == 1 && test112_reader_ret == co::ERROR_WAKEUP));
    ASSERT_FN(CHK_BOOL(test112_stop_ret == co::ERROR_GENERIC));  /* the DEL failed: EBADF */
    DBG("stop_fd after a close: run() came back RUN_OK, reader woke with %zd, stop answered %d",
            test112_reader_ret, test112_stop_ret);
    return 0;
}

static int test112_all() {
    ASSERT_FN(test112_uaf_phase());
    ASSERT_FN(test112_hang_phase());
    return 0;
}

#else /* !COLIB_OS_LINUX */

static int test112_all() {
    /* the epoll io_pool_t this test drives is Linux-only */
    DBG("not supported on this OS");
    return 0;
}

#endif /* COLIB_OS_LINUX */

int main() {
    std::thread(test112_watchdog_main).detach();
    int ret = test112_all();
    print_test_result("018-056-reproduced_epoll_remove_waiter_bookkeeping.cpp", ret >= 0);
    return ret;
}
