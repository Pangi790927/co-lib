# Proposal: an EPOLLERR or EPOLLHUP delivery wakes every waiter of its fd (fix #051)

Status: [APPLIED 2026-10-30 on the user's word]

Apply verification, 2026-10-30: the header applied is byte for byte the proposal's patched copy
(`repro/fix/051/colib.h`; the `colib.h` of the day was identical to `repro/fix/051/base/colib.h`,
so the diff applied at fuzz 0). The test landed as
`tests/018-054-reproduced_epoll_hup_wakes_nobody.cpp` (Test110: the placeholder number and name
stood), its include flipped. On the header before the fix it fails by its check
(`test110_done == 1`, the reader phase; `repro/fix/051/018-054-base-plain.log`); on the header as
applied it passes on Linux (the socket control reads 0, the pipe reader reads 0, the pipe writer
gets -1 with `EPIPE`). The whole suite on Windows `[PASSED: 110]/[FAILED: 0]`, this test stubbed
there; on Linux (WSL Ubuntu, g++ 15.2, epoll) `[PASSED: 110]/[FAILED: 0]`, every file (`018-029`
builds there now and skips at run time). ASan (gcc, `-O1 -fsanitize=address
--param=asan-stack=0`) on a scratch copy of the applied header: this test, `005-003-io_stop_fd`,
`004-001-pool_clear` and `005-001-io` pass. The open questions stand as the proposal states them:
a hangup wake resumes with `ERROR_OK`, the WARNING no longer names the two bits, and
`repro/13-epoll_busy_spin.cpp` is left as it was.

## The bug

The epoll `io_pool_t::handle_ready()` takes one `epoll_wait` batch and, for each delivered fd,
queues the waiters whose own mask meets the delivered events: `if (w.mask & events)`. epoll
reports `EPOLLERR` and `EPOLLHUP` whether or not a waiter asked for them (`epoll_ctl(2)`), and on
a pipe they are often the only bits set: the read end of a pipe whose writer closed with nothing
buffered reports `EPOLLHUP` alone; the full write end of a pipe whose reader closed reports
`EPOLLERR` alone (a write end with room reports `EPOLLOUT | EPOLLERR`, which does meet a writer's
mask, so that case always worked). `co::read` waits `EPOLLIN` and `co::write` waits `EPOLLOUT`.

The wrong step is that loop: nothing meets the delivery, so nothing is queued and `remove_mask`
stays 0; `remove_waiter(fd, 0)` then takes its partial branch (the mask minus nothing is still the
mask), re-`MOD`s the fd to the mask it already had and erases no waiter. The fd stays registered
with its level-triggered condition, the WARNING "unexpected events" line is the only trace, and
`next_task_state()` finds the ready queue empty: `run()` returns `RUN_OK` with the coroutine still
parked. Every further `run()` returns the same way at once: the condition is level-triggered and
never disarmed, so a program that re-drives `run()` in a loop spins on one core (the shape of
`repro/13-epoll_busy_spin.md`), and a program that trusts `RUN_OK` to mean "nothing left to wait
for" ends with the reader never having seen its EOF.

Trigger, documented usage: `co::read` on a pipe whose writer closed (any child's output read over
a pipe reaches it at EOF), or `co::write` on a pipe whose reader closed while the pipe is full.

Confirmed consequence, 2026-10-29 on WSL (Ubuntu, g++ 15.2), with the regression test below on the
unpatched copy: the reader phase fails `CHK_BOOL(test110_done == 1)` right after `run()` returned
`RUN_OK`, the log carrying `WARNING: unexpected events[[EPOLLHUP]] on fd[3] with mask[[EPOLLIN]]`;
the writer phase, run first in an evidence copy, fails the same way with
`unexpected events[[EPOLLERR]] on fd[4] with mask[[EPOLLOUT]]`. Report 1 item A1 found the same
(its `t01_pipe_hup`, `t02_write_err`).

## Where the fix lands

| colib.h region                     | what changes                                    | adds/dels |
|------------------------------------|-------------------------------------------------|-----------|
| epoll `io_pool_t::handle_ready()`, | a delivery carrying EPOLLERR or EPOLLHUP wakes  | +7/-2     |
| the loop over the batch            | every waiter of that fd (`wakes` covers all     |           |
|                                    | masks); the WARNING no longer calls those two   |           |
|                                    | bits unexpected                                 |           |

No members, no new state, no public declaration or doc comment changes. `remove_waiter` is
untouched: with every waiter woken, `remove_mask` equals the fd's whole mask, so the existing
full-removal branch (`EPOLL_CTL_DEL` + `ret_evs.pop_back()`) takes the fd out, which is also what
disarms the level-triggered condition.

## The change

`diff -u` of `repro/fix/051/base/colib.h` (untouched copy of today's colib.h) against
`repro/fix/051/colib.h` (the patched copy); `repro/fix/051/051-fix.diff` holds it.

```diff
--- base/colib.h
+++ colib.h
@@ -3346,14 +3346,19 @@
             }
 
             uint32_t events = ret_evs[i].events;
-            if (~data->mask & events) {
+            if (~(data->mask | EPOLLERR | EPOLLHUP) & events) {
                 COLIB_DEBUG("WARNING: unexpected events[%s] on fd[%d] with mask[%s]",
                         dbg_epoll_events(events).c_str(), fd, dbg_epoll_events(data->mask).c_str());
             }
 
+            /* EPOLLERR and EPOLLHUP come whether or not a waiter asked for them, and on a pipe end
+            whose peer closed they can be the only bits. Both are conditions of the open file, not
+            of one direction, so they end every wait on the fd: the syscall each waiter runs reports
+            the condition (a read of 0, an EPIPE, a socket error). 2026-10-29 */
+            uint32_t wakes = (events & (EPOLLERR | EPOLLHUP)) ? ~0u : events;
             uint32_t remove_mask = 0;
             for (auto &w : data->waiters) {
-                if (w.mask & events) {
+                if (w.mask & wakes) {
                     w.state->err = ERROR_OK;
                     ready_tasks.push_back(w.state);
                     remove_mask |= w.mask;
```

Reasoning, one hunk in two parts:

- The wake rule. `EPOLLERR` and `EPOLLHUP` are properties of the open file description, not of a
  direction: after either, no wait on that fd can ever be satisfied the way it was asked (the
  kernel will not report `EPOLLIN` on a hung-up empty pipe, nor `EPOLLOUT` on a full one with no
  reader), and every syscall on it returns at once with the condition. So the delivery ends every
  wait of the fd, and each woken waiter runs its own syscall, which reports what happened in the
  syscall's own terms: `read` returns 0, `write` returns -1 with `EPIPE`, a socket call returns
  the pending `SO_ERROR`. This is what the report's verified fix does and I checked it is the
  right shape for the "other direction" too: a reader woken by a socket's `EPOLLERR` reads the
  error, a writer woken by a pipe's `EPOLLHUP` cannot exist (a pipe end is one direction), and a
  writer woken by a socket's `EPOLLHUP` gets `EPIPE`. The waiter's `err` stays `ERROR_OK`, as
  for any satisfied wait, because the wait did end and the syscall is the channel for the
  condition (open question 1).
- The bookkeeping. `remove_mask` is the union of the woken waiters' masks; with all of them woken
  it equals `data->mask` (add_waiter refuses overlapping masks, so the fd's mask is exactly that
  union), and `remove_waiter` takes its full-removal branch: `set_data(fd, nullptr)`,
  `EPOLL_CTL_DEL`, `ret_evs.pop_back()`. That is the correct end state: the fd leaves the set, so
  the level-triggered condition cannot come back on the next `epoll_wait`, and the fd's data is
  gone, so a later `co::read` on the same fd registers afresh (and is woken again at once, by the
  same condition, which is right: the condition persists). Nothing in `remove_waiter` changes.
- The WARNING. It flagged every delivered bit outside the fd's mask; `EPOLLERR` and `EPOLLHUP`
  are now handled, so the line no longer names them unexpected. Log-only.

## The regression test

`tests/018-054-reproduced_epoll_hup_wakes_nobody.cpp` (Test110; the placeholder number and name
stood at apply time, 2026-10-30), Reproduced Bugs category. Linux-only, stubbed on the other
platforms as the landed Windows-only tests are stubbed on Linux. Three phases, each on its own
pool: a socket control (a stream peer's close delivers `EPOLLIN` with its hangup, so its reader
always woke: the fix must keep that), then the two pipe phases of the bug: the read end of a pipe
whose writer closed (`EPOLLHUP` alone) must come back with 0, and the full write end of a pipe
whose reader closed (`EPOLLERR` alone; the test fills the pipe with nonblocking writes until
`EAGAIN` first) must come back with -1 and `EPIPE`, with `run()` returning `RUN_OK` and the
coroutine done both times. `SIGPIPE` is ignored so the `EPIPE` comes back as a value.

Pre-fix: the reader phase fails `CHK_BOOL(test110_done == 1)` (and the writer phase would fail the
same assertion; the ASSERT chain stops at the first, the evidence copy below ran it first).
Post-fix: passes unchanged, plain and under ASan. Both runs are in "How it was checked". The file
lands with the one include line flipped to `../colib.h`; the workspace copy includes `colib.h`.

```cpp
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
```

## How it was checked

Workspace `repro/fix/051/`: `base/colib.h` (untouched copy), `colib.h` (patched),
`tests_common.h`, the test, the three nearest tests with their include flipped, `linux.sh` (copies
the chosen header, `tests_common.h` and one test into a `mktemp -d /tmp/...` directory, builds
with the tests' flags `g++ -std=c++2a -O3 -g -Wno-format-security -I.`, ASan variant
`-fsanitize=address -O1 -g --param=asan-stack=0`, runs under `timeout 30`), and every log named
below. Linux runs are on WSL Ubuntu, g++ 15.2.0, kernel 6.18 (WSL2).

1. The test FAILS on the unpatched copy (`018-054-base-plain.log`; the timestamps and file
   prefixes of the log lines are cut here, long lines are wrapped):

```
colib.h:3350 handle_ready() :> WARNING: unexpected events[[EPOLLIN|EPOLLHUP]] on fd[3]
    with mask[[EPOLLIN]]
test110_socket_eof_control() :> socket, peer closed: read returned 0 (control)
colib.h:3350 handle_ready() :> WARNING: unexpected events[[EPOLLHUP]] on fd[3] with mask[[EPOLLIN]]
test110_read_end_hup() :> Assert[CHK_BOOL(test110_done == 1)] FAILED[ret: -1, errno: 0[Success]]
[FAILED]: 018-054-reproduced_epoll_hup_wakes_nobody.cpp
exit=255
```

   The writer phase, run first in an evidence copy of the test with the phase order swapped
   (`018-054-phase2-first.cpp`, `018-054-base-phase2-first.log`), fails the same way:

```
colib.h:3350 handle_ready() :> WARNING: unexpected events[[EPOLLERR]] on fd[4] with mask[[EPOLLOUT]]
test110_write_end_err() :> Assert[CHK_BOOL(test110_done == 1)] FAILED[ret: -1, errno: 11[Resource
    temporarily unavailable]]
exit=255
```

2. The test PASSES on the patched copy, plain (`018-054-patched-plain.log`) and under ASan
   (`018-054-patched-asan.log`):

```
test110_socket_eof_control() :> socket, peer closed: read returned 0 (control)
test110_read_end_hup() :> pipe read end, writer closed: read returned 0, errno 0
test110_write_end_err() :> pipe write end, reader closed: write returned -1, errno 32
[PASSED]: 018-054-reproduced_epoll_hup_wakes_nobody.cpp
exit=0
```

3. No-regression, the three nearest tests against the patched copy on WSL (include flipped,
   nothing else; `*-patched.log`): `005-003-io_stop_fd.cpp` [PASSED] exit 0,
   `004-001-pool_clear.cpp` [PASSED] exit 0, `005-001-io.cpp` [PASSED] exit 0.

4. Windows: the test builds against the patched copy with
   `cl -nologo -EHsc -await:strict -std:c++20 -Zi -FS -I. ... -link User32.lib ws2_32.lib` and
   its stub passes (`not supported on this OS`, `[PASSED]`, exit 0). The changed region is inside
   `#if COLIB_OS_LINUX`, so Windows compiles none of it.

5. The standby repro `repro/13-epoll_busy_spin.cpp`, include flipped, run against both copies
   (`13-base.log`, `13-patched.log`): PASS on both, `co::write failed promptly with -1 (errno 32:
   Broken pipe) after 1 run() returns`. Its trigger is not this bug: it writes to a pipe with
   room, and the kernel reports `EPOLLOUT | EPOLLERR` for a write end with room and no reader, so
   the writer's `EPOLLOUT` is met and it wakes on the unpatched header too. The spin it describes
   is real but needs the full pipe (this test's writer phase) or the hung-up read end (its reader
   phase); the repro's reading of `pipe_poll` missed the `EPOLLOUT` bit. The repro file is
   superseded by the regression test.

6. The standby repro `repro/20-epoll_close_awaited_fd.md` is a different mechanism: an fd closed
   under the pool leaves epoll silently (the kernel drops the registration at the last close, no
   event is ever delivered), so no delivery reaches `handle_ready` and this fix does not touch it.
   Its stop_fd-after-close sub-scenario is fix #052's hang; its close-without-stop hang is the
   documented misuse (`stop_fd` before `close`). It is run under #052.

NOT-CHECKED: the kqueue `io_pool_t` (the `#if COLIB_OS_UNIX` block) is a stub that wakes by the
event's `udata` and has no mask matching; nothing of this fix applies to it, and it is WIP. The
Windows IOCP backend is completion-based, with no readiness mask; no analogue exists there.

| check                       | result                                                       |
|-----------------------------|--------------------------------------------------------------|
| documented usage reaches it | yes: `co::read` on a pipe whose writer closed (EOF over a    |
|                             | pipe, a child's output), `co::write` on a full pipe whose    |
|                             | reader closed; the test's two pipe phases                    |
| DESIGN.md                   | the killer is untouched: a kill at an io wait goes through   |
|                             | `force_awake`, not `handle_ready`; rule 3 holds, a wait      |
|                             | ended by a hangup is a wait that did its work, the target    |
|                             | runs on to its next wait                                     |
| epoll / kqueue              | epoll fix, validated on WSL; Windows IOCP has no readiness   |
|                             | mask and no analogue; kqueue NOT-CHECKED (WIP stub, no mask  |
|                             | matching to fix)                                             |
| contract / docs             | no declaration or doc comment changes; `read`/`write` docs   |
|                             | already promise the syscall's return value, which is what a  |
|                             | 0 or an EPIPE is; `wait_event` now wakes with ERROR_OK on a  |
|                             | hangup it did not ask for (open question 1); nothing in      |
|                             | 02_api.md to mirror unless question 1 wants a line           |
| callers                     | `handle_ready` is called from `next_task_state` only (the    |
|                             | blocking and the polling form); the waiters it wakes come    |
|                             | from `read`, `write`, `accept`, `connect`, `wait_event`, and |
|                             | the timerfd of `sleep` (never ERR/HUP); all read             |
| pending fixes               | #25 (clear(), another function), #052 (`remove_waiter`,      |
|                             | which this now reaches on its full branch for every hangup), |
|                             | #053 (adjacent lines of the same loop; the merged text is in |
|                             | #053's Interactions)                                         |
| runs                        | fails before (both pipe phases), passes after, plain and     |
|                             | ASan, on WSL; the Windows stub builds and passes; the suites |
|                             | were not run (the main session runs them at apply time)      |

## Interactions

- #052 (`remove_waiter` bookkeeping on an `epoll_ctl` failure): composes, no shared lines. After
  this fix every hangup takes the full-removal branch of `remove_waiter`; a `DEL` cannot fail
  there (the fd was alive at `epoll_wait`, and no user code runs between the wait and the `DEL`),
  but if it ever did, #052 keeps the tables consistent. Apply in either order.
- #053 (`handle_ready` reads `ret_evs` past its end): the same loop. #053 changes the loop header
  and the two reads of `ret_evs[i]`, this fix changes the WARNING condition and the waiter match,
  lines apart but inside one diff context. #053's Interactions carries the merged text of the
  loop with both applied. Apply in either order; the second one needs the merged text.
- #25 (two-phase `io_pool_t::clear()`): a different function; `clear()` never goes through
  `handle_ready`. Independent.
- Fix #054 (Linux `connect` errno) sits downstream: a refused connect delivers `EPOLLOUT |
  EPOLLERR | EPOLLHUP` (#054's base run logs `unexpected events[[EPOLLOUT|EPOLLERR|EPOLLHUP]] on
  fd[3] with mask[[EPOLLOUT]]`), so its waiter woke before this fix as well; with this fix it
  wakes on the error bits too. Independent.
- Report 2's R2-6 (the `COLIB_DEBUG` lines of `add_waiter`/`remove_waiter` with one argument
  fewer than specifiers): not touched here; the WARNING line this fix reconditions has its
  arguments right.

## Open questions for the user

1. The contract this fix states: a wait ended by `EPOLLERR` or `EPOLLHUP` resumes its coroutine
   with `ERROR_OK`, as a satisfied wait, and the syscall the coroutine then runs reports the
   condition. For `read`/`write`/`accept`/`connect` that is the natural face (0, -1/EPIPE, the
   socket error). For `wait_event`, a caller that asked for `EPOLLIN` now wakes with `ERROR_OK` on
   a hangup it did not ask for and cannot tell the two apart: the awaiter has no channel for the
   delivered events. Is `ERROR_OK` the answer you want there, or should a hangup-only wake carry
   a distinct code (there is none today; `ERROR_GENERIC` would call an EOF an error), or should
   `wait_event` learn to report the delivered events? This fix decides nothing beyond `ERROR_OK`.
2. The WARNING line no longer names `EPOLLERR`/`EPOLLHUP` as unexpected (they are handled). A
   log-only change; say the word if you want the line kept as it was.
3. `repro/13-epoll_busy_spin.cpp` passes on both copies because its trigger has room in the pipe
   (item 5 above); the regression test carries the real triggers. Whether the repro file is kept,
   corrected or removed is yours; nothing under `repro/` was changed.
