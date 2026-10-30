# Proposal: remove_waiter() finishes its tables whatever epoll_ctl answered (fix #052)

Status: [APPLIED 2026-10-30 on the user's word]

Apply verification, 2026-10-30: applied on the header with fixes #051 and #054 in; the diff
(`repro/fix/052/052-fix.diff`, which matches `base/colib.h` against the patched copy) applied at
fuzz 0 with a seven-line offset, and the applied header differs from the patched copy by #051's
and #054's hunks alone. The test landed as
`tests/018-056-reproduced_epoll_remove_waiter_bookkeeping.cpp` (Test112: the placeholder
018-055/Test111 had gone to #054's test), renumbered, its include flipped; the block in "The
regression test" is that file, while the evidence files under `repro/fix/052/` keep the
placeholder name. On the header before the fix (scratch copies) the landed test dies in the
first phase's teardown (a segmentation fault plain; under ASan the heap-use-after-free in
`state_access_t::unlink` under `close_wait` under `io_pool_t::clear()`, the block freed by
`read`), and the hang-first evidence copy trips its watchdog (`TIMED OUT after 15s while 'hang
phase: run()'`); on the header as applied it passes on Linux (the reader woken with
`ERROR_WAKEUP`, the stop answered `ERROR_GENERIC`, the writer ended once at teardown, `run()`
back with `RUN_OK` after the `stop_fd`). The whole suite on Windows `[PASSED: 112]/[FAILED: 0]`,
this test stubbed there; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 112]/[FAILED: 0]`.
ASan (gcc, `-O1 -fsanitize=address --param=asan-stack=0`) on a scratch copy of the applied
header: this test, `005-003-io_stop_fd`, `004-001-pool_clear` and `005-001-io` pass. Applied as
proposed: an `epoll_ctl` failure still answers `ERROR_GENERIC` (open question 1 stands, with the
2026-10-30 note at the end on its weight), no debug check at the `EBADF` (question 2), no doc
change (question 3).

## The bug

The epoll `io_pool_t` keeps three things in step with the kernel's epoll set: one `fd_data_t`
per registered fd (its waiters and the union of their masks), one slot of `ret_evs` per
registered fd (the vector doubles as `epoll_wait`'s output buffer and as "is anything registered":
`handle_ready()` returns at once when it is empty, and otherwise blocks in `epoll_wait(-1)`), and
the kernel's own registration. `io_pool_t::remove_waiter()` is the one place that takes waiters
out, and it returned on an `epoll_ctl` failure before finishing its own bookkeeping, in both of
its branches:

- Full removal (the fd keeps no other events): `set_data(fd, nullptr)` drops the fd's data, then
  `epoll_ctl(EPOLL_CTL_DEL)`, and on its failure `return ERROR_GENERIC` before
  `ret_evs.pop_back()`. The fd is out of epoll and out of the tables, yet `ret_evs` keeps a slot.
  Once every coroutine is done, `next_task_state()` sees an empty ready queue, `handle_ready()`
  no longer takes its "nothing to wait on" exit, and `epoll_wait(-1)` blocks on an empty set:
  `run()` never returns (report 2, R2-1).
- Partial removal (the fd keeps other events): `data->mask` is updated, then
  `epoll_ctl(EPOLL_CTL_MOD)`, and on its failure `return ERROR_GENERIC` before the woken waiters
  are erased from `fd_data_t::waiters`. `force_awake()` had already queued them with the stop's
  error, so they resume, finish, and their frames are freed, while the table still names them;
  `io_pool_t::clear()` later walks the stale entry: `close_wait()` reads a freed coroutine
  frame (report 3, R3-1).

The `epoll_ctl` failure both need is `EBADF`: an fd closed under the pool, which the kernel
already dropped from the set at the close. That order (close, then stop) is the one the `stop_fd`
doc warns against, but the obvious recovery a user tries after a close is exactly `stop_fd`, and
the library answered it with a silent hang, or, for a partial stop, with a dangling pointer at
teardown. The tables are the library's own promise: they must mirror the set whatever the kernel
answered, and the failure can still be reported.

Trigger (a): a coroutine parked in `co::read` on a pipe, another closes the pipe's read end and
`co_await`s `stop_fd` on it; `run()` must come back once both are done. Trigger (b): a reader
(`EPOLLIN`) and a writer (`EPOLLOUT`, the socket's send buffer filled first) parked on one socket;
a third coroutine closes the socket, stops `EPOLLIN` alone, lets the woken reader finish, and
force-stops the pool; the pool is destroyed.

Confirmed consequence, 2026-10-29 on WSL (Ubuntu, g++ 15.2), regression test below on the
unpatched copy: (a) `TIMED OUT after 15s while 'hang phase: run()' - run() did not come back`
(the test's watchdog); (b) under ASan, `heap-use-after-free ... READ of size 8` in
`state_access_t::unlink` under `close_wait` under `io_pool_t::clear()` under `~pool_internal_t`,
the block `freed by ... read colib.h:5307` (the reader's `co::read` frame); in the plain build the
same phase ends in a segmentation fault.

## Where the fix lands

| colib.h region                     | what changes                                    | adds/dels |
|------------------------------------|-------------------------------------------------|-----------|
| epoll `io_pool_t::remove_waiter()` | an `epoll_ctl` failure is recorded instead of   | +8/-3     |
| (private)                          | returned: the waiters are erased, or the fd's   |           |
|                                    | `ret_evs` slot popped, whatever it answered;    |           |
|                                    | the failure is returned at the end; a comment   |           |
|                                    | states the rule                                 |           |

No members, no new state, no public declaration or doc comment changes. The two `COLIB_DEBUG`
lines are kept byte for byte (they carry report 2's R2-6 argument-count defect; that is a
separate fix and these lines are left for it).

## The change

`diff -u` of `repro/fix/052/base/colib.h` (untouched copy of today's colib.h) against
`repro/fix/052/colib.h` (the patched copy); `repro/fix/052/052-fix.diff` holds it.

```diff
--- base/colib.h
+++ colib.h
@@ -3489,12 +3489,17 @@
     }
 
 private:
