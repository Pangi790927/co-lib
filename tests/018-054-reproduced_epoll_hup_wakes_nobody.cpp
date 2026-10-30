#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test110 - Reproduced Bugs: an EPOLLHUP or EPOLLERR delivered alone woke no waiter, run() ended
================================================================================================= */

/* The epoll io_pool_t::handle_ready() woke a waiter only when the events epoll delivered met the
waiter's own mask. epoll reports EPOLLERR and EPOLLHUP whether or not a waiter asked for them, and
on a pipe they are often the only bits: the read end of a pipe whose writer closed with nothing
buffered reports EPOLLHUP alone, the full write end of a pipe whose reader closed reports EPOLLERR
alone. co::read waits EPOLLIN and co::write waits EPOLLOUT, so nothing met the delivery: nothing
was queued, the fd stayed registered with its level-triggered condition, next_task_state() found an
empty ready queue, and run() returned RUN_OK with the coroutine still parked; every further run()
returned the same way at once (repro/13-epoll_busy_spin.md's spin). A program reading a child's
output over a pipe never saw its EOF.

Fixed in handle_ready() (fix #051): a delivery carrying EPOLLERR or EPOLLHUP wakes every waiter of
the fd, whatever it waited for, since both bits are conditions of the open file and not of one
direction; the syscall each waiter then runs reports the condition (a read of 0, an EPIPE, a
socket error). The two pipe phases pin the fix: the reader must come back with 0 and the writer
with -1 and EPIPE, and run() must return RUN_OK with each done. The socket control pins what always
worked: a stream peer's close delivers EPOLLIN with its hangup, so that reader woke before the fix
too. Reproduced 2026-10-29 (repro/fix/051/). */

#if COLIB_OS_LINUX

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>

static int     test110_done  = 0;    /* the io coroutine ran past its await                  */
static ssize_t test110_ret   = -999; /* what the await handed back                          */
static int     test110_errno = 0;    /* errno right after the await                         */

static co::task_t test110_reader(int fd) {
    char buff[16];
    test110_ret = co_await co::read(fd, buff, sizeof(buff));
    test110_errno = errno;
    test110_done++;
    co_return 0;
}

static co::task_t test110_writer(int fd) {
    test110_ret = co_await co::write(fd, "x", 1);
    test110_errno = errno;
    test110_done++;
    co_return 0;
}

/* fills a pipe through its write end with nonblocking writes until EAGAIN, so the kernel has no
EPOLLOUT to report on it */
static int test110_fill_pipe(int wfd) {
    int flags = fcntl(wfd, F_GETFL, 0);
    ASSERT_FN(flags);
    ASSERT_FN(fcntl(wfd, F_SETFL, flags | O_NONBLOCK));
    char chunk[4096];
    memset(chunk, 'f', sizeof(chunk));
    while (true) {
        ssize_t n = ::write(wfd, chunk, sizeof(chunk));
        if (n < 0 && errno == EAGAIN)
            break;
        ASSERT_FN(n);
    }
    return 0;
}

/* runs one io coroutine on its own pool; the pool dies here, before the fd is closed */
static int test110_run(co::task_t t) {
    test110_done = 0;
    test110_ret = -999;
    test110_errno = 0;
    auto pool = co::create_pool();
    pool->sched(t);
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    return 0;
}

/* the read end of a pipe whose writer closed with nothing buffered: EPOLLHUP alone */
static int test110_read_end_hup() {
    int fds[2];
    ASSERT_FN(pipe(fds));
    FnScope scope([&] { close(fds[0]); });
    close(fds[1]);  /* the writer is gone before the reader parks */
    ASSERT_FN(test110_run(test110_reader(fds[0])));
    ASSERT_FN(CHK_BOOL(test110_done == 1));  /* pre-fix 0: run() came back with it parked */
    ASSERT_FN(CHK_BOOL(test110_ret == 0));   /* the EOF */
    DBG("pipe read end, writer closed: read returned %zd, errno %d", test110_ret, test110_errno);
    return 0;
}

/* the full write end of a pipe whose reader closed: EPOLLERR alone */
static int test110_write_end_err() {
    int fds[2];
    ASSERT_FN(pipe(fds));
    FnScope scope([&] { close(fds[1]); });
    ASSERT_FN(test110_fill_pipe(fds[1]));
    close(fds[0]);  /* the reader is gone, the buffer stays full */
    ASSERT_FN(test110_run(test110_writer(fds[1])));
    ASSERT_FN(CHK_BOOL(test110_done == 1));  /* pre-fix 0: run() came back with it parked */
    ASSERT_FN(CHK_BOOL(test110_ret == -1 && test110_errno == EPIPE));
    DBG("pipe write end, reader closed: write returned %zd, errno %d", test110_ret, test110_errno);
    return 0;
}

/* control: a stream socket whose peer closed reports EPOLLIN with its hangup, so its reader woke
before the fix as well, and must still read its EOF */
static int test110_socket_eof_control() {
    int sv[2];
    ASSERT_FN(socketpair(AF_UNIX, SOCK_STREAM, 0, sv));
    FnScope scope([&] { close(sv[0]); });
    close(sv[1]);
    ASSERT_FN(test110_run(test110_reader(sv[0])));
    ASSERT_FN(CHK_BOOL(test110_done == 1));
    ASSERT_FN(CHK_BOOL(test110_ret == 0));
    DBG("socket, peer closed: read returned %zd (control)", test110_ret);
    return 0;
}

static int test110_all() {
    signal(SIGPIPE, SIG_IGN);  /* the EPIPE must come back as a value, not end the process */
    ASSERT_FN(test110_socket_eof_control());
    ASSERT_FN(test110_read_end_hup());
    ASSERT_FN(test110_write_end_err());
    return 0;
}

#else /* !COLIB_OS_LINUX */

static int test110_all() {
    /* the epoll io_pool_t this test drives is Linux-only */
    DBG("not supported on this OS");
    return 0;
}

#endif /* COLIB_OS_LINUX */

int main() {
    int ret = test110_all();
    print_test_result("018-054-reproduced_epoll_hup_wakes_nobody.cpp", ret >= 0);
    return ret;
}
