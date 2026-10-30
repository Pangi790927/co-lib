#define COLIB_ENABLE_DEBUG_NAMES true

#include "../colib.h"
#include "tests_common.h"

/* Test111 - Reproduced Bugs: Linux co::connect answered a refused connect with the positive errno
================================================================================================= */

/* The Linux co::connect, once its EPOLLOUT wait ended, read the socket's SO_ERROR and, when it was
set, did `co_return result` with that raw errno: a refused connection came back as 111
(ECONNREFUSED), a positive value. The library's failures are negative (error_e), so a caller's
`ret < 0` read the failed connect as a success, 1, 2 and 3 would have read as ERROR_YIELDED,
ERROR_SUSPENDED and ERROR_FINISHED, and the log line printed the stale errno of the last syscall
instead of the result. The Windows co::connect answers a failure with the stopper's error or
ERROR_GENERIC, the reason in GetLastError(); the Linux side had no such face. Its immediate
failure (::connect itself failing, EINPROGRESS aside) also read errno only after two further
fcntl calls, which may leave their own.

Fixed in co::connect (fix #054): a failed connection answers ERROR_GENERIC with errno set to the
SO_ERROR value, the log names that value, and the errno of ::connect is saved right after the call
and restored for the caller on the immediate failure. The refused phase is the bug: a TCP connect
to a loopback port that was just bound, listened on and closed, so nobody listens there, must come
back ERROR_GENERIC with errno ECONNREFUSED (pre-fix: 111). The immediate phase pins the saved
errno: a unix socket connect to a path that does not exist fails inside ::connect with ENOENT and
must come back ERROR_GENERIC with that errno. Reproduced 2026-10-29 (repro/fix/054/,
repro/15-linux_connect_errno.md). */

#if COLIB_OS_LINUX

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int test111_done  = 0;    /* the connecting coroutine ran past its await          */
static int test111_ret   = -999; /* what co::connect handed back                        */
static int test111_errno = 0;    /* errno right after the await, before anything else   */

static co::task_t test111_connect(int fd, const sockaddr *sa, socklen_t len) {
    test111_ret = co_await co::connect(fd, (sockaddr *)sa, len);
    test111_errno = errno;
    test111_done++;
    co_return 0;
}

/* a loopback port nobody listens on: bound and listened on for a moment, then closed; the
kernel's ephemeral pick is handed back */
static int test111_free_port(uint16_t *port) {
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_FN(lfd);
    FnScope scope([lfd] { close(lfd); });
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    ASSERT_FN(bind(lfd, (sockaddr *)&addr, sizeof(addr)));
    ASSERT_FN(listen(lfd, 1));
    socklen_t alen = sizeof(addr);
    ASSERT_FN(getsockname(lfd, (sockaddr *)&addr, &alen));
    *port = ntohs(addr.sin_port);
    return 0;
}

/* one connect on its own pool */
static int test111_run(int fd, const sockaddr *sa, socklen_t len) {
    test111_done = 0;
    test111_ret = -999;
    test111_errno = 0;
    auto pool = co::create_pool();
    pool->sched(test111_connect(fd, sa, len));
    ASSERT_FN(CHK_BOOL(pool->run() == co::RUN_OK));
    ASSERT_FN(CHK_BOOL(test111_done == 1));
    return 0;
}

/* the asynchronous failure: a TCP connect to a loopback port nobody listens on ends through the
EPOLLOUT wait with SO_ERROR == ECONNREFUSED */
static int test111_refused() {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint16_t port = 0;
        ASSERT_FN(test111_free_port(&port));
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_FN(fd);
        FnScope scope([fd] { close(fd); });
        sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        ASSERT_FN(test111_run(fd, (sockaddr *)&addr, sizeof(addr)));
        if (test111_ret == 0) {
            DBG("port %u was taken in between, trying another", (unsigned)port);
            continue;
        }
        DBG("refused connect to port %u: co::connect answered %d, errno %d (%s)",
                (unsigned)port, test111_ret, test111_errno, strerror(test111_errno));
        ASSERT_FN(CHK_BOOL(test111_ret == co::ERROR_GENERIC));  /* pre-fix: 111 */
        ASSERT_FN(CHK_BOOL(test111_errno == ECONNREFUSED));
        return 0;
    }
    DBG("three ephemeral ports were taken in between: nothing to judge");
    return -1;
}