+    /* The tables mirror the epoll set, so they drop the waiters, or the fd with its slot in
+    ret_evs, whatever epoll_ctl answered: an fd closed under the pool is already out of the set
+    (EBADF or ENOENT), and a table that kept its slot would make the pool wait on nothing, or walk
+    a waiter that no longer exists. The failure is still reported. 2026-10-29 */
     error_e remove_waiter(const io_desc_t& io_desc) {
         auto data = get_data(io_desc.fd);
         if (!data) {
             COLIB_DEBUG("FAILED attempted to remove an inexisting waiter fd[%d]", io_desc.fd);
             return ERROR_GENERIC;
         }
+        error_e err = ERROR_OK;
         if ((data->mask & ~io_desc.events) != 0) {
             struct epoll_event ev;
             ev.events = data->mask & ~io_desc.events;
@@ -3504,7 +3509,7 @@
             if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_MOD, io_desc.fd, &ev)) < 0) {
                 COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_MOD fd[%d] events%s err:%s[%d] -> ret:%d",
                         io_desc.fd, dbg_epoll_events(ev.events).c_str(), strerror(errno), ret);
-                return ERROR_GENERIC;
+                err = ERROR_GENERIC;
             }
             data->waiters.erase(
                 std::remove_if(
@@ -3523,11 +3528,11 @@
             if ((ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, io_desc.fd, NULL)) < 0) {
                 COLIB_DEBUG("FAILED epoll_ctl EPOLL_CTL_DEL fd[%d] err:%s[%d] -> ret:%d",
                         io_desc.fd, strerror(errno), ret);
-                return ERROR_GENERIC;
+                err = ERROR_GENERIC;
             }
             ret_evs.pop_back();
         }
-        return ERROR_OK;
+        return err;
     }
