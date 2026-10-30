# Proposal: the Linux co::connect answers a failed connection with ERROR_GENERIC (fix #054)

Status: [APPLIED 2026-10-30 on the user's word, with the amendment of the same day]

Apply verification, 2026-10-30: applied on the header with fix #051 in, as the main diff plus the
amendment's delta, regenerated as one diff of `repro/fix/054/base/colib.h` against the amended
`repro/fix/054/colib.h` (`repro/fix/054/054-fix-amended.diff`; the proposal's `054-fix.diff`
predates the amendment); it applied at fuzz 0 with a five-line offset from #051, and the applied
header differs from the amended copy by #051's hunk alone. The test landed as
`tests/018-055-reproduced_linux_connect_errno.cpp` (Test111), the repro's three-phase file
renumbered, its include flipped; the block in "The regression test" is that file, while the
evidence logs below and the 2026-10-29 narration keep the placeholder names (`018-057`,
`test113_`). On the header before the fix (#051 in, scratch copy) the test fails by its check
(`answered 111, errno 115`, the refused phase); on the header as applied it passes on Linux (the
refused connect -1 with `ECONNREFUSED`, the missing unix path -1 with `ENOENT`, the flags kept
with one `F_GETFL` and `O_NONBLOCK` clear after). The whole suite on Windows
`[PASSED: 111]/[FAILED: 0]`, this test stubbed there; on Linux (WSL Ubuntu, g++ 15.2, epoll)
`[PASSED: 111]/[FAILED: 0]`. ASan (gcc, `-O1 -fsanitize=address --param=asan-stack=0`) on a
scratch copy of the applied header: this test, `005-001-io`, `005-003-io_stop_fd` and
`004-001-pool_clear` pass. `docs/02_api.md`'s `connect()` entry carries the new return sentence
(open question 2); the kqueue `connect` is untouched (open question 3); the face is
`ERROR_GENERIC` with the reason in errno (open question 1).

Amended 2026-10-30 by the main session (report 6, item R6-2): the saved flags are restored,
with no second read of them; see the last section.

## The bug

The Linux (epoll) `co::connect` makes the fd nonblocking, calls `::connect`, restores the flags,
and on `EINPROGRESS` parks on an `EPOLLOUT` wait. After the wake it reads the socket's pending
error, `getsockopt(SO_ERROR)`, into `result`, and the wrong step is its failure branch:
`if (result != 0) { COLIB_DEBUG(..., strerror(errno), errno); co_return result; }`. `result` is
the raw errno of the failed connection, a positive number (111 for `ECONNREFUSED`, 110 for
`ETIMEDOUT`, 113 for `EHOSTUNREACH`), and it is returned as the coroutine's value. The library's
failures are negative (`error_e`: `ERROR_GENERIC` -1 and below; 1, 2, 3 are `ERROR_YIELDED`,
`ERROR_SUSPENDED`, `ERROR_FINISHED`), so a caller's `ret < 0`, or the suite's own
`ASSERT_COFN(co_await co::connect(...))`, reads a refused connection as a success, and a
`result` of 1, 2 or 3 would read as one of the three positive statuses. The log line of that
branch printed `strerror(errno)`, which is the errno of the last syscall (`EINPROGRESS` from
`::connect`, since `getsockopt` succeeded), not the connection's error. The Windows `co::connect`
answers a failure with the stopper's error or `ERROR_GENERIC`, the reason in `GetLastError()`;
the Linux side had no such face.

A second, static finding of the same function (report 1's note, folded in): the immediate
failure test `if (res < 0 && errno != EINPROGRESS)` reads `errno` after two `fcntl` calls that
follow `::connect`; a successful call may leave errno unspecified (POSIX), glibc happens to leave
it alone, so the test and the caller's errno held by luck.

Trigger: a TCP connect to a loopback port nobody listens on (report 1's `t07_connect_errno`,
`repro/15-linux_connect_errno.md`'s write-only repro, which could now run).

Confirmed consequence, 2026-10-29 on WSL (Ubuntu, g++ 15.2), regression test below on the
unpatched copy: `co::connect answered 111, errno 115 (Operation now in progress)`, the log line
`FAILED connect: fd: 3 err: Operation now in progress[115]`, and
`CHK_BOOL(test113_ret == co::ERROR_GENERIC)` failed.

## Where the fix lands

| colib.h region                    | what changes                                     | adds/dels |
|-----------------------------------|--------------------------------------------------|-----------|
| the doc comment of the Linux      | the `@return` names the failure face:            | +3/-1     |
| `connect` declaration             | ERROR_GENERIC with the reason in errno, or the   |           |
|                                   | stopper's error                                  |           |
| Linux `co::connect`, after        | the errno of `::connect` is saved right after    | +1/-0     |
| `::connect`                       | the call                                         |           |
| Linux `co::connect`, the          | tests the saved errno; restores it for the       | +3/-2     |
| immediate failure                 | caller and answers ERROR_GENERIC (the same -1,   |           |
|                                   | named)                                           |           |
| Linux `co::connect`, the SO_ERROR | answers ERROR_GENERIC with errno set to the      | +5/-2     |
| failure                           | result; the log prints the result                |           |

Total +12/-5. No members, no signature changes. The kqueue twin of `connect` (the
`#if COLIB_OS_UNIX` block) has the same `co_return result` and is not touched (open question 3).

## The change

`diff -u` of `repro/fix/054/base/colib.h` (untouched copy of today's colib.h) against
`repro/fix/054/colib.h` (the patched copy); `repro/fix/054/054-fix.diff` holds it.

```diff
--- base/colib.h
+++ colib.h
@@ -1799,7 +1799,9 @@
  * @param sa - socket address, same as ::connect
  * @param len - size of socket address, same as ::connect
  * @return **Coroutine** that resolves to: the execution of the function and **further**
- * resolves to the success value of the function, i.e. ERROR_OK for success. */
+ * resolves to the success value of the function, i.e. ERROR_OK for success, ERROR_GENERIC for a
+ * connection that failed, with the reason in errno, or the stopper's error for a stopped wait.
+ * 2026-10-29 */
 inline task_t connect(int fd, sockaddr *sa, socklen_t *len);
 
 /*! @fn
@@ -5228,6 +5230,7 @@
     }
 
     int res = ::connect(fd, sa, len);
+    int connect_errno = errno;  /* the fcntl calls below may leave their own. 2026-10-29 */
 
     if (need_nonblock && ((flags = fcntl(fd, F_GETFL, 0)) < 0)) {
         COLIB_DEBUG("FAILED to get the new flags for the fd[%d] %s[%d]",
@@ -5242,8 +5245,9 @@
 
     /* now that our intention to connect was sent to the os we first check if the connection was
     not done */
-    if (res < 0 && errno != EINPROGRESS) {
-        co_return res;
+    if (res < 0 && connect_errno != EINPROGRESS) {
+        errno = connect_errno;
+        co_return ERROR_GENERIC;
     }
 
     if (res == 0)
@@ -5271,8 +5275,11 @@
     }
 
     if (result != 0) {
-        COLIB_DEBUG("FAILED connect: fd: %d err: %s[%d]", fd, strerror(errno), errno);
-        co_return result;
+        /* a failed connection answers as every failure here does, negative, with the reason left
+        where ::connect leaves it. 2026-10-29 */
+        COLIB_DEBUG("FAILED connect: fd: %d err: %s[%d]", fd, strerror(result), result);
+        errno = result;
+        co_return ERROR_GENERIC;
     }
 
     co_return ERROR_OK;
```

Reasoning, per hunk:

- The doc comment: the declaration's `@return` said only what success looks like; the failure
  face is now part of the contract and is stated where callers read it. This is the comment
  `docs/02_api.md`'s `connect()` entry mirrors (open question 2). The declaration line itself
  (`socklen_t *len`, the report's A10 and the TODO above it) is not touched.
- `connect_errno`: the errno of `::connect` is the one the next test and the caller need; it is
  taken before the two `fcntl` calls that restore the flags, so nothing they do can change it.
- The immediate failure: the `EINPROGRESS` test reads the saved value; the caller gets
  `ERROR_GENERIC` (the same -1 `res` always was, now named) with errno restored to the
  connection's own, instead of whatever the last `fcntl` left.
- The `SO_ERROR` failure: `result` is the connection's errno, so the answer is the library's
  failure value with errno set to it, the face the Windows `connect` already has (its reason in
  `GetLastError()`); the log prints the result, not the stale errno. `errno` is set after the log
  so the log's own calls cannot disturb it, and the awaiter's path (a stopped wait returns the
  stopper's error before this point) is unchanged.

## The regression test

`tests/018-055-reproduced_linux_connect_errno.cpp` (Test111; the number and name assigned at
apply time, 2026-10-30), Reproduced Bugs category. Linux-only, stubbed elsewhere. Two phases on
their own pools. The refused phase is the bug: a listener is bound to `127.0.0.1:0`, listened on,
its port read back and closed, so nobody listens there; a blocking TCP socket is handed to
`co::connect` with that port, and the coroutine reads errno right after the await. It must come
back `ERROR_GENERIC` with `ECONNREFUSED` (pre-fix: 111 and the stale `EINPROGRESS`); a connect
that succeeds because another process took the port in between tries another port, three at
most. The immediate phase pins the saved errno: a unix socket connect to a path that does not
exist fails inside `::connect` with `ENOENT`, before any wait, and must come back `ERROR_GENERIC`
with that errno. The third phase is the amendment's (see the last section): it makes the second
flag read fail and checks the flags are kept. The block below is the file as landed, all three.

Pre-fix: the refused phase fails `CHK_BOOL(test111_ret == co::ERROR_GENERIC)`; the immediate
phase, run first in an evidence copy, passes on the unpatched copy too (glibc's `fcntl` leaves
errno alone on success), so it guards the fold-in rather than reproduces a failure. Post-fix:
passes unchanged, plain and under ASan. The file lands with its include flipped to `../colib.h`.

```cpp
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
```

## How it was checked

Workspace `repro/fix/054/`: `base/colib.h` (untouched copy), `colib.h` (patched),
`tests_common.h`, the test, an evidence copy with the phases swapped
(`018-057-immediate-first.cpp`), the three nearest tests and the standby repro with their include
flipped, `linux.sh` (as in #051), `compose.py`/`compose.sh`/`compose-all/` (item 6), and every
log named below. Linux runs on WSL Ubuntu, g++ 15.2.0; log timestamps cut, long lines wrapped.

1. The test FAILS on the unpatched copy (`018-057-base-plain.log`):

```
colib.h:3350 handle_ready() :> WARNING: unexpected events[[EPOLLOUT|EPOLLERR|EPOLLHUP]] on fd[3]
    with mask[[EPOLLOUT]]
colib.h:5274 connect() :> FAILED connect: fd: 3 err: Operation now in progress[115]
test113_refused() :> refused connect to port 58557: co::connect answered 111, errno 115
    (Operation now in progress)
test113_refused() :> Assert[CHK_BOOL(test113_ret == co::ERROR_GENERIC)] FAILED[ret: -1, errno:
    115[Operation now in progress]]
[FAILED]: 018-057-reproduced_linux_connect_errno.cpp
exit=255
```

   The immediate phase, run first in the evidence copy (`018-057-base-immediate-first.log`),
   passes on the unpatched copy (`co::connect answered -1, errno 2 (No such file or directory)`)
   and the refused phase then fails as above: the saved-errno hunk guards against a libc that
   does not keep errno across a successful `fcntl`; this libc does.

2. The test PASSES on the patched copy, plain (`018-057-patched-plain.log`) and under ASan
   (`018-057-patched-asan.log`):

```
colib.h:5280 connect() :> FAILED connect: fd: 3 err: Connection refused[111]
test113_refused() :> refused connect to port 53795: co::connect answered -1, errno 111
    (Connection refused)
test113_immediate() :> connect to a missing unix path: co::connect answered -1, errno 2 (No such
    file or directory)
[PASSED]: 018-057-reproduced_linux_connect_errno.cpp
exit=0
```

3. No-regression, the three nearest tests against the patched copy on WSL (`*-patched.log`;
   `005-001-io.cpp` is the suite's one user of the Linux `co::connect`): `005-001-io.cpp`
   [PASSED] exit 0, `005-003-io_stop_fd.cpp` [PASSED] exit 0, `004-001-pool_clear.cpp` [PASSED]
   exit 0.

4. Windows: the test builds against the patched copy with the spec's `cl` line and its stub
   passes (`not supported on this OS`, `[PASSED]`, exit 0). The changed function is inside
   `#if COLIB_OS_LINUX`; the doc comment hunk is inside `#if COLIB_OS_LINUX || COLIB_OS_UNIX`
   and is a comment.

5. The standby repro `repro/15-linux_connect_errno.cpp`, include flipped, run against both copies
   (`15-base.log`, `15-patched.log`). Its probe shows the wake fork this kernel takes, and its
   verdict holds on the base and is refuted on the patched copy; the regression test supersedes
   it:

```
PROBE: poll() rc 1, revents 0x001d: POLLIN POLLOUT POLLERR POLLHUP
PROBE: SO_ERROR 111 (Connection refused)
base:    co::connect returned: 111  dbg_enum: [ERROR_UNKNOWN]
         FAIL-REPRO: co::connect co_returned the positive SO_ERROR errno (111, ...)   exit=1
patched: co::connect returned: -1  dbg_enum: ERROR_GENERIC
         PASS: co::connect returned a negative error_e (-1, ERROR_GENERIC) ...        exit=0
```

6. Composition: `repro/fix/054/compose-all/colib.h` is today's header with #051, #052, #053 and
   #054 applied together (built by `compose.py` from the four workspaces: #053's copy of the
   batch, #051's wake rule on top of it, #052's `remove_waiter`, this fix's `connect` and doc
   comment; `compose.diff` is its diff against the base). `compose.sh` runs the four regression
   tests of the round (#052's under ASan) and the three nearest tests against it. Every one
   passes (`compose-all/compose-run.log`):

```
018-054-reproduced_epoll_hup_wakes_nobody.cpp: [PASSED] exit=0
018-055-reproduced_epoll_remove_waiter_bookkeeping.cpp: [PASSED] exit=0      (ASan build)
018-056-reproduced_epoll_batch_pop_back.cpp: [PASSED] exit=0
018-057-reproduced_linux_connect_errno.cpp: [PASSED] exit=0
005-001-io.cpp: [PASSED] exit=0
005-003-io_stop_fd.cpp: [PASSED] exit=0
004-001-pool_clear.cpp: [PASSED] exit=0
```

   The merged diff has eight hunks: the four fixes' own, with #051's and #053's merged into one
   (`@@ -3336,24 +3338,34 @@`), the text FIX_053's Interactions shows.

NOT-CHECKED: the kqueue `connect` (`#if COLIB_OS_UNIX`), which carries the same `co_return
result`, is not built on this box and is not changed (open question 3).

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: every connection that fails after `EINPROGRESS`         |
|                             | (refused, timed out, unreachable); `ASSERT_COFN(co_await     |
|                             | co::connect(...))` read it as success                        |
| DESIGN.md                   | the killer is untouched; a kill or stop at the connect's     |
|                             | wait returns the stopper's error before the changed lines,   |
|                             | as before                                                    |
| epoll / kqueue              | Linux fix, validated on WSL; the Windows `connect` already   |
|                             | had this face and is untouched; the kqueue twin NOT-CHECKED  |
|                             | and not changed (open question 3)                            |
| contract / docs             | the `@return` of the Linux `connect` declaration (hunk 1);   |
|                             | `docs/02_api.md`'s `connect()` entry mirrors that comment    |
|                             | and needs the same sentence at apply time (open question 2)  |
| callers                     | no caller inside colib.h; in the suite, `005-001-io.cpp`     |
|                             | (passes); every caller that tested `< 0` now sees the        |
|                             | failure                                                      |
| pending fixes               | none in this function; the declaration's `socklen_t *len`    |
|                             | TODO (report 1's A10) sits on the line below the changed     |
|                             | comment and is not touched; #051 is upstream (the wake)      |
| runs                        | fails before (111), passes after, plain and ASan, on WSL;    |
|                             | the Windows stub builds and passes; the suites were not run  |
|                             | (the main session runs them)                                 |

## Interactions

- #051 (hangups wake every waiter): upstream of this function. A refused connect delivers
  `EPOLLOUT | EPOLLERR | EPOLLHUP` (the base log above), so the `EPOLLOUT` waiter woke before
  #051 and wakes after it; this fix only changes what the woken coroutine answers. Independent;
  both run together in item 6.
- #052, #053, #25: other functions, no interaction.
- Report 1's A10 (the declared-but-undefined `connect(int, sockaddr*, socklen_t*)` overload and
  the TODO on it): the doc comment this fix edits belongs to that declaration; the declaration is
  left as it is, so whatever A10's resolution is (one signature, matching the definitions) keeps
  the new `@return` text.
- Report 2's R2-6 (format-argument counts): the `COLIB_DEBUG` line this fix rewrites has two
  specifiers and two arguments, before and after.

## Open questions for the user

1. The failure face: `ERROR_GENERIC` with errno set to the connection's error, as the Windows
   `connect` does with `GetLastError()`, and as the briefing asked. The alternative, `-result`
   (a negative errno, carrying the reason in the value), collides with `error_e`: `-ENOENT` is
   `ERROR_TIMEO`, `-ESRCH` is `ERROR_WAKEUP`, `-EINTR` is `ERROR_USER`, `-EIO` is `ERROR_DEPEND`.
   The diff takes the first; it is a contract, so it is yours.
2. The doc comment change and its mirror: the `@return` sentence is in the diff; `docs/02_api.md`
   quotes that comment under `connect()` and needs the same sentence at apply time (I may not
   write under `docs/`). Say if you would rather keep the comment as it was and leave the face to
   the code.
3. The kqueue `connect` has the same two lines (`co_return result` and the stale-errno log) and
   the same errno-after-fcntl read. The backend is WIP, so this proposal leaves it; the same three
   hunks apply there verbatim if you want it done in the same pass.
4. The immediate failure now reads `co_return ERROR_GENERIC` instead of `co_return res`: the same
   value (-1), named. Stated so the change is visible; nothing observable moves.

## Amendment of 2026-10-30: the flags are restored from what was saved (report 6, item R6-2)

`connect` read the socket's flags a second time after `::connect` (an `F_GETFL`) and gave up on
that call's failure without restoring the flags it had saved at the start, so a blocking socket
was left non-blocking, against the doc ("left blocking or non-blocking, as it was"); the
fault-injection campaign showed it with the third `fcntl` on the socket failing. The second read
was never needed: the flags to put back are the saved ones. The amendment restores those, in one
`F_SETFL`, on every path after `::connect`; a failure of that restore is still reported, as
before (nothing else can be done for it). The epoll `connect` only: the kqueue twin has the
same two blocks and stays as it is, like the rest of that WIP backend in this proposal. The
delta on this proposal's patched copy (repro/fix/054/before_amend.colib.h is the copy as
proposed):

```diff
--- patched/colib.h (as proposed)
+++ patched/colib.h (amended)
@@ -5232,12 +5232,9 @@
     int res = ::connect(fd, sa, len);
     int connect_errno = errno;  /* the fcntl calls below may leave their own. 2026-10-29 */
 
-    if (need_nonblock && ((flags = fcntl(fd, F_GETFL, 0)) < 0)) {
-        COLIB_DEBUG("FAILED to get the new flags for the fd[%d] %s[%d]",
-                fd, strerror(errno), errno);
-        co_return ERROR_GENERIC;
-    }
-    if (need_nonblock && (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0)) {
+    /* the flags saved above are put back, whatever ::connect answered: the fd is left as it was.
+    2026-10-30 */
+    if (need_nonblock && (fcntl(fd, F_SETFL, flags) < 0)) {
         COLIB_DEBUG("FAILED to toggle off the O_NONBLOCK flag on fd[%d] %s [%d]",
                 fd, strerror(errno), errno);
         co_return ERROR_GENERIC;
```

The regression test gained a third phase (Linux): the test defines `fcntl()` itself, which the
executable's definition wins for the whole process (the real one through `dlsym`), and fails the
second `F_GETFL` on the socket under test with EBADF; a blocking socket connects to a closed
loopback port. On the copy as proposed the connect answers ERROR_GENERIC from that failure and
the socket is left non-blocking; on the amended copy there is no second read, the connect runs to
its refusal (ERROR_GENERIC, errno ECONNREFUSED) and the flags are as they were. On the copy as
proposed:

```
WARNING: unexpected events[[EPOLLOUT|EPOLLERR|EPOLLHUP]] on fd[3] with mask[[EPOLLOUT]]
FAILED connect: fd: 3 err: Connection refused[111]
refused connect to port 38035: co::connect answered -1, errno 111 (Connection refused)
connect to a missing unix path: co::connect answered -1, errno 2 (No such file or directory)
FAILED to get the new flags for the fd[3] Bad file descriptor[9]
flags kept: ret -1 errno 9, F_GETFL calls 2, O_NONBLOCK after 1
Assert[CHK_BOOL(flags >= 0 && (flags & O_NONBLOCK) == 0)] FAILED[ret: -1, errno: 9[Bad file descriptor]]
[FAILED]: 018-057-reproduced_linux_connect_errno.cpp
exit=255
```

On the amended copy:

```
WARNING: unexpected events[[EPOLLOUT|EPOLLERR|EPOLLHUP]] on fd[3] with mask[[EPOLLOUT]]
FAILED connect: fd: 3 err: Connection refused[111]
refused connect to port 44891: co::connect answered -1, errno 111 (Connection refused)
connect to a missing unix path: co::connect answered -1, errno 2 (No such file or directory)
WARNING: unexpected events[[EPOLLOUT|EPOLLERR|EPOLLHUP]] on fd[3] with mask[[EPOLLOUT]]
FAILED connect: fd: 3 err: Connection refused[111]
flags kept: ret -1 errno 111, F_GETFL calls 1, O_NONBLOCK after 0
[PASSED]: 018-057-reproduced_linux_connect_errno.cpp
exit=0
```

The two phases as proposed pass unchanged; `005-001-io` passes on the amended copy on Linux.