/* the immediate failure: a unix socket connect to a path that does not exist fails inside
::connect, before any wait, with ENOENT */
static int test111_immediate() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    ASSERT_FN(fd);
    FnScope scope([fd] { close(fd); });
    sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/nonexistent-dir-018-055/sock", sizeof(addr.sun_path) - 1);
    ASSERT_FN(test111_run(fd, (sockaddr *)&addr, sizeof(addr)));
    DBG("connect to a missing unix path: co::connect answered %d, errno %d (%s)",
            test111_ret, test111_errno, strerror(test111_errno));
    ASSERT_FN(CHK_BOOL(test111_ret == co::ERROR_GENERIC));
    ASSERT_FN(CHK_BOOL(test111_errno == ENOENT));
    return 0;
}

static int test111_all() {
    ASSERT_FN(test111_refused());
    ASSERT_FN(test111_immediate());
    return 0;
}

#else /* !COLIB_OS_LINUX */

static int test111_all() {
    /* the epoll co::connect this test drives is Linux-only */
    DBG("not supported on this OS");
    return 0;
}

#endif /* COLIB_OS_LINUX */

/* Phase 3 (the amendment of 2026-10-30, report 6's item R6-2): connect() read the socket's flags
a second time after ::connect and gave up on that call's failure without restoring the flags it
had saved, leaving a blocking socket non-blocking. fcntl() is interposed for the process (the
executable's definition wins over libc's; the real one is reached through dlsym): the second
F_GETFL on the socket under test fails with EBADF. Before the amendment the connect answered
ERROR_GENERIC from that failure with O_NONBLOCK left set; after it there is no second read, the
connect runs to its refusal, and the flags are as they were. */
#if COLIB_OS_LINUX

#include <cstdarg>
#include <dlfcn.h>

static int test111_fault_fd = -1;
static int test111_getfl_calls = 0;

extern "C" int fcntl(int fd, int cmd, ...) {
    static auto real = (int (*)(int, int, ...))dlsym(RTLD_NEXT, "fcntl");
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);          /* F_GETFL passes none: the read is harmless here */
    va_end(ap);
    if (fd == test111_fault_fd && cmd == F_GETFL && ++test111_getfl_calls == 2) {
        errno = EBADF;
        return -1;
    }
    return real(fd, cmd, arg);
}

static int test111_flags_kept() {
    uint16_t port = 0;
    ASSERT_FN(test111_free_port(&port));
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_FN(CHK_BOOL(fd >= 0));                   /* blocking, as made */
    sockaddr_in sa = {};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);
    test111_fault_fd = fd;
    test111_getfl_calls = 0;
    ASSERT_FN(test111_run(fd, (const sockaddr *)&sa, sizeof(sa)));
    test111_fault_fd = -1;
    int flags = fcntl(fd, F_GETFL, 0);
    close(fd);
    DBG("flags kept: ret %d errno %d, F_GETFL calls %d, O_NONBLOCK after %d", test111_ret,
            test111_errno, test111_getfl_calls, (int)((flags & O_NONBLOCK) != 0));
    ASSERT_FN(CHK_BOOL(flags >= 0 && (flags & O_NONBLOCK) == 0));   /* as it was */
    ASSERT_FN(CHK_BOOL(test111_ret == co::ERROR_GENERIC && test111_errno == ECONNREFUSED));
    return 0;
}

#else /* !COLIB_OS_LINUX */

static int test111_flags_kept() { return 0; }

#endif /* COLIB_OS_LINUX */

int main() {
    int ret = test111_all();
    if (ret == 0)
        ret = test111_flags_kept();
    print_test_result("018-055-reproduced_linux_connect_errno.cpp", ret >= 0);
    return ret;
}