```

Reasoning, three hunks of one shape:

- The comment and `err`: the function's rule, stated where it holds. One local carries the
  answer to the end, so neither branch can leave before its last step.
- The partial branch: a failed `MOD` is recorded and the erase of the woken waiters runs anyway.
  `data->mask` was already narrowed before the call (base behavior, kept), so after the erase the
  mask and the waiter list agree again: the remaining waiters are exactly the ones the mask names.
  What the kernel holds for the fd is whatever it holds (nothing, after a close); that is the
  user's misuse to recover from, and the table no longer names a frame that is gone.
- The full branch: a failed `DEL` is recorded and the slot is popped anyway. The data was already
  dropped before the call (base behavior, kept), so after the pop the three counts agree: no
  data, no slot, and nothing the kernel will ever deliver for that fd. `handle_ready()` takes its
  "nothing to wait on" exit as soon as the last registered fd leaves, by whatever route.

The return value is unchanged on every path (`ERROR_GENERIC` on a failed `epoll_ctl`, as before),
so `stop_fd` after a close still answers -1 and `clear()` still returns early on it, as the base
did (open question 1 is the alternative).

## The regression test

`tests/018-056-reproduced_epoll_remove_waiter_bookkeeping.cpp` (Test112; the number and name
assigned at apply time), Reproduced Bugs category. Linux-only, stubbed elsewhere. It defines
`COLIB_DISABLE_ALLOCATOR true` so the coroutine frames come from malloc and AddressSanitizer can
see the stale entry (with the bucket allocator the freed frame stays intact inside its slab, and
the same read passes unseen). Two phases, the use-after-free first, then the hang, each on its own
pool; a 15 s watchdog thread ends the process with a message when `run()` does not come back.

- Use-after-free phase: a reader and a writer park on one unix socket (the writer after the
  test filled the send buffer with nonblocking writes until `EAGAIN`), a third coroutine closes
  the socket, awaits `stop_io({fd, EPOLLIN})`, yields so the woken reader runs to its end and
  frees its frame, then force-stops the pool. `run()` must return `RUN_STOPPED`, the reader must
  have woken with `ERROR_WAKEUP`, the stop must have answered `ERROR_GENERIC` (the `MOD` failed,
  `EBADF`), the writer must not have resumed; the pool's teardown must then end the writer
  exactly once (a frame marker counts its destructor) and read no freed frame.
- Hang phase: a reader parks on a pipe, a second coroutine closes the read end and awaits
  `stop_fd` on it. `run()` must return `RUN_OK` with the reader woken by `ERROR_WAKEUP` and the
  stop answered `ERROR_GENERIC` (the `DEL` failed, `EBADF`).

Pre-fix: the use-after-free phase aborts under ASan (plain build: a segmentation fault), and the
hang phase, run first in an evidence copy, trips the watchdog. Post-fix: passes unchanged, plain
and under ASan. The file lands with its include flipped to `../colib.h`.

```cpp
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
```

## How it was checked

Workspace `repro/fix/052/`: `base/colib.h` (untouched copy), `colib.h` (patched),
`tests_common.h`, the test, an evidence copy with the phases swapped
(`018-055-hang-first.cpp`), the three nearest tests with their include flipped, `linux.sh` (as
in #051: a `mktemp -d /tmp/...` build with the tests' flags, ASan `-fsanitize=address -O1 -g
--param=asan-stack=0`, `timeout 30`), and every log named below. Linux runs on WSL Ubuntu, g++
15.2.0. The log lines below have their timestamps cut and are wrapped at 100 columns.

1. The test FAILS on the unpatched copy. Plain build (`018-055-base-plain.log`): the
   use-after-free phase reads the freed frame and the process dies there:

```
linux.sh: line 19:   806 Segmentation fault         timeout 30 ./x.bin
exit=139
```

   ASan build (`018-055-base-asan.log`), the same phase:

```
==832==ERROR: AddressSanitizer: heap-use-after-free on address 0x6d8e331e02e0 ...
READ of size 8 at 0x6d8e331e02e0 thread T0
    #0 in colib::state_access_t::unlink(colib::state_t*) colib.h:2586
    #1 in colib::close_wait(colib::state_t*) colib.h:2619
    #2 in colib::io_pool_t::clear() colib.h:3460
    #3 in colib::pool_internal_t::clear() colib.h:4941
    #4 in colib::pool_internal_t::~pool_internal_t() colib.h:4216
freed by thread T0 here:
    #1 in read colib.h:5307
previously allocated by thread T0 here:
    #1 in colib::read(int, void*, unsigned long) colib.h:5295
exit=1
```

   The hang phase, run first in the evidence copy (`018-055-base-hang-first.log`):

```
colib.h:3524 remove_waiter() :> FAILED epoll_ctl EPOLL_CTL_DEL fd[3] err:Bad file descriptor[-1]
    -> ret:-173159376
colib.h:3449 force_awake() :> FAILED remove_waiter in force_awake fd[3] events[EPOLLIN]
018-055-reproduced_epoll_remove_waiter_bookkeeping.cpp: TIMED OUT after 15s while 'hang phase:
    run()' - run() did not come back
exit=1
```

2. The test PASSES on the patched copy, plain (`018-055-patched-plain.log`) and under ASan
   (`018-055-patched-asan.log`), the same lines in both:

```
colib.h:3510 remove_waiter() :> FAILED epoll_ctl EPOLL_CTL_MOD fd[3] events[EPOLLOUT] err:Bad file
    descriptor[-1] -> ret:-751398356
colib.h:3449 force_awake() :> FAILED remove_waiter in force_awake fd[3] events[EPOLLIN]
colib.h:3529 remove_waiter() :> FAILED epoll_ctl EPOLL_CTL_DEL fd[3] err:Bad file descriptor[-1]
    -> ret:3449
colib.h:3464 clear() :> FAILED to remove awaiter: fd:3
colib.h:4947 clear() :> WARNING: FAILED to clear events waiting for io
test111_uaf_phase() :> partial stop after a close: reader woke with -3, stop answered -1, the
    teardown ended the writer once
colib.h:3529 remove_waiter() :> FAILED epoll_ctl EPOLL_CTL_DEL fd[3] err:Bad file descriptor[-1]
    -> ret:1935763568
colib.h:3449 force_awake() :> FAILED remove_waiter in force_awake fd[3] events[EPOLLIN]
test111_hang_phase() :> stop_fd after a close: run() came back RUN_OK, reader woke with -3, stop
    answered -1
[PASSED]: 018-055-reproduced_epoll_remove_waiter_bookkeeping.cpp
exit=0
```

   (The `ret:` garbage in the `epoll_ctl` lines is report 2's R2-6, the missing `errno`
   argument; untouched here.)

3. No-regression, the three nearest tests against the patched copy on WSL (`*-patched.log`):
   `005-003-io_stop_fd.cpp` [PASSED] exit 0, `004-001-pool_clear.cpp` [PASSED] exit 0,
   `005-001-io.cpp` [PASSED] exit 0.

4. Windows: the test builds against the patched copy with the spec's `cl` line and its stub
   passes (`not supported on this OS`, `[PASSED]`, exit 0). The changed function is inside
   `#if COLIB_OS_LINUX`.

5. The standby repro `repro/20-epoll_close_awaited_fd.cpp`, include flipped, run against both
   copies (`20-base.log`, `20-patched.log`): identical output on both. Its sub-scenario 1 is
   this fix's hang trigger (close, then `stop_fd`), but the repro escapes the pool with
   `force_stop` after a 700 ms sleep instead of letting it run dry, so it never meets the hang;
   it records `stop_fd ... returned -1`, the victim woken with -3, `run()` RUN_STOPPED, on both
   copies. Sub-scenarios 2 (the closed number reused by a fresh pipe while the stale `fd_data`
   blocks it) and 3 (a close with no stop at all: `run()` blocks forever, its watchdog at 10 s)
   are the close-without-stop misuse, which this fix does not touch: no stop runs there, so
   `remove_waiter` is never reached. The repro is superseded by the regression test for
   sub-scenario 1; 2 and 3 document the misuse.

NOT-CHECKED: the kqueue `io_pool_t` (`#if COLIB_OS_UNIX`) has no `remove_waiter`; its
`force_awake` and `clear` are empty stubs (WIP). The Windows IOCP `io_pool_t` keeps its tables
in `dequeue_data`, which has no kernel call that can fail between the table steps; no analogue.

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | no: both triggers need an fd closed under the pool, which    |
|                             | `stop_fd`'s doc tells the user not to do; the fix keeps the  |
|                             | library's own tables right under that misuse (no hang, no    |
|                             | dangling waiter) and still reports it; no library code for   |
|                             | the misuse itself (open question 2)                          |
| DESIGN.md                   | the killer is untouched; a kill at an io wait goes through   |
|                             | `force_awake` -> `remove_waiter`, whose answer and whose     |
|                             | table effect on a healthy fd are unchanged; rules 1 and 3    |
|                             | hold as before                                               |
| epoll / kqueue              | epoll fix, validated on WSL; IOCP has no analogue; kqueue    |
|                             | NOT-CHECKED (WIP stubs)                                      |
| contract / docs             | none: every return value is as before; `stop_fd` after a     |
|                             | close still answers ERROR_GENERIC (open question 1 is the    |
|                             | alternative); nothing in 02_api.md to mirror                 |
| callers                     | `remove_waiter` is called from `handle_ready` (every wake),  |
|                             | `force_awake` (every stop and kill) and `clear()` (every     |
|                             | teardown); all three read: none of them looks at the tables  |
|                             | after a failure in a way the finished bookkeeping breaks;    |
|                             | `clear()` returns on the failure as before                   |
| pending fixes               | #25 (clear(), calls it, no shared lines), #051 and #053      |
|                             | (handle_ready, no shared lines)                              |
| runs                        | fails before (the use-after-free under ASan and as a plain   |
|                             | segfault; the hang in the evidence copy), passes after,      |
|                             | plain and ASan, on WSL; the Windows stub builds and passes;  |
|                             | the suites were not run (the main session runs them)         |

## Interactions

- #25 (two-phase `io_pool_t::clear()`, pending): composes, no shared lines, either order. #25
  changes `clear()` only: it copies each fd's waiter states out, calls
  `remove_waiter(fd, 0xffffffff)` per fd, and ends the copied states afterwards. This fix changes
  `remove_waiter`'s body only. They do not subsume each other: with #25 alone, the stale entry of
  trigger (b) is copied out and ended all the same (the same use-after-free, one function later);
  with this fix alone, `clear()`'s own erase-under-iteration stays. With both, trigger (b)'s
  teardown ends the writer once and reads nothing stale, as this test requires. One seam they
  share: `clear()` returns `ERROR_GENERIC` on the first fd whose `DEL` fails (an fd closed under
  the pool), leaving the later fds' waiters unended; #25 keeps that return where it was, this
  fix keeps the error that triggers it. That is report 1's A2 (a leak at teardown), not assigned
  in this round; open question 1 is the smallest change that would let `clear()` go on.
- #051 (hangups wake every waiter): calls this function on its full branch for every hangup;
  no shared lines. A `DEL` cannot fail there on a healthy fd. Either order.
- #053 (`handle_ready` reads `ret_evs` past its end): the pops this fix makes unconditional are
  the pops #053 walks around; #053 copies the batch and does not touch `remove_waiter`. No
  shared lines, either order. Together, every pop happens and none is read past.
- Report 2's R2-6 (the two `COLIB_DEBUG` lines here print `ret` where `errno` is meant): the
  lines are kept byte for byte so that fix applies on top without a merge.
- Fix #03 (removed 2026-10-13) was the kill-after-close shape of the same misuse on the kill
  path; it changed the killer's answer and was taken out. This fix changes no answer and no
  caller, only the table steps after a failed `epoll_ctl`.

## Open questions for the user

1. Should an `epoll_ctl` failure that means "already gone" (`EBADF`, `ENOENT`) be answered
   `ERROR_OK`? The diff keeps `ERROR_GENERIC` on every failure, so the misuse is still reported:
   `stop_fd` after a close answers -1 (the two `stop_ret == ERROR_GENERIC` lines of the test pin
   that), `force_awake` and `handle_ready` return it, and `clear()` still returns at the first
   such fd (A2's leak stays as it is). Answering `ERROR_OK` for those two errnos would make
   `stop_fd` after a close a clean no-op and let `clear()` walk every fd; it is a contract change
   for `stop_fd`/`stop_io`, so it is yours. The test's two lines flip with it.
2. Close-before-stop is a misuse; this fix adds no diagnostic for it. A check behind
   `COLIB_ENABLE_DEBUG_CHECKS` at the `EBADF` (e.g. an assert naming the order the doc asks for)
   is possible; not added unless you want it.
3. No doc change: `stop_fd`'s comment already says not to close an fd before removing it from the
   pool. If you want the comment to also say what happens when one does (the stop answers an
   error, the pool stays consistent), that line is a one-liner at apply time.

Note added 2026-10-30 by the main session (report 6, item R6-3, repro/reports/2026-10-30-report-6.md):
the fault-injection sweep shows the weight of open question 1. With this fix as proposed the
bookkeeping stays consistent but the failure is still answered ERROR_GENERIC, `handle_ready` returns
it, and `next_task_state()` ends the run with RUN_ERRORED: in the sweep one `EPOLL_CTL_DEL` failing
with EBADF at the first event (one fd closed while registered) ended `run()` with all 46 coroutines
alive and nothing done, the woken ones left queued. One mishandled descriptor takes the whole pool
down. The report's answer is the question's: EBADF/ENOENT read as "already gone" (the fd's data
dropped, its waiters woken with an error, the loop going on), RUN_ERRORED kept for the poll's own
descriptor. Fix #097 documents what the next run() does after RUN_ERRORED, which holds with this
fix applied (the bookkeeping is consistent) and not before (R2-1's hang).
